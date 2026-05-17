#include "kernel/arch/x86_64/pci.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/core/mm/slab.hpp"
#include <stdint.h>

static constexpr uint16_t PCI_CONFIG_ADDR = 0xCF8;
static constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;

static PciDevice* device_list      = nullptr;
static PciDevice* device_list_tail = nullptr;

// ── Internal helpers ──────────────────────────────────────────────────────────

uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (1UL << 31)
                  | (static_cast<uint32_t>(bus)  << 16)
                  | ((static_cast<uint32_t>(dev)  & 0x1F) << 11)
                  | ((static_cast<uint32_t>(func) & 0x07) << 8)
                  | (offset & 0xFC);
    x86::outl(PCI_CONFIG_ADDR, addr);
    return x86::inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read(bus, dev, func, offset & ~0x3);
    return static_cast<uint16_t>((val >> ((offset & 0x2) * 8)) & 0xFFFF);
}

static uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read(bus, dev, func, offset & ~0x3);
    return static_cast<uint8_t>((val >> ((offset & 0x3) * 8)) & 0xFF);
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    return pci_config_read(bus, dev, func, offset);
}

// ── Device list helpers ───────────────────────────────────────────────────────

static void pci_add_device(uint8_t bus, uint8_t dev, uint8_t func) {
    // Vendor & device ID (offset 0x00)
    uint32_t id       = pci_config_read32(bus, dev, func, 0x00);
    uint16_t vendor_id = id & 0xFFFF;
    uint16_t device_id = (id >> 16) & 0xFFFF;

    // Class code, subclass, prog IF, revision (offset 0x08)
    uint32_t class_rev = pci_config_read32(bus, dev, func, 0x08);
    uint8_t  rev_id    = class_rev & 0xFF;
    uint8_t  prog_if   = (class_rev >> 8) & 0xFF;
    uint8_t  subclass  = (class_rev >> 16) & 0xFF;
    uint8_t  class_code = (class_rev >> 24) & 0xFF;

    // Header type (offset 0x0C, byte at offset 0x0E)
    uint32_t header     = pci_config_read32(bus, dev, func, 0x0C);
    uint8_t  header_type = (header >> 16) & 0xFF;

    // Interrupt line (offset 0x3C)
    uint32_t irq_reg  = pci_config_read32(bus, dev, func, 0x3C);
    uint8_t  irq_line = irq_reg & 0xFF;

    // BARs (offsets 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24)
    uint32_t bars[6] = {};
    if (header_type == 0x00) {
        for (int i = 0; i < 6; i++) {
            bars[i] = pci_config_read32(bus, dev, func, 0x10 + i * 4);
        }
    }

    // Allocate and populate device node
    PciDevice* pd = static_cast<PciDevice*>(kmalloc(sizeof(PciDevice)));
    if (!pd) return;

    pd->bus        = bus;
    pd->dev        = dev;
    pd->func       = func;
    pd->vendor_id  = vendor_id;
    pd->device_id  = device_id;
    pd->class_code = class_code;
    pd->subclass   = subclass;
    pd->prog_if    = prog_if;
    pd->rev_id     = rev_id;
    pd->irq_line   = irq_line;
    for (int i = 0; i < 6; i++) pd->bar[i] = bars[i];
    pd->next = nullptr;

    // Append to linked list
    if (device_list_tail) {
        device_list_tail->next = pd;
    } else {
        device_list = pd;
    }
    device_list_tail = pd;

    // Log summary
    klog("PCI ");
    klog_hex(bus); klog(":");
    klog_hex(dev); klog(".");
    klog_hex(func); klog(" vendor=");
    klog_hex(vendor_id); klog(" device=");
    klog_hex(device_id); klog(" class=");
    klog_hex(class_code); klog(" subclass=");
    klog_hex(subclass); klog("\n");
}

// ── Public API ────────────────────────────────────────────────────────────────

void pci_init() {
    // Triple-loop: bus 0..255, device 0..31, function 0..7
    // For each bus: check device at function 0 first; if vendor==0xFFFF, skip.
    // If multi-function (header_type bit 7), scan functions 1..7.
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            // Check vendor at function 0
            uint16_t vendor = pci_config_read16(bus, dev, 0, 0);
            if (vendor == 0xFFFF) continue;

            pci_add_device(bus, dev, 0);

            // Check if multi-function device
            uint8_t header_type = pci_config_read8(bus, dev, 0, 0x0E);
            if (header_type & 0x80) {
                for (uint8_t func = 1; func < 8; func++) {
                    vendor = pci_config_read16(bus, dev, func, 0);
                    if (vendor == 0xFFFF) continue;
                    pci_add_device(bus, dev, func);
                }
            }
        }
    }
}

PciDevice* pci_first_device() {
    return device_list;
}

PciDevice* pci_find_by_class(uint8_t class_code, uint8_t subclass) {
    PciDevice* cur = device_list;
    while (cur) {
        if (cur->class_code == class_code && cur->subclass == subclass)
            return cur;
        cur = cur->next;
    }
    return nullptr;
}
