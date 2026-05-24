#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

constexpr int NUM_CACHES = 8;
constexpr size_t CACHE_SIZES[NUM_CACHES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct SlabCache;

struct Slab {
    Slab* next;
    void* freelist;
    size_t obj_count;
    size_t free_count;
    SlabCache* cache;  // back-pointer for O(1) kfree
};

struct SlabCache {
    size_t obj_size;
    size_t objs_per_slab;
    Slab* slabs_partial;
    Slab* slabs_full;
    Slab* slabs_free;
};

SlabCache g_caches[NUM_CACHES];
uint64_t g_hhdm_offset = 0;

// Address translation helper using the stored offset
inline auto slab_phys_to_virt(uint64_t phys) -> void* {
    return reinterpret_cast<void*>(g_hhdm_offset + phys);
}

auto cache_index(size_t size) -> int {
    for (int i = 0; i < NUM_CACHES; i++) {
        if (size <= CACHE_SIZES[i]) return i;
    }
    return -1;
}

auto cache_init(SlabCache* cache, size_t obj_size) -> void {
    cache->obj_size = obj_size;
    size_t effective = obj_size < sizeof(void*) ? sizeof(void*) : obj_size;
    cache->objs_per_slab = (PAGE_SIZE - sizeof(Slab)) / effective;
    cache->slabs_partial = nullptr;
    cache->slabs_full = nullptr;
    cache->slabs_free = nullptr;
}

auto slab_create(SlabCache* cache) -> bool {
    void* page = buddy_alloc_pages(0);
    if (!page) return false;

    uint64_t phys = reinterpret_cast<uint64_t>(page);
    Slab* slab = static_cast<Slab*>(slab_phys_to_virt(phys));

    slab->next = nullptr;
    slab->obj_count = cache->objs_per_slab;
    slab->free_count = cache->objs_per_slab;
    slab->freelist = nullptr;
    slab->cache = cache;

    // Build freelist: objects start after the Slab header
    uint8_t* obj_base = reinterpret_cast<uint8_t*>(slab + 1);
    for (size_t i = 0; i < cache->objs_per_slab; i++) {
        void** slot = reinterpret_cast<void**>(obj_base + i * cache->obj_size);
        *slot = slab->freelist;
        slab->freelist = slot;
    }

    slab->next = cache->slabs_partial;
    cache->slabs_partial = slab;
    return true;
}

auto cache_alloc(SlabCache* cache) -> void* {
    // Prefer partially-filled slabs
    if (cache->slabs_partial) {
        Slab* slab = cache->slabs_partial;
        void* obj = slab->freelist;
        slab->freelist = *static_cast<void**>(obj);
        slab->free_count--;
        if (slab->free_count == 0) {
            // Remove from partial list, add to full list
            cache->slabs_partial = slab->next;
            slab->next = cache->slabs_full;
            cache->slabs_full = slab;
        }
        return obj;
    }

    // Try an empty slab from the free list
    if (cache->slabs_free) {
        Slab* slab = cache->slabs_free;
        cache->slabs_free = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
        return cache_alloc(cache);
    }

    // No slabs available -- create one from buddy
    if (!slab_create(cache)) return nullptr;
    return cache_alloc(cache);
}

} // namespace

auto slab_init(uint64_t hhdm_offset) -> void {
    g_hhdm_offset = hhdm_offset;
    for (int i = 0; i < NUM_CACHES; i++) {
        cache_init(&g_caches[i], CACHE_SIZES[i]);
    }
}

auto kmalloc(size_t size) -> void* {
    int ci = cache_index(size);
    if (ci < 0) return nullptr;
    return cache_alloc(&g_caches[ci]);
}

auto kfree(void* ptr) -> void {
    if (!ptr) return;

    // Find the slab header by page-aligning the virtual address
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    auto* slab = static_cast<Slab*>(reinterpret_cast<void*>(page_addr));
    SlabCache* cache = slab->cache;

    bool was_full = (slab->free_count == 0);

    // Push the object back onto the slab's freelist
    *static_cast<void**>(ptr) = slab->freelist;
    slab->freelist = ptr;
    slab->free_count++;

    if (was_full) {
        // Remove slab from full list
        Slab** prev = &cache->slabs_full;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        // Add to partial list
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }

    // If slab is completely free, move it to the free list
    if (slab->free_count == cache->objs_per_slab) {
        Slab** prev = &cache->slabs_partial;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        slab->next = cache->slabs_free;
        cache->slabs_free = slab;
    }
}

auto kmalloc_usable_size(void* ptr) -> size_t {
    if (!ptr) return 0;
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    auto* slab = static_cast<Slab*>(reinterpret_cast<void*>(page_addr));
    return slab->cache->obj_size;
}
