#include <block.h>

#define BLOCK_MAX_DEVICES 4

static struct block_device *devices[BLOCK_MAX_DEVICES];
static uint32_t ndevices;

int block_register(struct block_device *dev)
{
    if (!dev || !dev->name || !dev->sector_count || !dev->read ||
        !dev->write || ndevices == BLOCK_MAX_DEVICES) {
        return -1;
    }
    devices[ndevices++] = dev;
    return 0;
}

uint32_t block_count(void)
{
    return ndevices;
}

struct block_device *block_get(uint32_t index)
{
    return index < ndevices ? devices[index] : 0;
}

static int block_range_ok(struct block_device *dev, uint32_t lba,
                          uint32_t count)
{
    return dev && count && lba < dev->sector_count &&
           count <= dev->sector_count - lba;
}

int block_read(struct block_device *dev, uint32_t lba, uint32_t count,
               uint8_t *buffer)
{
    if (!buffer || !block_range_ok(dev, lba, count)) {
        return -1;
    }
    return dev->read(dev, lba, count, buffer);
}

int block_write(struct block_device *dev, uint32_t lba, uint32_t count,
                const uint8_t *buffer)
{
    if (!buffer || !block_range_ok(dev, lba, count)) {
        return -1;
    }
    return dev->write(dev, lba, count, buffer);
}
