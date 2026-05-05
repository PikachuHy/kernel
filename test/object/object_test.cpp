#include <gtest/gtest.h>
#include "kernel/core/object/object.hpp"

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
