#include <ata.h>
#include <block.h>
#include <io.h>
#include <kernel.h>
#include <mutex.h>
#include <stdint.h>

#define ATA_REG_DATA       0
#define ATA_REG_ERROR      1
#define ATA_REG_SECCOUNT   2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_HDDEVSEL   6
#define ATA_REG_COMMAND    7
#define ATA_REG_STATUS     7

#define ATA_STATUS_ERR     0x01
#define ATA_STATUS_DRQ     0x08
#define ATA_STATUS_DF      0x20
#define ATA_STATUS_BSY     0x80

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY   0xEC

#define ATA_IDENTIFY_LBA   (1U << 9)
#define ATA_LBA28_SECTORS  0x10000000U
#define ATA_POLL_LIMIT     1000000U

struct ata_channel
{
    uint16_t io;
    uint16_t ctrl;
    struct mutex lock;
};

struct ata_device
{
    struct block_device block;
    struct ata_channel *channel;
    uint8_t slave;
    char model[41];
};

static struct ata_channel channels[2] = {
    { .io = 0x1F0, .ctrl = 0x3F6, .lock = MUTEX_INIT },
    { .io = 0x170, .ctrl = 0x376, .lock = MUTEX_INIT },
};

static struct ata_device ata_devices[4];
static const char *device_names[4] = { "hda", "hdb", "hdc", "hdd" };

static void ata_delay(struct ata_channel *channel)
{
    inb(channel->ctrl);
    inb(channel->ctrl);
    inb(channel->ctrl);
    inb(channel->ctrl);
}

static int ata_wait(struct ata_channel *channel, int require_drq)
{
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(channel->io + ATA_REG_STATUS);
        if (!status) {
            return -1;
        }
        if (!(status & ATA_STATUS_BSY)) {
            if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
                return -1;
            }
            if (!require_drq || (status & ATA_STATUS_DRQ)) {
                return 0;
            }
        }
    }
    return -1;
}

static void ata_select(struct ata_device *dev, uint32_t lba)
{
    outb(dev->channel->io + ATA_REG_HDDEVSEL,
         0xE0 | (dev->slave << 4) | ((lba >> 24) & 0x0F));
    ata_delay(dev->channel);
}

static int ata_prepare(struct ata_device *dev, uint32_t lba, uint8_t command)
{
    struct ata_channel *channel = dev->channel;

    ata_select(dev, lba);
    if (ata_wait(channel, 0) < 0) {
        return -1;
    }
    outb(channel->io + ATA_REG_SECCOUNT, 1);
    outb(channel->io + ATA_REG_LBA0, lba & 0xFF);
    outb(channel->io + ATA_REG_LBA1, (lba >> 8) & 0xFF);
    outb(channel->io + ATA_REG_LBA2, (lba >> 16) & 0xFF);
    outb(channel->io + ATA_REG_COMMAND, command);
    return ata_wait(channel, 1);
}

static int ata_read_blocks(struct block_device *block, uint32_t lba,
                           uint32_t count, uint8_t *buffer)
{
    struct ata_device *dev = block->impl;
    struct ata_channel *channel = dev->channel;
    int result = 0;

    mutex_lock(&channel->lock);
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_prepare(dev, lba + sector, ATA_CMD_READ_PIO) < 0) {
            result = -1;
            break;
        }
        for (uint32_t i = 0; i < 256; i++) {
            uint16_t word = inw(channel->io + ATA_REG_DATA);
            buffer[sector * BLOCK_SECTOR_SIZE + i * 2] = word & 0xFF;
            buffer[sector * BLOCK_SECTOR_SIZE + i * 2 + 1] = word >> 8;
        }
        ata_delay(channel);
    }
    mutex_unlock(&channel->lock);
    return result;
}

static int ata_write_blocks(struct block_device *block, uint32_t lba,
                            uint32_t count, const uint8_t *buffer)
{
    struct ata_device *dev = block->impl;
    struct ata_channel *channel = dev->channel;
    int result = 0;

    mutex_lock(&channel->lock);
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_prepare(dev, lba + sector, ATA_CMD_WRITE_PIO) < 0) {
            result = -1;
            break;
        }
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t offset = sector * BLOCK_SECTOR_SIZE + i * 2;
            uint16_t word = buffer[offset] | ((uint16_t)buffer[offset + 1] << 8);
            outw(channel->io + ATA_REG_DATA, word);
        }
        ata_delay(channel);
        if (ata_wait(channel, 0) < 0) {
            result = -1;
            break;
        }
    }
    if (!result) {
        ata_select(dev, 0);
        outb(channel->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
        if (ata_wait(channel, 0) < 0) {
            result = -1;
        }
    }
    mutex_unlock(&channel->lock);
    return result;
}

