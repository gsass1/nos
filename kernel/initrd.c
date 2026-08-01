#include <initrd.h>
#include <mm.h>
#include <string.h>
#include <kernel.h>

MODULE("INRD");

static struct fs_node *initrd_root;
static struct fs_node *root_nodes;
static struct fs_node *initrd_dev = 0;
static struct dirent dirent;
static uint32_t nroot_nodes;
// The ext2 mountpoint node, or 0 if no disk is mounted. Set by main.c after
// ext2_mount; appears as "disk" in the initrd root listing.
static struct fs_node *ext2_mount_node = 0;

struct tar_header_chain
{
    uint32_t size;
    struct tar_header *tar_header;
    struct tar_header_chain *next;
};

static struct tar_header_chain *headers = 0;

static uint32_t getsize(const char *in)
{
 
    unsigned int size = 0;
    unsigned int j;
    unsigned int count = 1;
 
    for (j = 11; j > 0; j--, count *= 8)
        size += ((in[j - 1] - '0') * count);
 
    return size;
}

static struct tar_header_chain *initrd_get_node_by_inode(uint32_t inode)
{
    uint32_t i;
    struct tar_header_chain *header = headers;
    for(i = 0; i < nroot_nodes; i++) {
        if(i == inode) {
            return header;
        }
        header = header->next;
    }
    panic("initrd_get_node_by_inode: Cannot find node for %d!\n", inode);
    return 0;
}

static uint32_t parse(void *ptr)
{
    mprintf(LOGLEVEL_DEBUG, "Parsing tar file\n");

    uint32_t i;
    uint32_t addr = (uint32_t)ptr;
    headers = kmalloc(sizeof(struct tar_header_chain));
    struct tar_header_chain *header = headers;

    for(i = 0; ; i++) {
        uint32_t size;
        struct tar_header *tar_header = (struct tar_header *)addr;

        mprintf(LOGLEVEL_DEBUG, "Address: 0x08%x\n", addr);

        if(tar_header->filename[0] == '\0') {
            mprintf(LOGLEVEL_DEBUG, "Tar header has no filename!\n");
            break;
        }

        size = getsize(tar_header->size);

        header->size = size;
        header->tar_header = tar_header;
        addr += ((size / 512) + 1) * 512;

        if(size % 512) {
            addr += 512;
        }

        header->next = kmalloc(sizeof(struct tar_header_chain));
        header->next->tar_header = 0;

        // Advance to the freshly allocated node. Without this every file
        // overwrote the first node, so only single-file initrds parsed right.
        header = header->next;
    }
    mprintf(LOGLEVEL_DEBUG, "Done parsing tar file, files: %d\n", i);
    return i;
}

static struct dirent *initrd_readdir(struct fs_node *node, uint32_t index)
{
    if(node == initrd_root && index == 0) {
        strcpy(dirent.name, "dev");
        dirent.name[3] = 0;
        dirent.inode = 0;
        return &dirent;
    }

    if(node == initrd_root && ext2_mount_node && index == 1) {
        strcpy(dirent.name, "disk");
        dirent.name[4] = 0;
        dirent.inode = 0;
        return &dirent;
    }

    // For the root, file entries start after "dev" (and "disk" if mounted).
    // For non-root nodes (initrd_dev), preserve the original base-1 behavior.
    uint32_t base;
    if (node == initrd_root) {
        base = ext2_mount_node ? 2 : 1;
    } else {
        base = 1;
    }

    if(index < base || index - base >= nroot_nodes) {
        return 0;
    }
    strcpy(dirent.name, root_nodes[index - base].name);
    dirent.name[strlen(root_nodes[index - base].name)] = 0;
    dirent.inode = root_nodes[index - base].inode;
    return &dirent;
}

static struct fs_node *initrd_finddir(struct fs_node *node, char *name)
{
    if(node == initrd_root && !strcmp(name, "dev")) {
        return initrd_dev;
    }

