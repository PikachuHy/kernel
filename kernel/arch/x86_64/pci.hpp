#pragma once
#include <stdint.h>

struct PciDevice {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  rev_id;
    uint8_t  irq_line;
    uint32_t bar[6];
    PciDevice* next;
};

constexpr uint8_t PCI_CLASS_STORAGE   = 0x01;
constexpr uint8_t PCI_SUBCLASS_SATA   = 0x06;
constexpr uint8_t PCI_SUBCLASS_NVME   = 0x08;

// Returns 0xFFFFFFFF if device not present.
auto pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) -> uint32_t;
auto pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) -> uint16_t;

// Enumerate all PCI devices, populate internal list.
auto pci_init() -> void;

// Iterator
auto pci_first_device() -> PciDevice*;

// Find first device matching class/subclass, or nullptr.
auto pci_find_by_class(uint8_t class_code, uint8_t subclass) -> PciDevice*;
