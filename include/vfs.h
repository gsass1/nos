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
typedef struct dirent *(*readdir_fsnode_t)(struct fs_node *, uint32_t);
typedef struct fs_node *(*finddir_fsnode_t)(struct fs_node *, char *);

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
    read_fsnode_t read;
    write_fsnode_t write;
    open_fsnode_t open;
    close_fsnode_t close;
    readdir_fsnode_t readdir;
    finddir_fsnode_t finddir;
    struct fs_node *ptr; // Used by mountpoints and symlinks.
};

extern struct fs_node *fs_root;

uint32_t vfs_read(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t vfs_write(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
void vfs_open(struct fs_node *node, uint8_t read, uint8_t write);
void vfs_close(struct fs_node *node);
struct dirent *vfs_readdir(struct fs_node *node, uint32_t index);
struct fs_node *vfs_finddir(struct fs_node *node, char *name);

#endif