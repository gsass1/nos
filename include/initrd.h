#ifndef __INITRD_H__
#define __INITRD_H__

#include <vfs.h>

// We use the Tar format for the initrd!
struct tar_header
{
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
};

struct fs_node *initrd_init(void *ptr);

// Install the ext2 mountpoint node so it appears as "disk" in the initrd root
// directory listing and is resolvable via vfs_resolve("disk/..."). Pass 0 to
// remove the mountpoint (no disk present).
void initrd_set_disk_mount(struct fs_node *node);

#endif