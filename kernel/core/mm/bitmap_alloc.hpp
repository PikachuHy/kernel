#pragma once

#include <stdint.h>
#include <stddef.h>

// Early-boot page allocator using a bitmap.
// Used to allocate page table pages before the buddy allocator is online.
// The bitmap is stored in BSS (compile-time allocated, fixed size for up to 4GB RAM).

// Initialize. bitmap_base_phys is where the bitmap itself lives in physical memory.
// hhdm_offset is Limine's HHDM offset for accessing physical memory before our
// direct map is set up.
void bitmap_init(uint64_t hhdm_offset, uint64_t bitmap_base_phys);

// Allocate a single zeroed 4KB page. Returns physical address, or 0 if OOM.
void* bitmap_alloc_page();

// Free a single 4KB page.
void bitmap_free_page(void* phys_addr);

// Statistics
size_t bitmap_free_page_count();
size_t bitmap_total_page_count();