static int ata_identify(struct ata_device *dev)
{
    struct ata_channel *channel = dev->channel;
    uint16_t identify[256];

    outb(channel->io + ATA_REG_HDDEVSEL, 0xA0 | (dev->slave << 4));
    ata_delay(channel);
    outb(channel->io + ATA_REG_SECCOUNT, 0);
    outb(channel->io + ATA_REG_LBA0, 0);
    outb(channel->io + ATA_REG_LBA1, 0);
    outb(channel->io + ATA_REG_LBA2, 0);
    outb(channel->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(channel->io + ATA_REG_STATUS);
    if (!status) {
        return -1;
    }
    for (uint32_t i = 0; i < ATA_POLL_LIMIT && (status & ATA_STATUS_BSY); i++) {
        status = inb(channel->io + ATA_REG_STATUS);
    }
    if (status & ATA_STATUS_BSY) {
        return -1;
    }
    if (inb(channel->io + ATA_REG_LBA1) || inb(channel->io + ATA_REG_LBA2)) {
        return -1;
    }
    if (ata_wait(channel, 1) < 0) {
        return -1;
    }
    for (uint32_t i = 0; i < 256; i++) {
        identify[i] = inw(channel->io + ATA_REG_DATA);
    }
    if (!(identify[49] & ATA_IDENTIFY_LBA)) {
        return -1;
    }

    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    if (!sectors) {
        return -1;
    }
    if (sectors > ATA_LBA28_SECTORS) {
        sectors = ATA_LBA28_SECTORS;
    }
    for (uint32_t i = 0; i < 20; i++) {
        dev->model[i * 2] = identify[27 + i] >> 8;
        dev->model[i * 2 + 1] = identify[27 + i] & 0xFF;
    }
    dev->model[40] = '\0';
    for (int i = 39; i >= 0 && dev->model[i] == ' '; i--) {
        dev->model[i] = '\0';
    }

    dev->block.sector_count = sectors;
    return 0;
}

void ata_init(void)
{
    for (uint32_t channel = 0; channel < 2; channel++) {
        outb(channels[channel].ctrl, 0x02);
        for (uint32_t slave = 0; slave < 2; slave++) {
            uint32_t index = channel * 2 + slave;
            struct ata_device *dev = &ata_devices[index];
            dev->channel = &channels[channel];
            dev->slave = slave;
            dev->block.name = device_names[index];
            dev->block.read = ata_read_blocks;
            dev->block.write = ata_write_blocks;
            dev->block.impl = dev;
            if (ata_identify(dev) == 0 && block_register(&dev->block) == 0) {
                kprintf("ata: %s: %s, %u sectors\n", dev->block.name,
                        dev->model, dev->block.sector_count);
            }
        }
    }
}

static int ata_test_mode(const char *cmdline, const char *mode)
{
    if (!cmdline) {
        return 0;
    }
    while (*cmdline) {
        while (*cmdline == ' ') {
            cmdline++;
        }
        const char *arg = cmdline;
        while (*cmdline && *cmdline != ' ') {
            cmdline++;
        }
        const char *expected = "ata-test=";
        uint32_t i = 0;
        while (arg + i < cmdline && expected[i] && arg[i] == expected[i]) {
            i++;
        }
        if (!expected[i]) {
            uint32_t j = 0;
            while (arg + i + j < cmdline && mode[j] &&
                   arg[i + j] == mode[j]) {
                j++;
            }
            if (!mode[j] && arg + i + j == cmdline) {
                return 1;
            }
        }
    }
    return 0;
}

static int ata_test_signature(const uint8_t *buffer, const char *signature)
{
    uint32_t i = 0;
    while (signature[i] && buffer[i] == (uint8_t)signature[i]) {
        i++;
    }
    return signature[i] == '\0';
}

void ata_test(const char *cmdline)
{
    int write_mode = ata_test_mode(cmdline, "write");
    int verify_mode = ata_test_mode(cmdline, "verify");
    if (!write_mode && !verify_mode) {
        return;
    }

    uint8_t sector[BLOCK_SECTOR_SIZE];
    if (block_count() != 4) {
        panic("ATA TEST: expected four disks\n");
    }
    for (uint32_t disk = 0; disk < block_count(); disk++) {
        struct block_device *dev = block_get(disk);
        if (!dev || dev->sector_count <= 8) {
            panic("ATA TEST: unsuitable disk\n");
        }
        if (write_mode) {
            if (block_read(dev, 7, 1, sector) < 0 ||
                !ata_test_signature(sector, "NOS ATA SEEDED SECTOR")) {
                panic("ATA TEST: seeded read failed\n");
            }
            for (uint32_t i = 0; i < BLOCK_SECTOR_SIZE; i++) {
                sector[i] = 0;
            }
            const char *signature = "NOS ATA PERSISTED SECTOR";
            for (uint32_t i = 0; signature[i]; i++) {
                sector[i] = signature[i];
            }
            if (block_write(dev, 8, 1, sector) < 0) {
                panic("ATA TEST: write failed\n");
            }
            for (uint32_t i = 0; i < BLOCK_SECTOR_SIZE; i++) {
                sector[i] = 0;
            }
            if (block_read(dev, 8, 1, sector) < 0 ||
                !ata_test_signature(sector, signature)) {
                panic("ATA TEST: write readback failed\n");
            }
            kprintf("ATA TEST: %s read/write ok\n", dev->name);
        } else {
            if (block_read(dev, 8, 1, sector) < 0 ||
                !ata_test_signature(sector, "NOS ATA PERSISTED SECTOR")) {
                panic("ATA TEST: persistence verify failed\n");
            }
            kprintf("ATA TEST: %s persistence ok\n", dev->name);
        }
    }
}