    if(node == initrd_root && ext2_mount_node && !strcmp(name, "disk")) {
        return ext2_mount_node;
    }

    uint32_t i;
    for(i = 0; i < nroot_nodes; i++) {
        if(!strcmp(name, root_nodes[i].name)) {
            return &root_nodes[i];
        }
    }
    return 0;
}

static uint32_t initrd_read(struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
	mprintf(LOGLEVEL_DEBUG, "initrd_read called\n");

    struct tar_header_chain *header = initrd_get_node_by_inode(node->inode);
    if(offset > header->size) {
		mprintf(LOGLEVEL_DEBUG, "offset > header->size\n");
        mprintf(LOGLEVEL_DEBUG, "%d > %d\n", offset, header->size);
		return 0;
    }
    if(offset + size > header->size) {
        size = header->size - offset;
    }
    // Add 512 to skip the header
    memcpy(buffer, (uint8_t *)header->tar_header + 512 + offset, size);
    return size;
}

void initrd_set_disk_mount(struct fs_node *node)
{
    ext2_mount_node = node;
}

struct fs_node *initrd_init(void *ptr)
{
    mprintf(LOGLEVEL_DEFAULT, "Initializing initrd\n");

    mprintf(LOGLEVEL_DEBUG, "Start address: 0x%08x\n", ptr);

    uint32_t num_files = 0;

    // Read in TAR contents
    num_files = parse(ptr);

    initrd_root = kmalloc(sizeof(struct fs_node));
    memset(initrd_root, 0, sizeof(*initrd_root));
    strcpy(initrd_root->name, "initrd");
    initrd_root->mask = 0;
    initrd_root->uid = 0;
    initrd_root->gid = 0;
    initrd_root->inode = 0;
    initrd_root->length = 0;
    initrd_root->flags = FS_DIRECTORY;
    initrd_root->read = 0;
    initrd_root->write = 0;
    initrd_root->open = 0;
    initrd_root->close = 0;
    initrd_root->readdir = &initrd_readdir;
    initrd_root->finddir = &initrd_finddir;
    initrd_root->ptr = 0;
    initrd_root->impl = 0;

    mprintf(LOGLEVEL_DEBUG, "initrd_root: 0x%08x\n", initrd_root);

    initrd_dev = kmalloc(sizeof(struct fs_node));
    memset(initrd_dev, 0, sizeof(*initrd_dev));
    strcpy(initrd_dev->name, "dev");
    initrd_dev->mask = initrd_dev->uid = initrd_dev->gid = initrd_dev->inode = initrd_dev->length = 0;
    initrd_dev->flags = FS_DIRECTORY;
    initrd_dev->read = 0;
    initrd_dev->write = 0;
    initrd_dev->open = 0;
    initrd_dev->close = 0;
    initrd_dev->readdir = &initrd_readdir;
    initrd_dev->finddir = &initrd_finddir;
    initrd_dev->ptr = 0;
    initrd_dev->impl = 0;

    root_nodes = kmalloc(sizeof(struct fs_node) * num_files);
    nroot_nodes = num_files;

    uint32_t i;
    struct tar_header_chain *header = headers;
    for(i = 0; i < nroot_nodes; i++) {
		mprintf(LOGLEVEL_DEBUG, "Added file: %s, inode: %d\n", header->tar_header->filename, i);
        memset(&root_nodes[i], 0, sizeof(root_nodes[i]));
        strcpy(root_nodes[i].name, header->tar_header->filename);
        root_nodes[i].length = header->size;
        root_nodes[i].inode = i;
        root_nodes[i].flags = FS_FILE;
        root_nodes[i].read = &initrd_read;
        root_nodes[i].write = 0;
        root_nodes[i].readdir = 0;
        root_nodes[i].finddir = 0;
        root_nodes[i].open = 0;
        root_nodes[i].close = 0;
        root_nodes[i].impl = 0;

        header = header->next;
    }
    return initrd_root;
}
