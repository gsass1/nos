#include <kernel.h>
#include <string.h>
#include <vfs.h>

MODULE("VFS ");

struct fs_node *fs_root = 0;

uint32_t vfs_read(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
	mprintf(LOGLEVEL_DEBUG, "vfs_read called\n");
    if(node->read != 0) {
        return node->read(node, offset, size, buffer);
    } else {
		mprintf(LOGLEVEL_DEBUG, "vfs_read failed, node->read = 0\n");
        return 0;
    }
}

uint32_t vfs_write(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    if(node->write != 0) {
        return node->write(node, offset, size, buffer);
    } else {
        return 0;
    }
}

void vfs_open(struct fs_node *node, uint8_t read, uint8_t write)
{
    (void)read;  // access-mode flags: nothing enforces them yet
    (void)write;
    if(node->open != 0) {
        node->open(node);
    }
}

void vfs_close(struct fs_node *node)
{
    if(node->close != 0) {
        node->close(node);
    }
}

struct dirent *vfs_readdir(struct fs_node *node, uint32_t index)
{
    if((node->flags&0x7) == FS_DIRECTORY && node->readdir != 0) {
        return node->readdir(node, index);
    } else {
        return 0;
    }
}

struct fs_node *vfs_finddir(struct fs_node *node, char *name)
{
    if((node->flags & 0x7) == FS_DIRECTORY && node->finddir) {
        return node->finddir(node, name);
    } else {
        return 0;
    }
}

// Follow a mountpoint: if the node has FS_MOUNTPOINT set, return its ptr
// (the mounted root); otherwise return the node itself.
static struct fs_node *vfs_follow(struct fs_node *node)
{
    if (node && (node->flags & FS_MOUNTPOINT) && node->ptr) {
        return (struct fs_node *)node->ptr;
    }
    return node;
}

struct fs_node *vfs_resolve(const char *path)
{
    if (!fs_root || !path) return 0;

    // Skip leading slashes. An empty path (or just "/") resolves to root.
    while (*path == '/') path++;
    if (!*path) return fs_root;

    struct fs_node *node = vfs_follow(fs_root);
    char comp[128];

    for (;;) {
        // Extract the next path component.
        uint32_t i = 0;
        while (*path && *path != '/' && i < sizeof(comp) - 1) {
            comp[i++] = *path++;
        }
        if (*path && *path != '/') {
            return 0; // component exceeds the VFS name limit
        }
        comp[i] = '\0';
        while (*path == '/') path++;

        if (i == 0) break; // trailing slash

        node = vfs_finddir(node, comp);
        if (!node) return 0;
        node = vfs_follow(node);

        if (!*path) break;
    }
    return node;
}

struct fs_node *vfs_create(struct fs_node *dir, const char *name)
{
    if (!dir || (dir->flags & 0x7) != FS_DIRECTORY || !dir->impl) {
        return 0;
    }
    struct fs_ops *ops = (struct fs_ops *)(dir->impl);
    if (!ops->create) {
        return 0;
    }
    return ops->create(dir, name);
}

int vfs_truncate(struct fs_node *node)
{
    if (!node || !node->impl) {
        return -1;
    }
    struct fs_ops *ops = (struct fs_ops *)(node->impl);
    if (ops->truncate) {
        return ops->truncate(node);
    }
    return -1;
}

int vfs_mkdir(struct fs_node *dir, const char *name)
{
    if (!dir || (dir->flags & 0x7) != FS_DIRECTORY || !dir->impl) {
        return -1;
    }
    struct fs_ops *ops = (struct fs_ops *)(dir->impl);
    if (!ops->mkdir) {
        return -1;
    }
    return ops->mkdir(dir, name);
}
