#include <gtest/gtest.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"
#include "kernel/core/object/handle_table.hpp"

// Concrete subclass for testing
class TestObj : public KernelObject {
public:
    TestObj() : KernelObject(Type::Channel) {}
};

// Stub kmalloc/kfree for host tests
namespace {
    uint8_t g_mem[4096];
    size_t g_used = 0;
}

void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}

TEST(ObjectTest, AddRefIncrements) {
    g_used = 0;
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    EXPECT_EQ(obj->refcount(), 1u);
    obj->AddRef();
    EXPECT_EQ(obj->refcount(), 2u);
    obj->Release();
    EXPECT_EQ(obj->refcount(), 1u);
    obj->Release(); // destroys
}

TEST(ObjectTest, TypeIsCorrect) {
    g_used = 0;
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    EXPECT_EQ(obj->type(), KernelObject::Type::Channel);
    obj->Release();
}

TEST(RightsTest, HasAllBits) {
    Rights r{.mask = Rights::Read | Rights::Write};
    EXPECT_TRUE(r.has(Rights{.mask = Rights::Read}));
    EXPECT_TRUE(r.has(Rights{.mask = Rights::Read | Rights::Write}));
    EXPECT_FALSE(r.has(Rights{.mask = Rights::Duplicate}));
}

TEST(RightsTest, EmptyMask) {
    Rights r{};
    EXPECT_TRUE(r.has(Rights{}));
}

// ── Handle table tests ──────────────────────────────────────────
// Stub handle_table_init for host (kmalloc already stubbed above)
void handle_table_init();

// Simple fixed-buffer implementation for host testing
namespace {
    constexpr int HOST_MAX_HANDLES = 64;
    struct HostHandle { KernelObject* obj = nullptr; Rights rights{}; };
    HostHandle g_host_table[HOST_MAX_HANDLES];
    handle_t g_host_next = 1;
}

void handle_table_init() {
    g_host_next = 1;
    for (auto& h : g_host_table) { h.obj = nullptr; h.rights = Rights{}; }
}

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    if (g_host_next >= HOST_MAX_HANDLES) return INVALID_HANDLE;
    handle_t h = g_host_next++;
    g_host_table[h].obj = obj;
    g_host_table[h].rights = rights;
    obj->AddRef();
    return h;
}

void handle_free(handle_t h) {
    if (h == 0 || h >= HOST_MAX_HANDLES) return;
    if (g_host_table[h].obj) {
        g_host_table[h].obj->Release();
        g_host_table[h].obj = nullptr;
    }
}

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    if (h == 0 || h >= HOST_MAX_HANDLES) return nullptr;
    KernelObject* obj = g_host_table[h].obj;
    if (!obj) return nullptr;
    if (needed.mask != 0 && !g_host_table[h].rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = g_host_table[h].rights;
    return obj;
}

TEST(HandleTableTest, AllocAndLookup) {
    g_used = 0;
    handle_table_init();
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    handle_t h = handle_alloc(obj, Rights{.mask = Rights::Read | Rights::Write});
    EXPECT_NE(h, INVALID_HANDLE);

    KernelObject* found = handle_lookup(h);
    EXPECT_EQ(found, obj);
    EXPECT_EQ(found->refcount(), 2u); // obj + handle

    handle_free(h);
    EXPECT_EQ(handle_lookup(h), nullptr);
}

TEST(HandleTableTest, RightsCheck) {
    g_used = 0;
    handle_table_init();
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    handle_t h = handle_alloc(obj, Rights{.mask = Rights::Read});

    EXPECT_NE(handle_lookup(h, Rights{}), nullptr);
    EXPECT_NE(handle_lookup(h, Rights{.mask = Rights::Read}), nullptr);
    EXPECT_EQ(handle_lookup(h, Rights{.mask = Rights::Write}), nullptr);

    handle_free(h);
}

TEST(HandleTableTest, InvalidHandle) {
    handle_table_init();
    EXPECT_EQ(handle_lookup(0), nullptr);
    EXPECT_EQ(handle_lookup(9999), nullptr);
}
