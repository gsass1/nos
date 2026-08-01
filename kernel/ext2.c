// ext2 revision-0 read/write filesystem for NOS.
//
// Mounts a tightly-validated, feature-free ext2 filesystem from a block
// device (the first ATA disk, see main.c). Supports directory listing and
// lookup, regular-file reads through direct and singly-indirect blocks, and
// create/truncate/write of regular files in existing directories. All
// metadata/data mutations are serialized by a per-mount mutex; reads also
// take the lock so a concurrent writer cannot race the block cache.
//
// Intentional limitations: 1 KiB blocks, one block group, revision 0, no
// incompatible or read-only-compatible features. No unlink, rmdir, symlinks,
// multi-group writes, or journaling.
#include <block.h>
#include <ext2.h>
#include <kernel.h>
#include <mm.h>
#include <mutex.h>
#include <string.h>
#include <vfs.h>

MODULE("EXT2");

#define EXT2_MAGIC      0xEF53
#define EXT2_REV_OLD    0
#define EXT2_ROOT_INO   2
#define EXT2_FIRST_NORMAL_INO 11

#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFLNK    0xA000
#define EXT2_S_IFMT     0xF000

// Maximum blocks a single file can use through direct + singly-indirect:
// 12 + 256 = 268 blocks = 268 KiB at 1 KiB block size.
#define EXT2_MAX_FILE_BLOCKS 268

// --- On-disk structures (packed, little-endian) ----------------------------

struct ext2_superblock
{
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // Revision-1+ fields (present on disk but zero for rev 0):
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
} __attribute__((packed));

