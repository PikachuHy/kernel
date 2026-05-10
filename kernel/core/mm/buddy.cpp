#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"

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

inline size_t buddy_idx(size_t idx, int order) {
    return idx ^ (1ULL << order);
}

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

int coalesce(size_t idx, int order) {
    while (order < BUDDY_MAX_ORDER) {
        size_t bud = buddy_idx(idx, order);
        if (bud >= g_total_pages) break;
        if (g_pages[bud].order != order) break;

        Page** prev = &g_free_lists[order];
        while (*prev != &g_pages[bud]) {
            prev = &(*prev)->next;
        }
        *prev = g_pages[bud].next;
        g_free_pages -= (1ULL << order);

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

// Static Page array in BSS. Sized for up to 1GB RAM (256K pages * 16 bytes = 4MB BSS).
constexpr size_t BUDDY_MAX_PAGES = 256 * 1024;  // 256K pages = 1GB physical
Page g_page_array[BUDDY_MAX_PAGES];

void buddy_init(uint64_t hhdm_offset, uint64_t page_array_phys) {
    (void)hhdm_offset;
    (void)page_array_phys;

    size_t usable_count;
    const MemRange* usable = pmm_usable_ranges(&usable_count);
    if (usable_count == 0) return;

    uint64_t max_phys = pmm_highest_phys_addr();
    g_total_pages = max_phys / PAGE_SIZE;
    if (g_total_pages > BUDDY_MAX_PAGES) g_total_pages = BUDDY_MAX_PAGES;

    g_pages = g_page_array;

    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        g_free_lists[i] = nullptr;
    }

    // Mark all pages as allocated initially
    for (size_t i = 0; i < g_total_pages; i++) {
        g_pages[i].order = -1;
        g_pages[i].next = nullptr;
    }

    g_free_pages = 0;

    // Free all usable pages.
    // Page tables allocated by bitmap are at very low addresses (<2MB).
    // Reserve pages 0-511 (first 2MB) to protect them.
    for (size_t r = 0; r < usable_count; r++) {
        uint64_t base = usable[r].base;
        uint64_t end = base + usable[r].length;
        size_t start_idx = phys_to_idx((base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        size_t end_idx = phys_to_idx(end & ~(PAGE_SIZE - 1));

        // Skip first 2MB (pages 0-511): IVT, BDA, page tables, etc.
        if (start_idx < 512) start_idx = 512;

        size_t i = start_idx;
        while (i < end_idx) {
            for (int order = BUDDY_MAX_ORDER; order >= 0; order--) {
                size_t block_size = 1ULL << order;
                if (i + block_size > end_idx) continue;
                if ((i & (block_size - 1)) != 0) continue;

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

void* buddy_alloc_pages(size_t order) {
    if (order > BUDDY_MAX_ORDER) return nullptr;

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
