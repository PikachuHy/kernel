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
void blkdev_register(BlockDevice* dev);

// Find a block device by name.
BlockDevice* blkdev_find(const char* name);

// Read/write with buffer cache. count = number of sectors.
int blkdev_read(BlockDevice* dev, uint64_t lba, void* buf, size_t count);
int blkdev_write(BlockDevice* dev, uint64_t lba, const void* buf, size_t count);

// Flush all dirty cache entries for a device.
void blkdev_flush(BlockDevice* dev);
