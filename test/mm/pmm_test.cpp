#include <gtest/gtest.h>
#include "kernel/core/mm/pmm.hpp"

TEST(PmmTest, SingleUsableRange) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},
    };
    pmm_init(ranges, 1, 0x200000, 0x300000);
    EXPECT_EQ(pmm_usable_memory(), 0x100000);
    EXPECT_EQ(pmm_total_memory(), 0x100000);
}

TEST(PmmTest, KernelCarvesUsableRange) {
    MemRange ranges[] = {
        {0x0, 0x500000, MEMMAP_USABLE},
    };
    pmm_init(ranges, 1, 0x200000, 0x300000);
    EXPECT_EQ(pmm_usable_memory(), 0x400000);
    EXPECT_EQ(pmm_total_memory(), 0x500000);
}

TEST(PmmTest, ReservedNotUsable) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},
        {0x200000, 0x100000, MEMMAP_RESERVED},
        {0x300000, 0x100000, MEMMAP_BOOTLOADER_RECLAIMABLE},
    };
    pmm_init(ranges, 3, 0, 0);
    EXPECT_EQ(pmm_usable_memory(), 0x200000);
    EXPECT_EQ(pmm_total_memory(), 0x300000);
}

TEST(PmmTest, UsableRangesPointer) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},
        {0x300000, 0x100000, MEMMAP_USABLE},
    };
    pmm_init(ranges, 2, 0, 0);
    size_t count = 0;
    const MemRange* ur = pmm_usable_ranges(&count);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(ur[0].base, 0x100000);
    EXPECT_EQ(ur[1].base, 0x300000);
}

TEST(PmmTest, IsUsableCheck) {
    MemRange ranges[] = {
        {0x100000, 0x200000, MEMMAP_USABLE},
        {0x500000, 0x100000, MEMMAP_RESERVED},
    };
    pmm_init(ranges, 2, 0x180000, 0x200000);
    EXPECT_TRUE(pmm_is_usable(0x100000, 0x1000));
    EXPECT_TRUE(pmm_is_usable(0x200000, 0x1000));
    EXPECT_FALSE(pmm_is_usable(0x180000, 0x1000));
    EXPECT_FALSE(pmm_is_usable(0x500000, 0x1000));
}
