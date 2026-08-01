#include <io.h>
#include <pci.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    uint32_t address = 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset,
                      uint32_t value)
{
    uint32_t address = 0x80000000U
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)fn << 8)
        | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(uint16_t vendor, uint16_t device, uint8_t *bus_out,
                    uint8_t *dev_out)
{
    // Function 0 on the first few buses is all QEMU's flat topologies need.
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = pci_config_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) {
                continue; // no device
            }
            if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                *bus_out = (uint8_t)bus;
                *dev_out = (uint8_t)dev;
                return 1;
            }
        }
    }
    return 0;
}
