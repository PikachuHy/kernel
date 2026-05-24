#include "kernel/core/mm/pmm.hpp"

namespace {

constexpr size_t MAX_RANGES = 128;
MemRange g_usable[MAX_RANGES];
size_t g_usable_count = 0;
uint64_t g_total = 0;
uint64_t g_usable_mem = 0;
uint64_t g_highest = 0;

auto overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) -> bool {
    return a_start < b_end && b_start < a_end;
}

} // namespace

auto pmm_init(
    const MemRange* ranges,
    size_t count,
    uint64_t kernel_phys_start,
    uint64_t kernel_phys_end) -> void
{
    g_usable_count = 0;
    g_total = 0;
    g_usable_mem = 0;
    g_highest = 0;

    for (size_t i = 0; i < count && g_usable_count < MAX_RANGES; i++) {
        const auto& r = ranges[i];
        uint64_t end = r.base + r.length;
        g_total += r.length;

        if (r.type == MEMMAP_USABLE || r.type == MEMMAP_BOOTLOADER_RECLAIMABLE) {
            uint64_t base = r.base;
            uint64_t len = r.length;

            // Carve out the kernel's physical range
            if (overlaps(base, base + len, kernel_phys_start, kernel_phys_end)) {
                if (base < kernel_phys_start) {
                    uint64_t before_len = kernel_phys_start - base;
                    g_usable[g_usable_count++] = {base, before_len, 0};
                    g_usable_mem += before_len;
                    if (kernel_phys_start > g_highest) g_highest = kernel_phys_start;
                }
                uint64_t after = kernel_phys_end;
                if (after < base + len) {
                    uint64_t remain = (base + len) - after;
                    g_usable[g_usable_count++] = {after, remain, 0};
                    g_usable_mem += remain;
                    if (after + remain > g_highest) g_highest = after + remain;
                }
            } else {
                g_usable[g_usable_count++] = {base, len, 0};
                g_usable_mem += len;
                if (end > g_highest) g_highest = end;
            }
        }
    }
}

auto pmm_total_memory() -> uint64_t { return g_total; }
auto pmm_usable_memory() -> uint64_t { return g_usable_mem; }
auto pmm_highest_phys_addr() -> uint64_t { return g_highest; }

auto pmm_usable_ranges(size_t* out_count) -> const MemRange* {
    *out_count = g_usable_count;
    return g_usable;
}

auto pmm_is_usable(uint64_t phys_addr, uint64_t length) -> bool {
    for (size_t i = 0; i < g_usable_count; i++) {
        if (g_usable[i].base <= phys_addr &&
            phys_addr + length <= g_usable[i].base + g_usable[i].length) {
            return true;
        }
    }
    return false;
}
