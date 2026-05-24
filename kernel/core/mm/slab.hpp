#pragma once

#include <stdint.h>
#include <stddef.h>

// Slab allocator: fixed-size caches backed by buddy page allocator.
// Size classes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.

// Initialize slab caches. Must be called after buddy_init().
// hhdm_offset: direct-map offset (DIRECT_MAP_BASE in kernel, heap offset in host tests).
auto slab_init(uint64_t hhdm_offset) -> void;

// Allocate memory. Rounds size up to the nearest size class.
// Returns nullptr for sizes > 2048 or OOM.
auto kmalloc(size_t size) -> void*;

// Free memory previously allocated by kmalloc.
auto kfree(void* ptr) -> void;

// Return the actual allocated size (>= requested due to rounding).
auto kmalloc_usable_size(void* ptr) -> size_t;
