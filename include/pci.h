#ifndef __PCI_H__
#define __PCI_H__

#include <stdint.h>

// Read a 32-bit register from PCI configuration space (mechanism #1).
uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);

// Scan for a device by vendor/device id. Returns 1 and fills bus/dev if
// found, 0 otherwise.
int pci_find_device(uint16_t vendor, uint16_t device, uint8_t *bus_out,
                    uint8_t *dev_out);

#endif
