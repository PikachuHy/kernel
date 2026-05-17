#pragma once
#include <stdint.h>
#include <stddef.h>

struct BlockDevice;

constexpr size_t BUF_CACHE_SIZE = 64;

struct BufCacheEntry {
    BlockDevice* dev;
    uint64_t     lba;
    uint8_t*     data;       // buddy_alloc_pages(0) = 4096 bytes (8 sectors)
    bool         dirty;
    uint64_t     access_time; // for LRU
};

void bufcache_init();
int  bufcache_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count);
int  bufcache_write(BlockDevice* dev, uint64_t lba, const void* buf, size_t count);
void bufcache_flush(BlockDevice* dev);
