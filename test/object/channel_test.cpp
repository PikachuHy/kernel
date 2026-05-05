#include <gtest/gtest.h>
#include "kernel/core/object/channel.hpp"

// Stubs for host testing
namespace {
    uint8_t g_mem[8192];
    size_t g_used = 0;
}
void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}

void handle_table_init() {}
handle_t handle_alloc(KernelObject*, Rights) { return 1; }
void handle_free(handle_t) {}

TEST(ChannelTest, WriteAndReadData) {
    g_used = 0;
    Channel ch;
    const char* msg = "hello";
    EXPECT_EQ(ch.Write(msg, 6, nullptr, 0), 0);

    char buf[32] = {};
    size_t len = 0;
    EXPECT_EQ(ch.Read(buf, sizeof(buf), &len, nullptr, 0, nullptr), 0);
    EXPECT_EQ(len, 6u);
    EXPECT_STREQ(buf, "hello");
}

TEST(ChannelTest, EmptyRead) {
    g_used = 0;
    Channel ch;
    size_t len;
    EXPECT_EQ(ch.Read(nullptr, 0, &len, nullptr, 0, nullptr), -2);
}

TEST(ChannelTest, FifoOrder) {
    g_used = 0;
    Channel ch;
    ch.Write("a", 2, nullptr, 0);
    ch.Write("b", 2, nullptr, 0);
    ch.Write("c", 2, nullptr, 0);

    char buf[4];
    size_t len;
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "a");
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "b");
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "c");
}
