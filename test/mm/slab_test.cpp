#include <gtest/gtest.h>
#include <stdlib.h>
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"

// Simulate 16MB of physical memory starting at 1MB.
static constexpr uint64_t SIM_PHYS_BASE = 0x100000;
static constexpr size_t SIM_MEM_SIZE = 16 * 1024 * 1024; // 16 MB

struct TestState {
    uint8_t* sim_mem;
    uint64_t hhdm_offset;
};
static TestState g_ts = {nullptr, 0};

static void setup() {
    g_ts.sim_mem = static_cast<uint8_t*>(malloc(SIM_MEM_SIZE));
    g_ts.hhdm_offset = reinterpret_cast<uint64_t>(g_ts.sim_mem) - SIM_PHYS_BASE;
    MemRange ranges[] = {
        {SIM_PHYS_BASE, SIM_MEM_SIZE, MEMMAP_USABLE},
    };
    pmm_init(ranges, 1, 0, 0);
    bitmap_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x100000);
    buddy_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x400000);
    slab_init(g_ts.hhdm_offset);
}

static void teardown() {
    free(g_ts.sim_mem);
    g_ts.sim_mem = nullptr;
}

// Helpers: since the stub has no ASSERT_NE, return bool for early-out.
static bool check_ptr(void* ptr) {
    if (!ptr) {
        // can't printf easily, just track via EXPECT_TRUE
        EXPECT_TRUE(false);
        return false;
    }
    return true;
}

TEST(SlabTest, KmAlloc32) {
    setup();
    void* p = kmalloc(32);
    if (check_ptr(p)) {
        kfree(p);
    }
    teardown();
}

TEST(SlabTest, KmAlloc16) {
    setup();
    void* p = kmalloc(16);
    if (check_ptr(p)) {
        kfree(p);
    }
    teardown();
}

TEST(SlabTest, AllocAllSizes) {
    setup();
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    void* ptrs[8] = {};
    bool ok = true;
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            ok = false;
            EXPECT_TRUE(false);
            break;
        }
    }
    if (ok) {
        for (int i = 0; i < 8; i++) {
            kfree(ptrs[i]);
        }
    }
    teardown();
}

TEST(SlabTest, AllocTooLarge) {
    setup();
    void* p = kmalloc(4096);
    EXPECT_TRUE(p == nullptr);
    teardown();
}

TEST(SlabTest, ReuseAfterFree) {
    setup();
    void* a = kmalloc(64);
    void* b = kmalloc(64);
    if (!a || !b) {
        EXPECT_TRUE(false);
        teardown();
        return;
    }
    kfree(a);
    void* c = kmalloc(64);
    EXPECT_TRUE(c == a);  // should reuse the freed slot
    if (c) {
        kfree(b);
        kfree(c);
    } else {
        kfree(b);
    }
    teardown();
}

TEST(SlabTest, WriteAndRead) {
    setup();
    int* p = static_cast<int*>(kmalloc(sizeof(int) * 10));
    if (!p) {
        EXPECT_TRUE(false);
        teardown();
        return;
    }
    for (int i = 0; i < 10; i++) p[i] = i * 42;
    for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], i * 42);
    kfree(p);
    teardown();
}

TEST(SlabTest, UsableSize) {
    setup();
    void* p = kmalloc(100);
    if (!p) {
        EXPECT_TRUE(false);
        teardown();
        return;
    }
    size_t usable = kmalloc_usable_size(p);
    EXPECT_TRUE(usable >= 100);
    EXPECT_TRUE(usable <= 128);
    kfree(p);
    teardown();
}

TEST(SlabTest, NullFree) {
    setup();
    kfree(nullptr); // must not crash
    teardown();
}

TEST(SlabTest, AllocManyAndFree) {
    setup();
    constexpr int COUNT = 100;
    void* ptrs[COUNT];
    bool ok = true;
    for (int i = 0; i < COUNT; i++) {
        ptrs[i] = kmalloc(128);
        if (!ptrs[i]) {
            ok = false;
            EXPECT_TRUE(false);
            break;
        }
    }
    if (ok) {
        for (int i = 0; i < COUNT; i++) {
            kfree(ptrs[i]);
        }
        // Allocate again to verify state after free
        for (int i = 0; i < COUNT; i++) {
            ptrs[i] = kmalloc(128);
            if (!ptrs[i]) {
                ok = false;
                EXPECT_TRUE(false);
                break;
            }
        }
        if (ok) {
            for (int i = 0; i < COUNT; i++) {
                kfree(ptrs[i]);
            }
        }
    }
    teardown();
}
