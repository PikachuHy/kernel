#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr int BUDDY_MAX_ORDER = 10;  // 4KB * 2^10 = 4MB max block

// Initialize the buddy allocator. Takes over physical page management
// from the bitmap allocator. Must be called after the direct map is active
// (paging_init has switched CR3).
// hhdm_offset: 0 if direct map is active, or Limine HHDM for transitional use.
void buddy_init(uint64_t hhdm_offset);

// Allocate 2^order contiguous physical pages. Returns physical address.
// Pages are NOT zeroed. Returns nullptr on OOM.
void* buddy_alloc_pages(size_t order);

// Free 2^order contiguous pages previously allocated by buddy_alloc_pages.
void buddy_free_pages(void* phys_addr, size_t order);

size_t buddy_free_page_count();
size_t buddy_total_pages();
