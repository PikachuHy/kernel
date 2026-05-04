#include <gtest/gtest.h>
#include <stdlib.h>
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"

// Simulate 16MB of physical memory starting at 0x100000 (1MB), like a real system.
static constexpr uint64_t SIM_PHYS_BASE = 0x100000;
static constexpr size_t SIM_MEM_SIZE = 16 * 1024 * 1024; // 16MB

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
    bitmap_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x100000);  // bitmap at 1MB into sim
    // Place Page array at 4MB into the simulated memory
    buddy_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x400000);
}

static void teardown() {
    free(g_ts.sim_mem);
    g_ts.sim_mem = nullptr;
}

TEST(BuddyTest, InitDoesNotCrash) {
    setup();
    EXPECT_TRUE(buddy_free_page_count() > 0);
    EXPECT_TRUE(buddy_total_pages() > 0);
    teardown();
}

TEST(BuddyTest, AllocSinglePage) {
    setup();
    void* page = buddy_alloc_pages(0);
    EXPECT_TRUE(page != nullptr);
    if (page) {
        size_t free_before = buddy_free_page_count();
        buddy_free_pages(page, 0);
        EXPECT_EQ(buddy_free_page_count(), free_before + 1);
    }
    teardown();
}

TEST(BuddyTest, AllocMultipleOrders) {
    setup();
    void* p0 = buddy_alloc_pages(0);
    void* p1 = buddy_alloc_pages(1);
    void* p2 = buddy_alloc_pages(2);
    EXPECT_TRUE(p0 != nullptr);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    if (p0 && p1) {
        uint64_t a0 = reinterpret_cast<uint64_t>(p0);
        uint64_t a1 = reinterpret_cast<uint64_t>(p1);
        EXPECT_TRUE(a1 >= a0 + 4096 || a0 >= a1 + 8192);
    }
    if (p1 && p2) {
        uint64_t a1 = reinterpret_cast<uint64_t>(p1);
        uint64_t a2 = reinterpret_cast<uint64_t>(p2);
        EXPECT_TRUE(a2 >= a1 + 8192 || a1 >= a2 + 16384);
    }
    teardown();
}

TEST(BuddyTest, FreeAndRealloc) {
    setup();
    void* p = buddy_alloc_pages(3);
    EXPECT_TRUE(p != nullptr);
    if (p) {
        buddy_free_pages(p, 3);
        void* p2 = buddy_alloc_pages(3);
        EXPECT_TRUE(p2 != nullptr);
        if (p2) {
            EXPECT_EQ(reinterpret_cast<uint64_t>(p),
                      reinterpret_cast<uint64_t>(p2));
        }
    }
    teardown();
}

TEST(BuddyTest, OOMReturnsNull) {
    setup();
    size_t total_4kb = buddy_free_page_count();
    bool ok = true;
    for (size_t i = 0; i < total_4kb; i++) {
        void* p = buddy_alloc_pages(0);
        if (p == nullptr) {
            ok = false;
            break;
        }
    }
    EXPECT_TRUE(ok);
    void* fail = buddy_alloc_pages(0);
    EXPECT_TRUE(fail == nullptr);
    teardown();
}

TEST(BuddyTest, CoalesceSiblings) {
    setup();
    void* a = buddy_alloc_pages(0);
    void* b = buddy_alloc_pages(0);
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    if (a && b) {
        buddy_free_pages(a, 0);
        buddy_free_pages(b, 0);
        void* c = buddy_alloc_pages(1);
        EXPECT_TRUE(c != nullptr);
    }
    teardown();
}
