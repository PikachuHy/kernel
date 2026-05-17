# Phase 9: Block Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** PCI enumeration + AHCI SATA driver + block device abstraction + buffer cache.

**Architecture:** All kernel space. PCI scans hardware, AHCI driver uses DMA for disk I/O, BlockDevice is an ops-based abstraction, BufferCache is a simple LRU array.

**Tech Stack:** C++26 freestanding, x86-64, Bazel 9

---

### Task 1: PCI Enumeration

**Files:**
- Create: `kernel/arch/x86_64/pci.hpp`
- Create: `kernel/arch/x86_64/pci.cpp`

Scan PCI configuration space, build device list, detect AHCI controllers.

**Key code:**

`pci.hpp`:
```cpp
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

constexpr uint8_t PCI_CLASS_STORAGE = 0x01;
constexpr uint8_t PCI_SUBCLASS_SATA = 0x06;

void pci_init();
PciDevice* pci_first_device();
PciDevice* pci_find_by_class(uint8_t class_code, uint8_t subclass);
```

`pci.cpp`:
```cpp
#include "kernel/arch/x86_64/pci.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/klog.hpp"

static PciDevice* g_pci_list = nullptr;

static uint32_t pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11)
                    | ((uint32_t)(func & 7) << 8) | (offset & 0xFC);
    x86::outl(0xCF8, addr);
    return x86::inl(0xCFC);
}

void pci_init() {
    klog("PCI: enumerating bus...\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_config_read(bus, dev, func, 0);
                uint16_t vendor = id & 0xFFFF;
                if (vendor == 0xFFFF) continue;

                PciDevice* d = static_cast<PciDevice*>(kmalloc(sizeof(PciDevice)));
                // ... (populate fields, add to g_pci_list)
                klog("  PCI "); klog_hex(bus); klog(":"); klog_hex(dev);
                klog(" vendor="); klog_hex(vendor);
                klog(" device="); klog_hex(id >> 16); klog("\n");

                if (func == 0) {
                    // Check if multi-function, continue scanning func 1-7
                }
            }
        }
    }
    klog("PCI: enumeration complete\n");
}
```

**Verification**:
```bash
bash scripts/run.sh 2>&1 | grep "PCI:"
```
Expected: list of QEMU PCI devices including vendor/device IDs.

---

### Task 2: Block Device Abstraction + Buffer Cache

**Files:**
- Create: `kernel/core/blk/blkdev.hpp`, `blkdev.cpp`
- Create: `kernel/core/blk/bufcache.hpp`, `bufcache.cpp`
- Create: `kernel/core/blk/BUILD.bazel`

BlockDevice is an ops-based abstraction. BufCache is a simple LRU array of 64 cached sectors.

**Verification**: No visible output yet — tested with AHCI in Task 3.

---

### Task 3: AHCI Driver

**Files:**
- Create: `kernel/core/blk/ahci.hpp`, `ahci.cpp`

Find AHCI via PCI, map ABAR, reset HBA, send IDENTIFY, register BlockDevice, verify by reading sector 0.

**Key verification output**:
```
AHCI: controller at MMIO 0x...
AHCI: port 0: QEMU HARDDISK    131072 sectors
AHCI: sector 0 loaded (MBR present)
```

---

### Task 4: Boot Integration + QEMU Test

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp` — add Phase 9 section after Phase 3

```cpp
// ── Phase 9: Block Layer ──
klog("\n=== Phase 9: Block Layer ===\n\n");
pci_init();
ahci_init();
```

**Verification**:
```bash
bash scripts/run.sh 2>&1 | grep -E "PCI:|AHCI:|Phase 9"
```
Expected: PCI devices listed, AHCI detected, disk info printed, MBR sector loaded.
