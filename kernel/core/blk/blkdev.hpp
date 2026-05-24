#pragma once
#include <stdint.h>
#include <stddef.h>

struct BlockDevice {
    char     name[32];
    uint64_t total_sectors;
    uint32_t sector_size;       // typically 512

    // Read/write count sectors starting at LBA. Returns 0 on success, negative on error.
    int (*read)(BlockDevice* dev, uint64_t lba, void* buf, size_t count);
    int (*write)(BlockDevice* dev, uint64_t lba, const void* buf, size_t count);

    // Internal driver data (e.g., AHCI port pointer)
    void* driver_data;

    BlockDevice* next;
};

// Register a block device (add to global list).
auto blkdev_register(BlockDevice* dev) -> void;

// Find a block device by name.
auto blkdev_find(const char* name) -> BlockDevice*;

// Read/write with buffer cache. count = number of sectors.
auto blkdev_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count) -> int;
auto blkdev_write(BlockDevice* dev, uint64_t lba, const void* buf, size_t count) -> int;

// Flush all dirty cache entries for a device.
auto blkdev_flush(BlockDevice* dev) -> void;
