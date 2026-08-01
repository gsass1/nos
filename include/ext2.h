#ifndef __EXT2_H__
#define __EXT2_H__

#include <vfs.h>

struct block_device;

// Mount an ext2 revision-0, feature-free filesystem from the given block
// device. Returns the root fs_node (FS_DIRECTORY | FS_MOUNTPOINT), or 0 on
// failure. Never formats -- returns 0 if the device is not a valid ext2
// filesystem or fails any validation check.
struct fs_node *ext2_mount(struct block_device *dev);

#endif
