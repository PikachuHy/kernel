#include <gtest/gtest.h>
#include <stdlib.h>
#include "kernel/core/mm/vmo.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/core/mm/slab.hpp"

// Simulate 64MB of physical memory starting at 1MB.
static constexpr uint64_t SIM_PHYS_BASE = 0x100000;
static constexpr size_t SIM_MEM_SIZE = 64 * 1024 * 1024; // 64 MB

struct TestState {
    uint8_t* sim_mem;
    uint64_t hhdm_offset;
};
static TestState g_ts = {nullptr, 0};

static void setup() {
    g_ts.sim_mem = static_cast<uint8_t*>(malloc(SIM_MEM_SIZE));
    ASSERT_NE(g_ts.sim_mem, nullptr);
    g_ts.hhdm_offset = reinterpret_cast<uint64_t>(g_ts.sim_mem) - SIM_PHYS_BASE;
    MemRange ranges[] = {
        {SIM_PHYS_BASE, SIM_MEM_SIZE, MEMMAP_USABLE},
    };
    pmm_init(ranges, 1, 0, 0);
    bitmap_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x100000);
    buddy_init(g_ts.hhdm_offset, SIM_PHYS_BASE + 0x400000);
    slab_init(g_ts.hhdm_offset);
    Vmo::SetDirectMapOffset(g_ts.hhdm_offset);
}

static void teardown() {
    free(g_ts.sim_mem);
    g_ts.sim_mem = nullptr;
}

TEST(VmoTest, CreateAnonymous) {
    setup();
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 4);
    ASSERT_NE(vmo, nullptr);
    EXPECT_EQ(vmo->type(), Vmo::Anonymous);
    EXPECT_EQ(vmo->size(), PAGE_SIZE * 4);
    EXPECT_EQ(vmo->num_pages(), 4u);
    vmo->Release();
    teardown();
}

TEST(VmoTest, GetPageFirstAccess) {
    setup();
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    ASSERT_NE(vmo, nullptr);

    uint64_t pa = vmo->GetPage(0, false);
    EXPECT_NE(pa, 0u);
    // Page should be zero-filled
    uint64_t* data = reinterpret_cast<uint64_t*>(g_ts.hhdm_offset + pa);
    EXPECT_EQ(data[0], 0u);
    EXPECT_EQ(data[511], 0u);

    vmo->Release();
    teardown();
}

TEST(VmoTest, GetPageReturnsSamePage) {
    setup();
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    uint64_t pa1 = vmo->GetPage(0, false);
    uint64_t pa2 = vmo->GetPage(0, false);
    EXPECT_EQ(pa1, pa2);
    vmo->Release();
    teardown();
}

TEST(VmoTest, CloneCoWSharesPages) {
    setup();
    Vmo* parent = Vmo::CreateAnonymous(PAGE_SIZE * 2);
    uint64_t p0 = parent->GetPage(0, false);
    uint64_t p1 = parent->GetPage(PAGE_SIZE, false);

    Vmo* child = parent->CloneCoW();
    ASSERT_NE(child, nullptr);

    // Child sees same physical pages
    EXPECT_EQ(child->GetPage(0, false), p0);
    EXPECT_EQ(child->GetPage(PAGE_SIZE, false), p1);

    child->Release();
    parent->Release();
    teardown();
}

TEST(VmoTest, CowWriteBreaksSharing) {
    setup();
    Vmo* parent = Vmo::CreateAnonymous(PAGE_SIZE);
    uint64_t orig_pa = parent->GetPage(0, false);

    // Write something to the page so we can verify copy
    uint64_t* orig_data = reinterpret_cast<uint64_t*>(g_ts.hhdm_offset + orig_pa);
    orig_data[0] = 0xDEADBEEFULL;

    Vmo* child = parent->CloneCoW();

    // Write in child triggers COW — should get a DIFFERENT physical page
    uint64_t child_pa = child->GetPage(0, true);
    EXPECT_NE(child_pa, orig_pa);

    // New page has copied content
    uint64_t* child_data = reinterpret_cast<uint64_t*>(g_ts.hhdm_offset + child_pa);
    EXPECT_EQ(child_data[0], 0xDEADBEEFULL);

    // Parent still has original page
    EXPECT_EQ(parent->GetPage(0, false), orig_pa);

    child->Release();
    parent->Release();
    teardown();
}

TEST(VmoTest, PhysicalType) {
    setup();
    Vmo* vmo = Vmo::CreatePhysical(PAGE_SIZE * 2, 0x100000);
    ASSERT_NE(vmo, nullptr);
    EXPECT_EQ(vmo->type(), Vmo::Physical);
    EXPECT_EQ(vmo->GetPage(0, false), 0x100000u);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE, false), 0x101000u);
    vmo->Release();
    teardown();
}

TEST(VmoTest, OutOfRangeReturnsZero) {
    setup();
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE, false), 0u);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE * 10, false), 0u);
    vmo->Release();
    teardown();
}
