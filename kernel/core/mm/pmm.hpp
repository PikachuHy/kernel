#pragma once

#include <stdint.h>
#include <stddef.h>

// Memory map entry types matching Limine protocol
constexpr uint32_t MEMMAP_USABLE                 = 0;
constexpr uint32_t MEMMAP_RESERVED               = 1;
constexpr uint32_t MEMMAP_ACPI_RECLAIMABLE       = 2;
constexpr uint32_t MEMMAP_ACPI_NVS               = 3;
constexpr uint32_t MEMMAP_BAD_MEMORY             = 4;
constexpr uint32_t MEMMAP_BOOTLOADER_RECLAIMABLE = 5;
constexpr uint32_t MEMMAP_KERNEL_AND_MODULES     = 6;
constexpr uint32_t MEMMAP_FRAMEBUFFER            = 7;

struct MemRange {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

// Initialize PMM from the memory map provided by Limine.
// kernel_phys_start..kernel_phys_end is carved out of usable ranges.
void pmm_init(
    const MemRange* ranges,
    size_t count,
    uint64_t kernel_phys_start,
    uint64_t kernel_phys_end);

uint64_t pmm_total_memory();
uint64_t pmm_usable_memory();
uint64_t pmm_highest_phys_addr();

// Returns pointer to internal array of usable ranges. count is output parameter.
const MemRange* pmm_usable_ranges(size_t* out_count);

// Check if a physical address range is entirely contained within usable memory.
bool pmm_is_usable(uint64_t phys_addr, uint64_t length);
