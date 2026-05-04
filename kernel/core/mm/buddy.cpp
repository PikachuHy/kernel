#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

struct Page {
    int order;     // -1 = allocated, 0..MAX_ORDER = free block of this order
    Page* next;    // next free block in this order's free list
};

Page* g_pages = nullptr;
Page* g_free_lists[BUDDY_MAX_ORDER + 1] = {};
size_t g_total_pages = 0;
size_t g_free_pages = 0;

inline size_t phys_to_idx(uint64_t phys) { return phys / PAGE_SIZE; }
inline uint64_t idx_to_phys(size_t idx) { return idx * PAGE_SIZE; }

// Buddy index: flip the order-th bit of the page index
inline size_t buddy_idx(size_t idx, int order) {
    return idx ^ (1ULL << order);
}

// Split a block at 'idx' of 'order' down to 'target_order'.
// Returns the block at idx at target_order (marked allocated).
void split(size_t idx, int order, int target_order) {
    while (order > target_order) {
        order--;
        size_t bud = idx + (1ULL << order);
        g_pages[bud].order = order;
        g_pages[bud].next = g_free_lists[order];
        g_free_lists[order] = &g_pages[bud];
        g_free_pages += (1ULL << order);
    }
    g_pages[idx].order = -1;  // allocated
    g_free_pages -= (1ULL << target_order);
}

// Try to coalesce from idx upward. Returns the final order.
int coalesce(size_t idx, int order) {
    while (order < BUDDY_MAX_ORDER) {
        size_t bud = buddy_idx(idx, order);
        if (bud >= g_total_pages) break;
        if (g_pages[bud].order != order) break;

        // Remove buddy from free list
        Page** prev = &g_free_lists[order];
        while (*prev != &g_pages[bud]) {
            prev = &(*prev)->next;
        }
        *prev = g_pages[bud].next;
        g_free_pages -= (1ULL << order);

        // Merge: idx becomes the lower of the pair
        if (bud < idx) idx = bud;
        order++;
    }
    g_pages[idx].order = order;
    g_pages[idx].next = g_free_lists[order];
    g_free_lists[order] = &g_pages[idx];
    g_free_pages += (1ULL << order);
    return order;
}

} // namespace

void buddy_init(uint64_t hhdm_offset) {
    size_t usable_count;
    const MemRange* usable = pmm_usable_ranges(&usable_count);
    if (usable_count == 0) return;

    g_total_pages = pmm_highest_phys_addr() / PAGE_SIZE;

    // Place Page array at 2MB physical to avoid overwriting page table
    // pages (0x1000-0x7000) and the bitmap (0x200000 area).
    size_t pages_array_bytes = g_total_pages * sizeof(Page);
    size_t pages_array_pages = (pages_array_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t page_array_phys = 0x400000;  // 4MB (above 2MB bitmap)

    g_pages = reinterpret_cast<Page*>(page_array_phys + hhdm_offset);

    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        g_free_lists[i] = nullptr;
    }

    // Mark all pages allocated initially
    for (size_t i = 0; i < g_total_pages; i++) {
        g_pages[i].order = -1;
        g_pages[i].next = nullptr;
    }

    g_free_pages = 0;

    // Free all usable pages except:
    // - Pages 0-511 (first 2MB: page tables, real-mode IVT, etc.)
    // - Pages used by the Page array itself (at 2MB = page 512)
    size_t page_array_start = phys_to_idx(page_array_phys);
    size_t page_array_end = page_array_start + pages_array_pages;
    for (size_t r = 0; r < usable_count; r++) {
        uint64_t base = usable[r].base;
        uint64_t end = base + usable[r].length;
        size_t start_idx = phys_to_idx((base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        size_t end_idx = phys_to_idx(end & ~(PAGE_SIZE - 1));

        // Skip pages occupied by Page array and bitmap (first ~5MB)
        if (start_idx < page_array_end + 128) start_idx = page_array_end + 128;

        // Free at maximum possible order
        size_t i = start_idx;
        while (i < end_idx) {
            for (int order = BUDDY_MAX_ORDER; order >= 0; order--) {
                size_t block_size = 1ULL << order;
                if (i + block_size <= end_idx && (i & (block_size - 1)) == 0) {
                    g_pages[i].order = order;
                    g_pages[i].next = g_free_lists[order];
                    g_free_lists[order] = &g_pages[i];
                    g_free_pages += block_size;
                    i += block_size;
                    break;
                }
            }
        }
    }
}

void* buddy_alloc_pages(size_t order) {
    if (order > BUDDY_MAX_ORDER) return nullptr;

    // Find smallest adequate order with a free block
    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (g_free_lists[o]) {
            Page* block = g_free_lists[o];
            g_free_lists[o] = block->next;
            size_t idx = block - g_pages;
            g_free_pages -= (1ULL << o);
            split(idx, o, order);
            return reinterpret_cast<void*>(idx_to_phys(idx));
        }
    }
    return nullptr;
}

void buddy_free_pages(void* phys_addr, size_t order) {
    if (!phys_addr || order > BUDDY_MAX_ORDER) return;
    size_t idx = phys_to_idx(reinterpret_cast<uint64_t>(phys_addr));
    g_pages[idx].order = static_cast<int>(order);
    coalesce(idx, order);
}

size_t buddy_free_page_count() { return g_free_pages; }
size_t buddy_total_pages() { return g_total_pages; }
