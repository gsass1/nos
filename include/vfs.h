#ifndef __VFS_H__
#define __VFS_H__

#include <stdint.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_CHARDEVICE  0x03
#define FS_BLOCKDEVICE 0x04
#define FS_PIPE        0x05
#define FS_SYMLINK     0x06
#define FS_MOUNTPOINT  0x08 // Is the file an active mountpoint?

struct dirent
{
    char name[128];
    uint32_t inode;
};

struct fs_node;

typedef uint32_t (*read_fsnode_t)(struct fs_node *, uint32_t, uint32_t, uint8_t *);
typedef uint32_t (*write_fsnode_t)(struct fs_node *, uint32_t, uint32_t, uint8_t *);
typedef void (*open_fsnode_t)(struct fs_node *);
typedef void (*close_fsnode_t)(struct fs_node *);
// readdir fills the CALLER's dirent (returns 0, or -1 past the end). A
// returned pointer to filesystem-internal storage would be overwritten by a
// concurrent reader under preemption before the caller copies it out.
typedef int (*readdir_fsnode_t)(struct fs_node *, uint32_t, struct dirent *);
typedef struct fs_node *(*finddir_fsnode_t)(struct fs_node *, char *);

// Optional filesystem operations (fs_node `ops`, may be NULL). Kept in a
// side table so basic filesystems don't have to fill a wall of pointers.
struct fs_ops
{
    struct fs_node *(*create)(struct fs_node *dir, const char *name);
    int (*mkdir)(struct fs_node *dir, const char *name);
    int (*truncate)(struct fs_node *node);
};

struct fs_node
{
    char name[128];     // The filename.
    uint32_t mask;        // The permissions mask.
    uint32_t uid;         // The owning user.
    uint32_t gid;         // The owning group.
    uint32_t flags;       // Includes the node type. See #defines above.
    uint32_t inode;       // This is device-specific - provides a way for a filesystem to identify files.
    uint32_t length;      // Size of the file, in bytes.
    uint32_t impl;        // An implementation-defined number.
    struct fs_ops *ops;   // Optional create/mkdir/truncate table, or NULL.
    read_fsnode_t read;
    write_fsnode_t write;
    open_fsnode_t open;
    close_fsnode_t close;
    readdir_fsnode_t readdir;
    finddir_fsnode_t finddir;
    void *ptr; // Mountpoint: mounted root fs_node. ext2: struct ext2_mount* context.
};

extern struct fs_node *fs_root;

uint32_t vfs_read(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_write(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
void vfs_open(struct fs_node *node, uint8_t read, uint8_t write);
void vfs_close(struct fs_node *node);
// Fill *out with the index'th entry of the directory. Returns 0 on success,
// -1 past the end (or if the node is not a listable directory).
int vfs_readdir(struct fs_node *node, uint32_t index, struct dirent *out);
struct fs_node *vfs_finddir(struct fs_node *node, char *name);

// Hierarchical path resolution: walk '/'-separated components from fs_root,
// following mountpoints (nodes with FS_MOUNTPOINT set, ptr = mounted root).
// Returns the resolved node, or 0 if any component is not found.
struct fs_node *vfs_resolve(const char *path);

// Create a file named `name` in directory `dir`. Returns the new node, or 0
// if the directory does not support creation.
struct fs_node *vfs_create(struct fs_node *dir, const char *name);

// Truncate a file node to zero length (free all data blocks).
// Returns 0 on success, -1 on failure (e.g. unsupported indirect pointers).
int vfs_truncate(struct fs_node *node);

// Create a directory named `name` in directory `dir`. Returns 0 on success,
// -1 if the directory does not support mkdir or the operation fails.
int vfs_mkdir(struct fs_node *dir, const char *name);

#endif
