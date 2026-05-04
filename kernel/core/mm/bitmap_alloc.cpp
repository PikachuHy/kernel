#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

uint64_t g_hhdm = 0;

// Bitmap: one bit per 4KB page. Sized for up to 4GB physical address space.
// 4GB / 4KB = 1M pages, 1M bits / 64 = 16K uint64_t entries = 128KB.
constexpr size_t MAX_PAGES = 1024 * 1024;
constexpr size_t BITMAP_U64S = MAX_PAGES / 64;
uint64_t g_bitmap_bss[BITMAP_U64S];

// Actual runtime values
uint64_t* g_bitmap = g_bitmap_bss;
size_t g_total_pages = 0;
size_t g_scan_start = 0;

inline bool bit_test(const uint64_t* bitmap, size_t idx) {
    return (bitmap[idx / 64] >> (idx % 64)) & 1;
}

inline void bit_set(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] |= (1ULL << (idx % 64));
}

inline void bit_clear(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

inline size_t phys_to_idx(uint64_t phys) { return phys / PAGE_SIZE; }
inline uint64_t idx_to_phys(size_t idx) { return idx * PAGE_SIZE; }

} // namespace

void bitmap_init(uint64_t hhdm_offset, uint64_t bitmap_base_phys) {
    g_hhdm = hhdm_offset;

    size_t usable_count;
    const MemRange* usable = pmm_usable_ranges(&usable_count);
    uint64_t max_phys = pmm_highest_phys_addr();

    g_total_pages = max_phys / PAGE_SIZE;
    if (g_total_pages > MAX_PAGES) g_total_pages = MAX_PAGES;

    // Mark ALL pages as used initially
    for (size_t i = 0; i < g_total_pages / 64 + 1; i++) {
        g_bitmap[i] = ~0ULL;
    }

    // Mark usable ranges as free
    for (size_t r = 0; r < usable_count; r++) {
        uint64_t start = usable[r].base;
        uint64_t end = start + usable[r].length;
        size_t start_idx = phys_to_idx(start);
        size_t end_idx = phys_to_idx((end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        for (size_t i = start_idx; i < end_idx && i < g_total_pages; i++) {
            bit_clear(g_bitmap, i);
        }
    }

    // Mark the bitmap's own pages as used
    size_t bitmap_bytes = (g_total_pages + 7) / 8;
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t bitmap_start_idx = phys_to_idx(bitmap_base_phys);
    for (size_t i = 0; i < bitmap_pages; i++) {
        bit_set(g_bitmap, bitmap_start_idx + i);
    }

    g_scan_start = 0;
}

void* bitmap_alloc_page() {
    // Scan forward from last position
    for (size_t i = g_scan_start; i < g_total_pages; i++) {
        if (!bit_test(g_bitmap, i)) {
            bit_set(g_bitmap, i);
            g_scan_start = i + 1;
            uint64_t phys = idx_to_phys(i);
            // Zero the page via HHDM
            uint8_t* ptr = reinterpret_cast<uint8_t*>(g_hhdm + phys);
            for (size_t j = 0; j < PAGE_SIZE; j++) ptr[j] = 0;
            return reinterpret_cast<void*>(phys);
        }
    }
    // Wrap around
    for (size_t i = 0; i < g_scan_start; i++) {
        if (!bit_test(g_bitmap, i)) {
            bit_set(g_bitmap, i);
            g_scan_start = i + 1;
            uint64_t phys = idx_to_phys(i);
            uint8_t* ptr = reinterpret_cast<uint8_t*>(g_hhdm + phys);
            for (size_t j = 0; j < PAGE_SIZE; j++) ptr[j] = 0;
            return reinterpret_cast<void*>(phys);
        }
    }
    return nullptr;  // OOM
}

void bitmap_free_page(void* phys_addr) {
    size_t idx = phys_to_idx(reinterpret_cast<uint64_t>(phys_addr));
    if (idx < g_total_pages) {
        bit_clear(g_bitmap, idx);
        if (idx < g_scan_start) g_scan_start = idx;
    }
}

bool bitmap_is_allocated(uint64_t phys_addr) {
    size_t idx = phys_to_idx(phys_addr);
    if (idx >= g_total_pages) return true;  // out of range = considered allocated
    return bit_test(g_bitmap, idx);
}

size_t bitmap_free_page_count() {
    size_t free = 0;
    for (size_t i = 0; i < g_total_pages; i++) {
        if (!bit_test(g_bitmap, i)) free++;
    }
    return free;
}

size_t bitmap_total_page_count() {
    return g_total_pages;
}
