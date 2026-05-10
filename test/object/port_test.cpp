#include <gtest/gtest.h>
#include "kernel/core/object/port.hpp"
#include "kernel/core/object/channel.hpp"

// Stubs (same pattern as channel_test)
namespace { uint8_t g_mem[16384]; size_t g_used = 0; }
void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}
// HandleTable used by all tests (re-initialized via g_handle_used reset)
static HandleTable g_test_ht;

TEST(PortTest, CreateAndAccept) {
    g_used = 0;
    g_test_ht.Init();
    Port port;

    handle_t client_chan;
    EXPECT_EQ(Port::Connect(&port, g_test_ht, &client_chan), 0);
    EXPECT_NE(client_chan, INVALID_HANDLE);

    handle_t server_chan;
    EXPECT_EQ(port.Accept(&server_chan), 0);
    EXPECT_NE(server_chan, INVALID_HANDLE);
}

TEST(PortTest, AcceptEmpty) {
    g_used = 0;
    Port port;
    handle_t chan;
    EXPECT_EQ(port.Accept(&chan), -2);
}

TEST(PortTest, NameRegistry) {
    g_used = 0;
    Port port;
    port_register_name("test/echo", &port);
    EXPECT_EQ(port_lookup_name("test/echo"), &port);
    EXPECT_EQ(port_lookup_name("nonexistent"), nullptr);
}

TEST(PortTest, MultipleConnections) {
    g_used = 0;
    g_test_ht.Init();
    Port port;
    handle_t c1, c2, s1, s2;
    EXPECT_EQ(Port::Connect(&port, g_test_ht, &c1), 0);
    EXPECT_EQ(Port::Connect(&port, g_test_ht, &c2), 0);
    EXPECT_EQ(port.Accept(&s1), 0);
    EXPECT_EQ(port.Accept(&s2), 0);
    EXPECT_NE(s1, s2);
}
