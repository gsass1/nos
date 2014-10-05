#include <kernel.h>
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
