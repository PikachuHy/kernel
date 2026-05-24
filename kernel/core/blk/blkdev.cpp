#include "kernel/core/blk/blkdev.hpp"
#include "kernel/core/blk/bufcache.hpp"

// Global linked list head
static BlockDevice* s_devices = nullptr;

auto blkdev_register(BlockDevice* dev) -> void {
    dev->next = s_devices;
    s_devices = dev;
}

auto blkdev_find(const char* name) -> BlockDevice* {
    for (BlockDevice* d = s_devices; d; d = d->next) {
        bool match = true;
        for (int i = 0; i < 32; i++) {
            if (d->name[i] != name[i]) { match = false; break; }
            if (d->name[i] == '\0') break;
        }
        if (match) return d;
    }
    return nullptr;
}

auto blkdev_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count) -> int {
    return bufcache_read(dev, lba, buf, count);
}

auto blkdev_write(BlockDevice* dev, uint64_t lba, const void* buf, size_t count) -> int {
    return bufcache_write(dev, lba, buf, count);
}

auto blkdev_flush(BlockDevice* dev) -> void {
    bufcache_flush(dev);
}
