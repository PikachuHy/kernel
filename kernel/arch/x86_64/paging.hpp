#pragma once

#include <stdint.h>

extern uint64_t g_hhdm;

constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t LARGE_PAGE_SIZE = 0x200000;
constexpr uint64_t HUGE_PAGE_SIZE = 0x40000000;
constexpr uint16_t PAGE_TABLE_ENTRIES = 512;

namespace PageFlags {
    constexpr uint64_t Present    = 1ULL << 0;
    constexpr uint64_t Writable   = 1ULL << 1;
    constexpr uint64_t User       = 1ULL << 2;
    constexpr uint64_t WriteThru  = 1ULL << 3;
    constexpr uint64_t CacheDis   = 1ULL << 4;
    constexpr uint64_t Accessed   = 1ULL << 5;
    constexpr uint64_t Dirty      = 1ULL << 6;
    constexpr uint64_t Huge       = 1ULL << 7;
    constexpr uint64_t Global     = 1ULL << 8;
    constexpr uint64_t NoExec     = 1ULL << 63;
}

struct alignas(PAGE_SIZE) PageTable {
    uint64_t entries[PAGE_TABLE_ENTRIES];
};

inline auto pml4_index(uint64_t va) noexcept -> uint16_t { return (va >> 39) & 0x1FF; }
inline auto pdpt_index(uint64_t va) noexcept -> uint16_t { return (va >> 30) & 0x1FF; }
inline auto pd_index(uint64_t va) noexcept -> uint16_t   { return (va >> 21) & 0x1FF; }
inline auto pt_index(uint64_t va) noexcept -> uint16_t   { return (va >> 12) & 0x1FF; }

constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;

inline auto phys_to_virt(uint64_t phys_addr) noexcept -> void* {
    return reinterpret_cast<void*>(DIRECT_MAP_BASE + phys_addr);
}

inline auto virt_to_phys(const void* virt_addr) noexcept -> uint64_t {
    return reinterpret_cast<uint64_t>(virt_addr) - DIRECT_MAP_BASE;
}

// Take over paging: create new page tables with kernel and direct map.
// hhdm: Limine HHDM offset for transitional phys-to-virt during setup.
auto paging_init(
    uint64_t hhdm,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t kernel_size) -> void;

// Save the kernel-half PML4 entries (indices 256-511) as a template
// for new process address spaces. Call after paging_init succeeds.
auto paging_save_kernel_template() -> void;

// Returns the saved kernel PML4 template (physical address).
auto paging_kernel_pml4_template() -> uint64_t;

// Walk the page table for `va` in the given PML4, creating intermediate
// tables as needed via bitmap_alloc_page. Installs `pa | flags` as the
// leaf PTE. Returns true on success, false on allocation failure.
auto page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) -> bool;

// Unmap a 4K page. Returns the physical address that was mapped, or 0.
auto page_table_unmap(uint64_t pml4_phys, uint64_t va) -> uint64_t;

// Look up the physical address mapped at `va` in the given PML4.
// Returns 0 if not mapped.
auto page_table_lookup(uint64_t pml4_phys, uint64_t va) -> uint64_t;

inline constexpr auto make_pte(uint64_t phys_addr, uint64_t flags) noexcept -> uint64_t {
    return (phys_addr & ~(PAGE_SIZE - 1)) | flags;
}

inline constexpr auto pte_phys_addr(uint64_t entry) noexcept -> uint64_t {
    return entry & ~(PAGE_SIZE - 1);
}
