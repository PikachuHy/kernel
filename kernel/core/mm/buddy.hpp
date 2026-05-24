#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr int BUDDY_MAX_ORDER = 10;  // 4KB * 2^10 = 4MB max block

// Initialize the buddy allocator. Takes over physical page management
// from the bitmap allocator. Must be called after paging_init.
// hhdm_offset: Limine HHDM offset for phys-to-virt access.
// page_array_phys: physical address where the Page metadata array is placed.
//   Must NOT overlap kernel, bitmap, or page table pages.
auto buddy_init(uint64_t hhdm_offset, uint64_t page_array_phys) -> void;

// Allocate 2^order contiguous physical pages. Returns physical address.
// Pages are NOT zeroed. Returns nullptr on OOM.
auto buddy_alloc_pages(size_t order) -> void*;

// Free 2^order contiguous pages previously allocated by buddy_alloc_pages.
auto buddy_free_pages(void* phys_addr, size_t order) -> void;

auto buddy_free_page_count() -> size_t;
auto buddy_total_pages() -> size_t;
