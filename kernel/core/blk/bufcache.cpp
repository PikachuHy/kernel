#include "kernel/core/blk/bufcache.hpp"
#include "kernel/core/blk/blkdev.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/arch/x86_64/paging.hpp"

// ── Static cache state ────────────────────────────────────────────────
static BufCacheEntry s_cache[BUF_CACHE_SIZE];
static uint64_t      s_access_counter = 0;

// ── Internal helpers ──────────────────────────────────────────────────

// Compute number of sectors per 4096-byte cache line for a device.
static inline auto sectors_per_line(BlockDevice* dev) -> size_t {
    return 4096 / dev->sector_size;
}

// Find the cache entry index for (dev, cache_lba), or return BUF_CACHE_SIZE.
static auto cache_lookup(BlockDevice* dev, uint64_t cache_lba) -> size_t {
    for (size_t i = 0; i < BUF_CACHE_SIZE; i++) {
        if (s_cache[i].dev == dev &&
            s_cache[i].lba == cache_lba &&
            s_cache[i].data != nullptr) {
            return i;
        }
    }
    return BUF_CACHE_SIZE;
}

// Find or create a cache slot.
// Returns index on success, BUF_CACHE_SIZE on OOM.
static auto cache_alloc_or_evict(void) -> size_t {
    // First pass: reuse an already-vacant slot.
    for (size_t i = 0; i < BUF_CACHE_SIZE; i++) {
        if (s_cache[i].dev == nullptr) {
            if (s_cache[i].data == nullptr) {
                void* phys = buddy_alloc_pages(0);
                if (!phys) return BUF_CACHE_SIZE;
                s_cache[i].data = reinterpret_cast<uint8_t*>(
                    DIRECT_MAP_BASE + reinterpret_cast<uint64_t>(phys));
            }
            return i;
        }
    }

    // All slots occupied -- find LRU victim.
    size_t victim = 0;
    uint64_t min_time = s_cache[0].access_time;
    for (size_t i = 1; i < BUF_CACHE_SIZE; i++) {
        if (s_cache[i].access_time < min_time) {
            min_time = s_cache[i].access_time;
            victim = i;
        }
    }

    // Write back the evicted entry if it is dirty.
    if (s_cache[victim].dirty && s_cache[victim].dev) {
        size_t n = sectors_per_line(s_cache[victim].dev);
        s_cache[victim].dev->write(
            s_cache[victim].dev,
            s_cache[victim].lba,
            s_cache[victim].data,
            n);
    }

    s_cache[victim].dev    = nullptr;
    s_cache[victim].dirty  = false;
    // Keep data buffer for reuse.
    return victim;
}

// ── Public API ────────────────────────────────────────────────────────

auto bufcache_init() -> void {
    for (size_t i = 0; i < BUF_CACHE_SIZE; i++) {
        s_cache[i].dev         = nullptr;
        s_cache[i].lba         = 0;
        s_cache[i].data        = nullptr;
        s_cache[i].dirty       = false;
        s_cache[i].access_time = 0;
    }
}

auto bufcache_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count) -> int {
    size_t spl = sectors_per_line(dev);

    for (size_t i = 0; i < count; i++) {
        uint64_t cur_lba  = lba + i;
        uint64_t base_lba = (cur_lba / spl) * spl;
        uint64_t boff     = (cur_lba % spl) * dev->sector_size;  // byte offset within cache line

        size_t idx = cache_lookup(dev, base_lba);
        if (idx == BUF_CACHE_SIZE) {
            idx = cache_alloc_or_evict();
            if (idx == BUF_CACHE_SIZE) return -1;  // OOM

            // Fill the cache line from the device.
            s_cache[idx].dev  = dev;
            s_cache[idx].lba  = base_lba;
            s_cache[idx].dirty = false;

            int ret = dev->read(dev, base_lba, s_cache[idx].data, spl);
            if (ret != 0) {
                s_cache[idx].dev = nullptr;
                return ret;
            }
        }

        // Copy the requested sector from the cache line.
        uint8_t* src = s_cache[idx].data + boff;
        uint8_t* dst = static_cast<uint8_t*>(buf) + i * dev->sector_size;
        for (size_t b = 0; b < dev->sector_size; b++) {
            dst[b] = src[b];
        }

        s_cache[idx].access_time = ++s_access_counter;
    }

    return 0;
}

auto bufcache_write(BlockDevice* dev, uint64_t lba, const void* buf, size_t count) -> int {
    size_t spl = sectors_per_line(dev);

    for (size_t i = 0; i < count; i++) {
        uint64_t cur_lba  = lba + i;
        uint64_t base_lba = (cur_lba / spl) * spl;
        uint64_t boff     = (cur_lba % spl) * dev->sector_size;

        // Write-through: push this single sector to the device immediately.
        const uint8_t* src = static_cast<const uint8_t*>(buf) + i * dev->sector_size;
        int ret = dev->write(dev, cur_lba, src, 1);
        if (ret != 0) return ret;

        // Update the cache if the line is already resident.
        size_t idx = cache_lookup(dev, base_lba);
        if (idx != BUF_CACHE_SIZE) {
            uint8_t* dst = s_cache[idx].data + boff;
            for (size_t b = 0; b < dev->sector_size; b++) {
                dst[b] = src[b];
            }
            s_cache[idx].dirty       = true;
            s_cache[idx].access_time = ++s_access_counter;
        }
    }

    return 0;
}

auto bufcache_flush(BlockDevice* dev) -> void {
    size_t spl = sectors_per_line(dev);

    for (size_t i = 0; i < BUF_CACHE_SIZE; i++) {
        if (s_cache[i].dev == dev && s_cache[i].dirty) {
            dev->write(dev, s_cache[i].lba, s_cache[i].data, spl);
            s_cache[i].dirty = false;
        }
    }
}
