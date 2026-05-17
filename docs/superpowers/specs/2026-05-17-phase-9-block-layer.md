# Phase 9: Block Layer — Design Spec

## Overview

Kernel-space block device layer with PCI enumeration, AHCI SATA driver, block
device abstraction, and buffer cache. Foundation for disk-backed filesystems.

**Architecture: all kernel space.**

## Subsystems

### 1. PCI Bus Enumerator

Scan PCI configuration space via I/O ports `0xCF8`/`0xCFC` (Configuration Address/Data).

```
pci_init() → enumerate all buses/devices/functions → populate device list
```

Key data structure:
```cpp
struct PciDevice {
    uint8_t  bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  rev_id;
    uint8_t  irq_line;
    uint32_t bar[6];         // BAR0-BAR5 physical addresses
};
```

**Filter for AHCI**: class=0x01 (mass storage), subclass=0x06 (SATA).

### 2. Block Device Abstraction

```cpp
struct BlockDevice {
    char     name[32];
    uint64_t total_sectors;
    uint32_t sector_size;       // typically 512

    int (*read)(BlockDevice* dev, uint64_t lba, void* buf, size_t count);
    int (*write)(BlockDevice* dev, uint64_t lba, const void* buf, size_t count);

    BlockDevice* next;          // global linked list
};
// Returns 0 on success, negative on error.
```

### 3. AHCI Driver

- **Init**: Read ABAR from PCI BAR5, map via direct map (`DIRECT_MAP_BASE + phys`).
- **HBA Reset**: Reset HBA via GHC register, enable AHCI mode, configure interrupts.
- **Port Init**: For each implemented port, reset port, start command engine.
- **IDENTIFY**: Send ATA IDENTIFY DEVICE via FIS-based command. Parse model string and LBA48 capacity.
- **DMA Read/Write**: Allocate PRDT (Physical Region Descriptor Table) from buddy pages, build command table, issue READ/WRITE DMA EXT commands.

### 4. Buffer Cache

```cpp
constexpr size_t BUF_CACHE_COUNT = 64;

struct BufCacheEntry {
    BlockDevice* dev;
    uint64_t     lba;            // sector number
    uint8_t*     data;           // buddy page (512 bytes)
    bool         dirty;
    uint64_t     access_time;    // for LRU eviction
};
```

Simple LRU array (no hash table for Phase 9). Cache miss → read from device → fill entry.

### 5. Boot Integration

```cpp
// After Phase 3 (APIC+Timer), before Phase 5 (Scheduler)
pci_init();
ahci_init();
bd_cache_init();
```

### Files

| File | Purpose |
|------|---------|
| `kernel/arch/x86_64/pci.cpp` / `pci.hpp` | PCI enumeration |
| `kernel/core/blk/pci.hpp` | PciDevice shared struct |
| `kernel/core/blk/ahci.cpp` / `ahci.hpp` | AHCI driver |
| `kernel/core/blk/blkdev.cpp` / `blkdev.hpp` | BlockDevice abstraction |
| `kernel/core/blk/bufcache.cpp` / `bufcache.hpp` | Buffer cache |
| `kernel/core/blk/BUILD.bazel` | Build rules |

## Dependencies

| Dependency | Status |
|------------|--------|
| Paging (DIRECT_MAP_BASE for MMIO) | Done |
| Buddy allocator (DMA buffers, cache pages) | Done |
| LAPIC + IOAPIC (AHCI interrupt) | Done |
| IRQ dispatch (register AHCI handler) | Done |
| klog (debug output) | Done |

## Verification

```bash
bash scripts/run.sh
```
Expected:
- PCI scan lists all QEMU devices (vendor/device IDs)
- AHCI controller detected
- IDENTIFY prints disk model and size (e.g., "QEMU HARDDISK", 131072 sectors)
- Read MBR sector 0, verify first byte is not 0x00
