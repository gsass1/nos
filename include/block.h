#ifndef __BLOCK_H__
#define __BLOCK_H__

#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512

struct block_device;

typedef int (*block_read_t)(struct block_device *, uint32_t, uint32_t, uint8_t *);
typedef int (*block_write_t)(struct block_device *, uint32_t, uint32_t,
                             const uint8_t *);

struct block_device
{
    const char *name;
    uint32_t sector_count;
    block_read_t read;
    block_write_t write;
    void *impl;
};

int block_register(struct block_device *dev);
uint32_t block_count(void);
struct block_device *block_get(uint32_t index);
int block_read(struct block_device *dev, uint32_t lba, uint32_t count,
               uint8_t *buffer);
int block_write(struct block_device *dev, uint32_t lba, uint32_t count,
                const uint8_t *buffer);

#endif