struct ext2_group_desc
{
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

struct ext2_inode
{
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;   // in 512-byte sectors
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; // 0-11 direct, 12 singly-indirect, 13-14 deeper
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

// Directory entry header (variable-length record, name follows at offset 8).
struct ext2_dirent
{
    uint32_t inode;
    uint16_t rec_len;
    uint16_t name_len;  // 16-bit without filetype feature (we require none)
} __attribute__((packed));

// --- Mount context ----------------------------------------------------------

// The node cache stores one pointer per validated inode (max 8192 = 32 KiB,
// since s_inodes_count <= s_inodes_per_group <= 8192 is required at mount).
// fs_node structs are allocated lazily on first lookup and reused thereafter.

struct ext2_mount
{
    struct block_device *dev;
    struct ext2_superblock sb;
    struct ext2_group_desc gd;
    uint32_t block_size;        // always 1024
    uint32_t sectors_per_block; // always 2
    uint32_t inode_size;        // always 128 (rev 0)
    uint32_t inode_table_block; // first block of the inode table
    uint32_t inode_table_blocks;// number of blocks the inode table spans
    uint32_t first_data_block;  // first usable data block after all metadata
    uint32_t alloc_start_bit;   // first bit to search in the block bitmap
    uint32_t sb_block;          // block holding the superblock
    uint32_t gd_block;          // block holding the group descriptor
    struct mutex lock;          // serializes all access (read and write)
    struct fs_node *root_node;
    // Per-mount inode->fs_node cache. One pointer per inode (up to 8192).
    // Indexed by inode number - 1. Repeated lookups return the same node so
    // we don't leak fs_node allocations on every open/resolve.
    struct fs_node **node_cache;
    uint32_t node_cache_size;
};

// --- Static dirent buffer (like initrd's single-entry approach) -------------

static struct dirent s_dirent;

// --- Filesystem operations tables (stored via fs_node->impl) ----------------

static struct fs_ops ext2_dir_ops;
static struct fs_ops ext2_file_ops;

// Forward declarations for VFS callbacks.
static struct dirent *ext2_readdir(struct fs_node *node, uint32_t index);
static struct fs_node *ext2_finddir(struct fs_node *node, char *name);
static uint32_t ext2_read_file(struct fs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer);
static uint32_t ext2_write_file(struct fs_node *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static struct fs_node *ext2_create(struct fs_node *dir, const char *name);
static int ext2_mkdir(struct fs_node *dir, const char *name);
static int ext2_truncate(struct fs_node *node);

// --- Block range validation -------------------------------------------------

// A block number is in the filesystem if it falls within [first_data_block,
// blocks_count). Raw variant for use during mount before the mount context
// exists.
static int ext2_blk_in_range_raw(struct ext2_superblock *sb, uint32_t blk)
{
    return blk >= sb->s_first_data_block && blk < sb->s_blocks_count;
}

static int ext2_blk_in_range(struct ext2_mount *m, uint32_t blk)
{
    return ext2_blk_in_range_raw(&m->sb, blk);
}

// A data pointer must be 0 (sparse hole) or a valid data block at or above
// first_data_block — never a metadata block (superblock, GD, bitmaps, inode
// table). This prevents a corrupt inode from directing reads/writes into
// filesystem metadata.
static int ext2_data_blk_ok(struct ext2_mount *m, uint32_t blk)
{
    return blk == 0 || (blk >= m->first_data_block &&
                        blk < m->sb.s_blocks_count);
}

// Check every data pointer in an inode before I/O. Rejects inodes with
// doubly/triply-indirect pointers (i_block[13], i_block[14]) set, since this
// driver does not handle them and freeing them would leak.
static int ext2_inode_pointers_ok(struct ext2_mount *m, struct ext2_inode *in)
{
    for (uint32_t i = 0; i < 12; i++) {
        if (!ext2_data_blk_ok(m, in->i_block[i])) return 0;
    }
    if (!ext2_data_blk_ok(m, in->i_block[12])) return 0;
    // Doubly/triply-indirect blocks must be zero — this driver can't handle them.
    if (in->i_block[13] != 0 || in->i_block[14] != 0) return 0;
    return 1;
}

// --- Block I/O helpers ------------------------------------------------------

static int ext2_read_block(struct ext2_mount *m, uint32_t blk, void *buf)
{
    if (!ext2_blk_in_range(m, blk)) return -1;
    return block_read(m->dev, blk * m->sectors_per_block,
                      m->sectors_per_block, buf);
}

static int ext2_write_block(struct ext2_mount *m, uint32_t blk, const void *buf)
{
    if (!ext2_blk_in_range(m, blk)) return -1;
    return block_write(m->dev, blk * m->sectors_per_block,
                       m->sectors_per_block, buf);
}

// --- Superblock / group descriptor persistence ------------------------------

static int ext2_write_superblock(struct ext2_mount *m)
{
    uint8_t *buf = kmalloc(m->block_size);
    if (!buf) return -1;
    if (ext2_read_block(m, m->sb_block, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf, &m->sb, sizeof(m->sb));
    int r = ext2_write_block(m, m->sb_block, buf);
    kfree(buf);
    return r;
}

static int ext2_write_group_desc(struct ext2_mount *m)
{
    uint8_t *buf = kmalloc(m->block_size);
    if (!buf) return -1;
    if (ext2_read_block(m, m->gd_block, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf, &m->gd, sizeof(m->gd));
    int r = ext2_write_block(m, m->gd_block, buf);
    kfree(buf);
    return r;
}

// --- Inode I/O --------------------------------------------------------------

static int ext2_read_inode(struct ext2_mount *m, uint32_t ino,
                           struct ext2_inode *out)
{
    if (ino < 1 || ino > m->sb.s_inodes_count) return -1;
    uint32_t idx = ino - 1;
    uint32_t off = idx * m->inode_size;
    uint32_t blk = m->inode_table_block + off / m->block_size;
    uint32_t in_block = off % m->block_size;

    // The inode must fit within the inode table's validated span.
    if (blk >= m->inode_table_block + m->inode_table_blocks) return -1;

    uint8_t *buf = kmalloc(m->block_size);
    if (!buf) return -1;
    if (ext2_read_block(m, blk, buf) < 0) { kfree(buf); return -1; }
    memcpy(out, buf + in_block, sizeof(*out));
    kfree(buf);
    return 0;
}

static int ext2_write_inode(struct ext2_mount *m, uint32_t ino,
                            const struct ext2_inode *in)
{
    if (ino < 1 || ino > m->sb.s_inodes_count) return -1;
    uint32_t idx = ino - 1;
    uint32_t off = idx * m->inode_size;
    uint32_t blk = m->inode_table_block + off / m->block_size;
    uint32_t in_block = off % m->block_size;

    if (blk >= m->inode_table_block + m->inode_table_blocks) return -1;

    uint8_t *buf = kmalloc(m->block_size);
    if (!buf) return -1;
    if (ext2_read_block(m, blk, buf) < 0) { kfree(buf); return -1; }
    memcpy(buf + in_block, in, sizeof(*in));
    int r = ext2_write_block(m, blk, buf);
    kfree(buf);
    return r;
}

// --- Block / inode allocation -----------------------------------------------

// Allocate a free block. Returns the block number, or 0 on failure (no space
// or I/O error). Does not underflow free counters. Starts searching at
// alloc_start_bit so a malformed clear metadata bit can never allocate a
// metadata block.
static uint32_t ext2_alloc_block(struct ext2_mount *m)
{
    if (m->sb.s_free_blocks_count == 0 || m->gd.bg_free_blocks_count == 0)
        return 0;

    uint8_t *bm = kmalloc(m->block_size);
    if (!bm) return 0;
    if (ext2_read_block(m, m->gd.bg_block_bitmap, bm) < 0) { kfree(bm); return 0; }

    // Only search bits from alloc_start_bit up to the last data block.
    uint32_t total = m->sb.s_blocks_count - m->sb.s_first_data_block;
    for (uint32_t i = m->alloc_start_bit; i < total; i++) {
        if (!(bm[i / 8] & (1 << (i % 8)))) {
            uint32_t blk = i + m->sb.s_first_data_block;
            // Final block must be a valid data block.
            if (!ext2_data_blk_ok(m, blk) || blk >= m->sb.s_blocks_count) {
                kfree(bm);
                return 0;
            }
            bm[i / 8] |= (1 << (i % 8));
            int wr = ext2_write_block(m, m->gd.bg_block_bitmap, bm);
            if (wr < 0) {
                kfree(bm);
                return 0;
            }

            // Zero the newly allocated block.
            uint8_t *zero = kmalloc(m->block_size);
            if (!zero) {
                bm[i / 8] &= ~(1 << (i % 8));
                ext2_write_block(m, m->gd.bg_block_bitmap, bm);
                kfree(bm);
                return 0;
            }
            memset(zero, 0, m->block_size);
            int zr = ext2_write_block(m, blk, zero);
            kfree(zero);
            if (zr < 0) {
                bm[i / 8] &= ~(1 << (i % 8));
                ext2_write_block(m, m->gd.bg_block_bitmap, bm);
                kfree(bm);
                return 0;
            }
            kfree(bm);

            m->sb.s_free_blocks_count--;
            m->gd.bg_free_blocks_count--;
            ext2_write_superblock(m);
            ext2_write_group_desc(m);
            return blk;
        }
    }
    kfree(bm);
    return 0;
}

// Free a data block. Validates it is a real data block (> 0, at or above
// first_data_block) before freeing. Does not underflow (checks the bit
// before clearing).
static void ext2_free_block(struct ext2_mount *m, uint32_t blk)
{
    if (blk == 0 || !ext2_data_blk_ok(m, blk)) return;
    uint32_t bit = blk - m->sb.s_first_data_block;

    uint8_t *bm = kmalloc(m->block_size);
    if (!bm) return;
    if (ext2_read_block(m, m->gd.bg_block_bitmap, bm) < 0) { kfree(bm); return; }
    if (bm[bit / 8] & (1 << (bit % 8))) {
        bm[bit / 8] &= ~(1 << (bit % 8));
        if (ext2_write_block(m, m->gd.bg_block_bitmap, bm) < 0) { kfree(bm); return; }
        m->sb.s_free_blocks_count++;
        m->gd.bg_free_blocks_count++;
        ext2_write_superblock(m);
        ext2_write_group_desc(m);
    }
    kfree(bm);
}

// Allocate a free inode. Returns the inode number, or 0 on failure.
static uint32_t ext2_alloc_inode(struct ext2_mount *m)
{
    if (m->sb.s_free_inodes_count == 0 || m->gd.bg_free_inodes_count == 0)
        return 0;

    uint8_t *bm = kmalloc(m->block_size);
    if (!bm) return 0;
    if (ext2_read_block(m, m->gd.bg_inode_bitmap, bm) < 0) { kfree(bm); return 0; }

    uint32_t total = m->sb.s_inodes_count;
    for (uint32_t i = EXT2_FIRST_NORMAL_INO - 1; i < total; i++) {
        if (!(bm[i / 8] & (1 << (i % 8)))) {
            bm[i / 8] |= (1 << (i % 8));
            int wr = ext2_write_block(m, m->gd.bg_inode_bitmap, bm);
            kfree(bm);
            if (wr < 0) return 0;

            m->sb.s_free_inodes_count--;
            m->gd.bg_free_inodes_count--;
            ext2_write_superblock(m);
            ext2_write_group_desc(m);
            return i + 1;
        }
    }
    kfree(bm);
    return 0;
}

// Free an inode (clear its bitmap bit and bump free counts).
static void ext2_free_inode(struct ext2_mount *m, uint32_t ino)
{
    if (ino < 1 || ino > m->sb.s_inodes_count) return;
    uint32_t bit = ino - 1;

    uint8_t *bm = kmalloc(m->block_size);
    if (!bm) return;
    if (ext2_read_block(m, m->gd.bg_inode_bitmap, bm) < 0) { kfree(bm); return; }
    if (bm[bit / 8] & (1 << (bit % 8))) {
        bm[bit / 8] &= ~(1 << (bit % 8));
        if (ext2_write_block(m, m->gd.bg_inode_bitmap, bm) < 0) { kfree(bm); return; }
        m->sb.s_free_inodes_count++;
        m->gd.bg_free_inodes_count++;
        ext2_write_superblock(m);
        ext2_write_group_desc(m);
    }
    kfree(bm);
}

// --- Directory record validation --------------------------------------------

// Validate a directory entry before any compare/copy. Returns 1 if the entry
// is well-formed, 0 if corrupt (caller must stop scanning this block).
static int ext2_dirent_ok(struct ext2_dirent *de, uint32_t off, uint32_t limit)
{
    // rec_len must be >= 8, 4-byte aligned, and within the remaining space.
    if (de->rec_len < 8) return 0;
    if (de->rec_len & 3) return 0;
    if (off + de->rec_len > limit) return 0;
    // name_len must fit within this record (after the 8-byte header) and <= 255.
    if (de->name_len > de->rec_len - 8) return 0;
    if (de->name_len > 255) return 0;
    return 1;
}

// --- File data read (direct + singly-indirect) ------------------------------

static uint32_t ext2_read_data(struct ext2_mount *m, struct ext2_inode *in,
                               uint32_t offset, uint32_t size, uint8_t *buf)
{
    if (offset >= in->i_size) return 0;
    if (offset + size > in->i_size) size = in->i_size - offset;

    uint32_t bs = m->block_size;
    uint32_t ptrs_per_block = bs / 4;
    uint8_t *blk = kmalloc(bs);
    if (!blk) return 0;
    uint8_t *indirect = 0;
    uint32_t got = 0;

    while (got < size) {
        uint32_t foff = offset + got;
        uint32_t bi = foff / bs;
        uint32_t in_blk = foff % bs;
        uint32_t chunk = bs - in_blk;
        if (chunk > size - got) chunk = size - got;

        uint32_t phys = 0;
        if (bi < 12) {
            phys = in->i_block[bi];
        } else if (bi < 12 + ptrs_per_block) {
            if (in->i_block[12]) {
                if (!ext2_data_blk_ok(m, in->i_block[12])) break;
                if (!indirect) {
                    indirect = kmalloc(bs);
                    if (!indirect) break;
                }
                if (ext2_read_block(m, in->i_block[12], indirect) < 0) break;
                phys = ((uint32_t *)indirect)[bi - 12];
            }
        } else {
            break; // doubly/triply indirect not supported
        }

        if (phys == 0) {
            memset(buf + got, 0, chunk); // hole
        } else {
            if (!ext2_data_blk_ok(m, phys)) break;
            if (ext2_read_block(m, phys, blk) < 0) break;
            memcpy(buf + got, blk + in_blk, chunk);
        }
        got += chunk;
    }

    kfree(blk);
    if (indirect) kfree(indirect);
    return got;
}

// --- File data write (allocates blocks as needed) ---------------------------

static uint32_t ext2_write_data(struct ext2_mount *m, uint32_t ino,
                                struct ext2_inode *in,
                                uint32_t offset, uint32_t size,
                                const uint8_t *buf)
{
    uint32_t bs = m->block_size;
    uint32_t ptrs_per_block = bs / 4;

    // Overflow check: offset + size must not wrap.
    if (offset > 0xFFFFFFFFU - size) return 0;
    uint32_t end = offset + size;
    // Capacity check: the write must not exceed direct + singly-indirect.
    uint32_t max_size = EXT2_MAX_FILE_BLOCKS * bs;
    if (end > max_size) return 0;

    uint8_t *blk = kmalloc(bs);
    if (!blk) return 0;
    uint8_t *indirect = 0;
    uint32_t wrote = 0;
    int dirty_indirect = 0;
    int ok = 1;

    while (wrote < size) {
        uint32_t foff = offset + wrote;
        uint32_t bi = foff / bs;
        uint32_t in_blk = foff % bs;
        uint32_t chunk = bs - in_blk;
        if (chunk > size - wrote) chunk = size - wrote;

        uint32_t phys = 0;
        if (bi < 12) {
            phys = in->i_block[bi];
            if (phys == 0) {
                phys = ext2_alloc_block(m);
                if (!phys) { ok = 0; break; }
                in->i_block[bi] = phys;
                in->i_blocks += bs / 512;
            }
        } else if (bi < 12 + ptrs_per_block) {
            if (in->i_block[12] == 0) {
                in->i_block[12] = ext2_alloc_block(m);
                if (!in->i_block[12]) { ok = 0; break; }
                in->i_blocks += bs / 512;
            }
            if (!ext2_data_blk_ok(m, in->i_block[12])) { ok = 0; break; }
            if (!indirect) {
                indirect = kmalloc(bs);
                if (!indirect) { ok = 0; break; }
            }
            if (ext2_read_block(m, in->i_block[12], indirect) < 0) { ok = 0; break; }
            uint32_t *ents = (uint32_t *)indirect;
            phys = ents[bi - 12];
            if (phys == 0) {
                phys = ext2_alloc_block(m);
                if (!phys) { ok = 0; break; }
                ents[bi - 12] = phys;
                dirty_indirect = 1;
                in->i_blocks += bs / 512;
            }
        } else {
            break;
        }

        if (!ext2_data_blk_ok(m, phys)) { ok = 0; break; }
        // Read-modify-write the target block.
        if (ext2_read_block(m, phys, blk) < 0) { ok = 0; break; }
        memcpy(blk + in_blk, buf + wrote, chunk);
        if (ext2_write_block(m, phys, blk) < 0) { ok = 0; break; }
        wrote += chunk;
    }

    // Persist the indirect block if it was modified.
    if (indirect) {
        if (dirty_indirect && ok && ext2_data_blk_ok(m, in->i_block[12])) {
            if (ext2_write_block(m, in->i_block[12], indirect) < 0) {
                ok = 0;
            }
        }
        kfree(indirect);
    }

    // Update i_size and persist the inode. If metadata linkage failed, do not
    // report the bytes as successfully written.
    if (offset + wrote > in->i_size) in->i_size = offset + wrote;
    if (ext2_write_inode(m, ino, in) < 0) {
        ok = 0;
    }
    if (!ok) { kfree(blk); return 0; }

    kfree(blk);
    return wrote;
}

// --- Directory walk helpers -------------------------------------------------

// Find an entry by name; returns the child inode number or 0.
// Reads direct blocks only (sufficient for small dirs in one block group).
static uint32_t ext2_dir_lookup(struct ext2_mount *m, struct ext2_inode *dir,
                                const char *name)
{
    uint32_t bs = m->block_size;
    uint32_t namelen = strlen(name);
    uint8_t *buf = kmalloc(bs);
    if (!buf) return 0;

    // Directory data is in direct blocks; i_size bounds the valid region.
    uint32_t dir_size = dir->i_size;
    for (uint32_t b = 0; b < 12; b++) {
        if (dir->i_block[b] == 0) continue;
        if (!ext2_data_blk_ok(m, dir->i_block[b])) break;
        if (ext2_read_block(m, dir->i_block[b], buf) < 0) break;

        // Only scan up to the remaining directory size.
        uint32_t limit = bs;
        if (b * bs + limit > dir_size) {
            if (dir_size <= b * bs) break;
            limit = dir_size - b * bs;
        }

        uint32_t off = 0;
        while (off + 8 <= limit) {
            struct ext2_dirent *de = (struct ext2_dirent *)(buf + off);
            if (!ext2_dirent_ok(de, off, limit)) break;
            if (de->inode != 0 && de->name_len == namelen) {
                if (memcmp(de + 1, name, namelen) == 0) {
                    kfree(buf);
                    return de->inode;
                }
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
    return 0;
}

// Add a directory entry to *dir_in (inode dir_ino). Allocates a fresh
// directory block if no existing block has space. Returns 0 on success.
static int ext2_dir_add(struct ext2_mount *m, uint32_t dir_ino,
                        struct ext2_inode *dir_in, uint32_t child_ino,
                        const char *name)
{
    uint32_t bs = m->block_size;
    uint32_t namelen = strlen(name);
    if (namelen > 255) return -1;
    uint32_t need = (8 + namelen + 3) & ~3u; // 4-byte aligned rec_len
    uint8_t *buf = kmalloc(bs);
    if (!buf) return -1;

    for (uint32_t b = 0; b < 12; b++) {
        if (dir_in->i_block[b] == 0) {
            // Allocate a fresh block for the directory.
            uint32_t nblk = ext2_alloc_block(m);
            if (!nblk) { kfree(buf); return -1; }
            uint32_t old_blocks = dir_in->i_blocks;
            uint32_t old_size = dir_in->i_size;
            dir_in->i_block[b] = nblk;
            dir_in->i_blocks += bs / 512;
            dir_in->i_size = (b + 1) * bs;

            memset(buf, 0, bs);
            struct ext2_dirent *de = (struct ext2_dirent *)buf;
            de->inode = child_ino;
            de->rec_len = bs; // spans the whole block
            de->name_len = namelen;
            memcpy(de + 1, name, namelen);
            if (ext2_write_block(m, nblk, buf) < 0 ||
                ext2_write_inode(m, dir_ino, dir_in) < 0) {
                dir_in->i_block[b] = 0;
                dir_in->i_blocks = old_blocks;
                dir_in->i_size = old_size;
                ext2_free_block(m, nblk);
                kfree(buf);
                return -1;
            }
            kfree(buf);
            return 0;
        }

        if (!ext2_data_blk_ok(m, dir_in->i_block[b])) break;
        if (ext2_read_block(m, dir_in->i_block[b], buf) < 0) break;

        // Only operate within the directory's valid size.
        uint32_t limit = bs;
        if (b * bs + limit > dir_in->i_size) {
            if (dir_in->i_size <= b * bs) break;
            limit = dir_in->i_size - b * bs;
        }

        uint32_t off = 0;
        while (off + 8 <= limit) {
            struct ext2_dirent *de = (struct ext2_dirent *)(buf + off);
            if (!ext2_dirent_ok(de, off, limit)) break;
            // Is this the last entry in the block?
            if (off + de->rec_len >= limit) {
                uint32_t actual = (8 + de->name_len + 3) & ~3u;
                if (de->rec_len >= actual + need) {
                    // Split: shrink this entry, append ours.
                    uint16_t old = de->rec_len;
                    de->rec_len = actual;
                    struct ext2_dirent *nd =
                        (struct ext2_dirent *)(buf + off + actual);
                    nd->inode = child_ino;
                    nd->rec_len = old - actual;
                    nd->name_len = namelen;
                    memcpy(nd + 1, name, namelen);
                    if (ext2_write_block(m, dir_in->i_block[b], buf) < 0) {
                        kfree(buf);
                        return -1;
                    }
                    kfree(buf);
                    return 0;
                }
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
    return -1; // no space in direct blocks
}

// --- Inode cache ------------------------------------------------------------

// Get or create a cached fs_node for the given inode. Returns 0 on failure.
static struct fs_node *ext2_get_node(struct ext2_mount *m, uint32_t ino,
                                     const char *name, uint16_t mode,
                                     uint32_t size)
{
    if (ino < 1 || ino > m->node_cache_size) return 0;
    uint32_t idx = ino - 1;

    if (m->node_cache[idx]) {
        // Update size in case it changed (e.g. after a write).
        m->node_cache[idx]->length = size;
        return m->node_cache[idx];
    }

    struct fs_node *fn = kmalloc(sizeof(struct fs_node));
    if (!fn) return 0;
    memset(fn, 0, sizeof(*fn));
    if (name) {
        strncpy(fn->name, name, sizeof(fn->name) - 1);
    }
    fn->inode = ino;
    fn->length = size;
    fn->ptr = m;

    if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        fn->flags = FS_DIRECTORY;
        fn->readdir = ext2_readdir;
        fn->finddir = ext2_finddir;
        fn->impl = (uint32_t)&ext2_dir_ops;
    } else {
        fn->flags = FS_FILE;
        fn->read = ext2_read_file;
        fn->write = ext2_write_file;
        fn->impl = (uint32_t)&ext2_file_ops;
    }

    m->node_cache[idx] = fn;
    return fn;
}

// --- VFS callbacks ----------------------------------------------------------

static struct dirent *ext2_readdir(struct fs_node *node, uint32_t index)
{
    struct ext2_mount *m = node->ptr;
    mutex_lock(&m->lock);

    struct ext2_inode din;
    if (ext2_read_inode(m, node->inode, &din) < 0) {
        mutex_unlock(&m->lock);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &din)) {
        mutex_unlock(&m->lock);
        return 0;
    }

    uint32_t bs = m->block_size;
    uint8_t *buf = kmalloc(bs);
    if (!buf) { mutex_unlock(&m->lock); return 0; }
    uint32_t cur = 0;
    struct dirent *result = 0;

    uint32_t dir_size = din.i_size;
    for (uint32_t b = 0; b < 12 && !result; b++) {
        if (din.i_block[b] == 0) continue;
        if (!ext2_data_blk_ok(m, din.i_block[b])) break;
        if (ext2_read_block(m, din.i_block[b], buf) < 0) break;

        uint32_t limit = bs;
        if (b * bs + limit > dir_size) {
            if (dir_size <= b * bs) break;
            limit = dir_size - b * bs;
        }

        uint32_t off = 0;
        while (off + 8 <= limit) {
            struct ext2_dirent *de = (struct ext2_dirent *)(buf + off);
            if (!ext2_dirent_ok(de, off, limit)) break;
            if (de->inode != 0) {
                if (cur == index) {
                    uint32_t nl = de->name_len;
                    if (nl > sizeof(s_dirent.name) - 1)
                        nl = sizeof(s_dirent.name) - 1;
                    memcpy(s_dirent.name, de + 1, nl);
                    s_dirent.name[nl] = '\0';
                    s_dirent.inode = de->inode;
                    result = &s_dirent;
                    break;
                }
                cur++;
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
    mutex_unlock(&m->lock);
    return result;
}

static struct fs_node *ext2_finddir(struct fs_node *node, char *name)
{
    struct ext2_mount *m = node->ptr;
    mutex_lock(&m->lock);

    struct ext2_inode din;
    if (ext2_read_inode(m, node->inode, &din) < 0) {
        mutex_unlock(&m->lock);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &din)) {
        mutex_unlock(&m->lock);
        return 0;
    }

    uint32_t ino = ext2_dir_lookup(m, &din, name);
    if (ino == 0) { mutex_unlock(&m->lock); return 0; }

    struct ext2_inode in;
    if (ext2_read_inode(m, ino, &in) < 0) { mutex_unlock(&m->lock); return 0; }

    uint16_t fmt = in.i_mode & EXT2_S_IFMT;
    if (fmt != EXT2_S_IFDIR && fmt != EXT2_S_IFREG) {
        mutex_unlock(&m->lock);
        return 0; // unsupported type
    }

    struct fs_node *fn = ext2_get_node(m, ino, name, in.i_mode, in.i_size);
    mutex_unlock(&m->lock);
    return fn;
}

static uint32_t ext2_read_file(struct fs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer)
{
    struct ext2_mount *m = node->ptr;
    mutex_lock(&m->lock);
    struct ext2_inode in;
    if (ext2_read_inode(m, node->inode, &in) < 0) {
        mutex_unlock(&m->lock);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &in)) {
        mutex_unlock(&m->lock);
        return 0;
    }
    uint32_t got = ext2_read_data(m, &in, offset, size, buffer);
    mutex_unlock(&m->lock);
    return got;
}

static uint32_t ext2_write_file(struct fs_node *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer)
{
    struct ext2_mount *m = node->ptr;
    mutex_lock(&m->lock);
    struct ext2_inode in;
    if (ext2_read_inode(m, node->inode, &in) < 0) {
        mutex_unlock(&m->lock);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &in)) {
        mutex_unlock(&m->lock);
        return 0;
    }
    uint32_t wrote = ext2_write_data(m, node->inode, &in, offset, size, buffer);
    node->length = in.i_size;
    mutex_unlock(&m->lock);
    return wrote;
}

// Truncate a file to zero length. Returns 0 on success, -1 on failure.
// Rejects inodes with doubly/triply-indirect pointers. Persists the
// zeroed-pointer inode BEFORE freeing the old data blocks, so an inode-write
// failure cannot leave live pointers to freed/reusable blocks. A later
// free-block failure leaks space but cannot alias live files.
static int ext2_truncate(struct fs_node *node)
{
    struct ext2_mount *m = node->ptr;
    mutex_lock(&m->lock);

    struct ext2_inode in;
    if (ext2_read_inode(m, node->inode, &in) < 0) {
        mutex_unlock(&m->lock);
        return -1;
    }
    if (!ext2_inode_pointers_ok(m, &in)) {
        // Corrupt or has doubly/triply-indirect pointers — refuse.
        mutex_unlock(&m->lock);
        return -1;
    }

    // If there is an indirect block, read and validate it now so we can fail
    // before changing anything. Every nonzero child pointer must be a valid
    // data block; a corrupt pointer means we cannot safely free the children.
    uint8_t *ind = 0;
    if (in.i_block[12]) {
        ind = kmalloc(m->block_size);
        if (!ind) {
            mutex_unlock(&m->lock);
            return -1;
        }
        if (ext2_read_block(m, in.i_block[12], ind) < 0) {
            kfree(ind);
            mutex_unlock(&m->lock);
            return -1;
        }
        // Validate every nonzero child pointer before proceeding.
        uint32_t cnt = m->block_size / 4;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t child = ((uint32_t *)ind)[i];
            if (child && !ext2_data_blk_ok(m, child)) {
                kfree(ind);
                mutex_unlock(&m->lock);
                return -1;
            }
        }
    }

    // Save the old block pointers so we can free them after the inode is
    // persisted with zeroed pointers.
    uint32_t old_blocks[12];
    for (uint32_t i = 0; i < 12; i++) {
        old_blocks[i] = in.i_block[i];
        in.i_block[i] = 0;
    }
    uint32_t old_indirect = in.i_block[12];
    in.i_block[12] = 0;
    in.i_size = 0;
    in.i_blocks = 0;

    // Persist the zeroed-pointer inode first. If this fails, the old blocks
    // are still allocated and reachable — no aliasing.
    if (ext2_write_inode(m, node->inode, &in) < 0) {
        kfree(ind);
        mutex_unlock(&m->lock);
        return -1;
    }
    node->length = 0;

    // Now free the saved old blocks. A failure here leaks space but cannot
    // alias live files because the inode no longer points to them.
    for (uint32_t i = 0; i < 12; i++) {
        if (old_blocks[i]) ext2_free_block(m, old_blocks[i]);
    }
    if (old_indirect) {
        uint32_t cnt = m->block_size / 4;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t child = ((uint32_t *)ind)[i];
            if (child && ext2_data_blk_ok(m, child))
                ext2_free_block(m, child);
        }
        ext2_free_block(m, old_indirect);
    }
    if (ind) kfree(ind);

    mutex_unlock(&m->lock);
    return 0;
}

static struct fs_node *ext2_create(struct fs_node *dir, const char *name)
{
    struct ext2_mount *m = dir->ptr;
    mutex_lock(&m->lock);

    struct ext2_inode din;
    if (ext2_read_inode(m, dir->inode, &din) < 0) {
        mutex_unlock(&m->lock);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &din)) {
        mutex_unlock(&m->lock);
        return 0;
    }

    // Reject if the name already exists.
    if (ext2_dir_lookup(m, &din, name) != 0) {
        mutex_unlock(&m->lock);
        return 0;
    }

    uint32_t ino = ext2_alloc_inode(m);
    if (!ino) { mutex_unlock(&m->lock); return 0; }

    // Initialize the new inode as an empty regular file.
    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT2_S_IFREG | 0644;
    in.i_links_count = 1;
    if (ext2_write_inode(m, ino, &in) < 0) {
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return 0;
    }

    // Append the directory entry.
    if (ext2_dir_add(m, dir->inode, &din, ino, name) < 0) {
        // Roll back the inode allocation.
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return 0;
    }

    struct fs_node *fn = ext2_get_node(m, ino, name, EXT2_S_IFREG | 0644, 0);
    mutex_unlock(&m->lock);
    return fn;
}

// Create a directory named `name` in the directory inode behind `dir`.
// Allocates one inode and one data block, initializes `.` and `..` records,
// bumps the parent's link count and the group's used-dirs count, and finally
// inserts the child entry into the parent. Returns 0 on success, -1 on
// failure.
//
// Resource ordering: inode is allocated first, then the data block, then the
// directory block and child inode are initialized. After the child inode is
// persisted, the fallible parent-side count updates happen before the
// directory entry is added: the parent link count is incremented and
// persisted, then bg_used_dirs_count is incremented and persisted. Only then
// is the child entry inserted via ext2_dir_add. If any of these steps fails,
// the count values are restored to their saved originals and the child
// inode/block are freed — so a failed mkdir never leaves a partially
// initialized child, a dangling parent entry, or stale count metadata.
static int ext2_mkdir(struct fs_node *dir, const char *name)
{
    struct ext2_mount *m = dir->ptr;
    mutex_lock(&m->lock);

    // Validate the parent is a directory.
    struct ext2_inode din;
    if (ext2_read_inode(m, dir->inode, &din) < 0) {
        mutex_unlock(&m->lock);
        return -1;
    }
    if (!ext2_inode_pointers_ok(m, &din)) {
        mutex_unlock(&m->lock);
        return -1;
    }
    if ((din.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Validate the name.
    uint32_t namelen = strlen(name);
    if (namelen == 0 || namelen > 255) {
        mutex_unlock(&m->lock);
        return -1;
    }
    // Reject reserved names.
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Reject if the name already exists.
    if (ext2_dir_lookup(m, &din, name) != 0) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Bumping the parent link count must not overflow uint16 (a full or
    // malformed parent at 0xFFFF would wrap to zero, corrupting the count).
    if (din.i_links_count == 0xFFFF) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Bumping bg_used_dirs_count must not overflow the total inode count
    // (it can never legitimately exceed s_inodes_count).
    if (m->gd.bg_used_dirs_count >= m->sb.s_inodes_count) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Allocate the new inode (ext2_alloc_inode skips reserved inodes).
    uint32_t ino = ext2_alloc_inode(m);
    if (!ino) {
        mutex_unlock(&m->lock);
        return -1;
    }

    // Allocate one data block for the directory (ext2_alloc_block only
    // returns validated data blocks and zeroes them).
    uint32_t blk = ext2_alloc_block(m);
    if (!blk) {
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }

    // Build the directory data block: "." and ".." records.
    uint32_t bs = m->block_size;
    uint8_t *dbuf = kmalloc(bs);
    if (!dbuf) {
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }
    memset(dbuf, 0, bs);

    // "." entry: 8-byte header + 1-byte name, padded to 12.
    struct ext2_dirent *dot = (struct ext2_dirent *)dbuf;
    dot->inode = ino;
    dot->rec_len = 12;
    dot->name_len = 1;
    *((uint8_t *)(dot + 1)) = '.';

    // ".." entry: occupies the rest of the block.
    struct ext2_dirent *dotdot = (struct ext2_dirent *)(dbuf + 12);
    dotdot->inode = dir->inode;
    dotdot->rec_len = bs - 12;
    dotdot->name_len = 2;
    *((uint8_t *)(dotdot + 1)) = '.';
    *((uint8_t *)(dotdot + 1) + 1) = '.';

    if (ext2_write_block(m, blk, dbuf) < 0) {
        kfree(dbuf);
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }
    kfree(dbuf);

    // Initialize the new directory inode: mode 0755, links=2 (self + parent
    // entry), size = one block, i_blocks = 2 sectors (1 KiB / 512).
    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT2_S_IFDIR | 0755;
    in.i_links_count = 2;
    in.i_size = bs;
    in.i_blocks = 2;
    in.i_block[0] = blk;

    if (ext2_write_inode(m, ino, &in) < 0) {
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }

    // Persist the parent link-count increment before adding the directory
    // entry. Save the old value so it can be restored on a later failure.
    uint16_t old_parent_links = din.i_links_count;
    din.i_links_count++;
    if (ext2_write_inode(m, dir->inode, &din) < 0) {
        din.i_links_count = old_parent_links;
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }

    // Persist the group's used-dirs-count increment. Save the old value so
    // it can be restored on a later failure.
    uint16_t old_used_dirs = m->gd.bg_used_dirs_count;
    m->gd.bg_used_dirs_count++;
    if (ext2_write_group_desc(m) < 0) {
        m->gd.bg_used_dirs_count = old_used_dirs;
        din.i_links_count = old_parent_links;
        ext2_write_inode(m, dir->inode, &din);
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }

    // Insert the child entry into the parent directory — the last fallible
    // step. If it fails, roll back both count updates and free the child.
    if (ext2_dir_add(m, dir->inode, &din, ino, name) < 0) {
        m->gd.bg_used_dirs_count = old_used_dirs;
        ext2_write_group_desc(m);
        din.i_links_count = old_parent_links;
        ext2_write_inode(m, dir->inode, &din);
        ext2_free_block(m, blk);
        ext2_free_inode(m, ino);
        mutex_unlock(&m->lock);
        return -1;
    }

    // Cache the new directory's fs_node so subsequent lookups find it.
    ext2_get_node(m, ino, name, EXT2_S_IFDIR | 0755, bs);

    mutex_unlock(&m->lock);
    return 0;
}

// --- Mount validation -------------------------------------------------------

struct fs_node *ext2_mount(struct block_device *dev)
{
    if (!dev) return 0;

    // Read the superblock (byte offset 1024 = sector 2 for 512-byte sectors).
    uint8_t sbuf[1024];
    if (block_read(dev, 2, 2, sbuf) < 0) return 0;

    struct ext2_superblock *sb = (struct ext2_superblock *)sbuf;

    if (sb->s_magic != EXT2_MAGIC) return 0;
    if (sb->s_rev_level != EXT2_REV_OLD) return 0;
    if (sb->s_log_block_size != 0) return 0; // require 1 KiB blocks

    // Require all feature masks zero (no compat, incompat, or ro-compat).
    if (sb->s_feature_compat != 0) return 0;
    if (sb->s_feature_incompat != 0) return 0;
    if (sb->s_feature_ro_compat != 0) return 0;

    // Geometry sanity.
    if (sb->s_blocks_count == 0 ||
        sb->s_inodes_count < EXT2_FIRST_NORMAL_INO) return 0;
    if (sb->s_blocks_per_group == 0 || sb->s_inodes_per_group == 0) return 0;
    if (sb->s_first_data_block != 1) return 0; // implied by 1 KiB blocks

    // Fragment geometry: for this subset, frags per group must equal blocks
    // per group (no fragmentation), and log_frag_size must match log_block_size.
    if (sb->s_log_frag_size != sb->s_log_block_size) return 0;
    if (sb->s_frags_per_group != sb->s_blocks_per_group) return 0;

    // Inode count must not exceed inodes per group (one group only).
    if (sb->s_inodes_count > sb->s_inodes_per_group) return 0;

    // Avoid count/size overflow: inode table bytes must not overflow uint32.
    uint32_t inode_size = 128; // rev 0
    if (sb->s_inodes_count > 0xFFFFFFFFU / inode_size) return 0;

    // Filesystem must fit on the block device (sectors = blocks * 2).
    if (sb->s_blocks_count > 0xFFFFFFFFU / 2) return 0; // overflow guard
    uint32_t fs_sectors = sb->s_blocks_count * 2;
    if (fs_sectors > dev->sector_count) return 0;

    // Require exactly one block group.
    if (sb->s_blocks_count > sb->s_first_data_block + sb->s_blocks_per_group)
        return 0;

    // Blocks/inodes per group must fit one 1 KiB bitmap (8192 bits).
    if (sb->s_blocks_per_group > 8192) return 0;
    if (sb->s_inodes_per_group > 8192) return 0;

    // Free counts must be bounded.
    if (sb->s_free_blocks_count > sb->s_blocks_count) return 0;
    if (sb->s_free_inodes_count > sb->s_inodes_count) return 0;

    // Read the group descriptor (block first_data_block + 1 = block 2).
    uint32_t gd_block = sb->s_first_data_block + 1;
    uint8_t gbuf[1024];
    if (block_read(dev, gd_block * 2, 2, gbuf) < 0) return 0;
    struct ext2_group_desc *gd = (struct ext2_group_desc *)gbuf;

    // Validate GD pointers are non-zero and in range.
    if (gd->bg_block_bitmap == 0 || gd->bg_inode_bitmap == 0 ||
        gd->bg_inode_table == 0) return 0;
    if (!ext2_blk_in_range_raw(sb, gd->bg_block_bitmap) ||
        !ext2_blk_in_range_raw(sb, gd->bg_inode_bitmap) ||
        !ext2_blk_in_range_raw(sb, gd->bg_inode_table)) return 0;

    // GD free counts must be bounded.
    if (gd->bg_free_blocks_count > sb->s_blocks_per_group) return 0;
    if (gd->bg_free_inodes_count > sb->s_inodes_per_group) return 0;
    // Used-dirs count must be bounded by total inodes.
    if (gd->bg_used_dirs_count > sb->s_inodes_count) return 0;

    // For one group, superblock and GD free counts must agree.
    if (sb->s_free_blocks_count != gd->bg_free_blocks_count) return 0;
    if (sb->s_free_inodes_count != gd->bg_free_inodes_count) return 0;

    // Validate the inode table span: it must be entirely in range.
    uint32_t inode_table_blocks =
        (sb->s_inodes_count * inode_size + 1024 - 1) / 1024;
    uint32_t inode_table_end = gd->bg_inode_table + inode_table_blocks;
    if (inode_table_end > sb->s_blocks_count) return 0;

    // Require metadata structures to be mutually non-overlapping and not
    // overlap the superblock or GD blocks. Each occupies exactly one block
    // except the inode table, which spans inode_table_blocks blocks.
    // Spans: [sb_block, sb_block+1), [gd_block, gd_block+1),
    //         [bb, bb+1), [ib, ib+1), [it, it+inode_table_blocks).
    uint32_t bb = gd->bg_block_bitmap;
    uint32_t ib = gd->bg_inode_bitmap;
    uint32_t it = gd->bg_inode_table;
    // Helper: two half-open intervals [a, a+na) and [b, b+nb) overlap.
#define SPANS_OVERLAP(a, na, b, nb) \
    ((a) < (b) + (nb) && (b) < (a) + (na))

    if (SPANS_OVERLAP(sb->s_first_data_block, 1, gd_block, 1)) return 0;
    if (SPANS_OVERLAP(sb->s_first_data_block, 1, bb, 1)) return 0;
    if (SPANS_OVERLAP(sb->s_first_data_block, 1, ib, 1)) return 0;
    if (SPANS_OVERLAP(sb->s_first_data_block, 1, it, inode_table_blocks)) return 0;
    if (SPANS_OVERLAP(gd_block, 1, bb, 1)) return 0;
    if (SPANS_OVERLAP(gd_block, 1, ib, 1)) return 0;
    if (SPANS_OVERLAP(gd_block, 1, it, inode_table_blocks)) return 0;
    if (SPANS_OVERLAP(bb, 1, ib, 1)) return 0;
    if (SPANS_OVERLAP(bb, 1, it, inode_table_blocks)) return 0;
    if (SPANS_OVERLAP(ib, 1, it, inode_table_blocks)) return 0;

#undef SPANS_OVERLAP

    // Derive the first usable data block as the maximum end of the GD block,
    // both bitmap blocks, and the full inode table span. This ensures no
    // allocated data block can overlap any metadata structure.
    uint32_t first_data = gd->bg_inode_table + inode_table_blocks;
    if (gd->bg_block_bitmap + 1 > first_data)
        first_data = gd->bg_block_bitmap + 1;
    if (gd->bg_inode_bitmap + 1 > first_data)
        first_data = gd->bg_inode_bitmap + 1;
    if (gd_block + 1 > first_data)
        first_data = gd_block + 1;
    if (first_data > sb->s_blocks_count) return 0;

    // The alloc_start_bit is the bit index of first_data in the block bitmap.
    uint32_t alloc_start_bit = first_data - sb->s_first_data_block;

    // Node cache: one pointer per validated inode. s_inodes_count is already
    // required <= s_inodes_per_group <= 8192, so no separate cap is needed.
    uint32_t cache_size = sb->s_inodes_count;

    // Allocate and populate the mount context.
    struct ext2_mount *m = kmalloc(sizeof(struct ext2_mount));
    if (!m) return 0;
    memset(m, 0, sizeof(*m));
    memcpy(&m->sb, sb, sizeof(m->sb));
    memcpy(&m->gd, gd, sizeof(m->gd));
    m->dev = dev;
    m->block_size = 1024;
    m->sectors_per_block = 2;
    m->inode_size = inode_size;
    m->inode_table_block = gd->bg_inode_table;
    m->inode_table_blocks = inode_table_blocks;
    m->first_data_block = first_data;
    m->alloc_start_bit = alloc_start_bit;
    m->sb_block = sb->s_first_data_block;
    m->gd_block = gd_block;
    m->lock = (struct mutex)MUTEX_INIT;

    // Allocate the inode->node cache (zeroed so all pointers start NULL).
    m->node_cache_size = cache_size;
    m->node_cache = kmalloc(sizeof(struct fs_node *) * cache_size);
    if (!m->node_cache) {
        kfree(m);
        return 0;
    }
    memset(m->node_cache, 0, sizeof(struct fs_node *) * cache_size);

    // Read and validate the root inode (inode 2).
    struct ext2_inode root_in;
    if (ext2_read_inode(m, EXT2_ROOT_INO, &root_in) < 0) {
        kfree(m->node_cache);
        kfree(m);
        return 0;
    }
    if ((root_in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        kfree(m->node_cache);
        kfree(m);
        return 0;
    }
    if (!ext2_inode_pointers_ok(m, &root_in)) {
        kfree(m->node_cache);
        kfree(m);
        return 0;
    }

    // Initialize the fs_ops tables (once).
    ext2_dir_ops.create = ext2_create;
    ext2_dir_ops.mkdir = ext2_mkdir;
    ext2_dir_ops.truncate = 0;
    ext2_file_ops.create = 0;
    ext2_file_ops.truncate = ext2_truncate;

    // Build and cache the root fs_node.
    struct fs_node *root = ext2_get_node(m, EXT2_ROOT_INO, "disk",
                                         root_in.i_mode, root_in.i_size);
    if (!root) {
        kfree(m->node_cache);
        kfree(m);
        return 0;
    }
    // The ext2 root is a regular directory node whose ptr holds the mount
    // context. The initrd root's finddir returns this node for "disk"; no
    // FS_MOUNTPOINT indirection is needed because this node's own
    // readdir/finddir are ext2 functions.
    root->flags = FS_DIRECTORY;
    root->ptr = m;
    m->root_node = root;

    kprintf("ext2: mounted %s (%u blocks, %u inodes, %u free)\n",
            dev->name, m->sb.s_blocks_count, m->sb.s_inodes_count,
            m->sb.s_free_inodes_count);
    return root;
}
