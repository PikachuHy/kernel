#include <gtest/gtest.h>
#include "kernel/arch/x86_64/irq.hpp"

static int g_call_count = 0;
static uint8_t g_last_vector = 0;

static bool test_handler(uint8_t vector) {
    g_call_count++;
    g_last_vector = vector;
    return true;
}

static bool test_handler2(uint8_t vector) {
    g_call_count += 10;
    g_last_vector = vector;
    return true;
}

TEST(IrqTest, RegisterAndDispatch) {
    irq_init();
    g_call_count = 0;
    g_last_vector = 0;
    EXPECT_EQ(irq_register(1, test_handler), 0);
    irq_dispatch(33);
    EXPECT_EQ(g_call_count, 1);
    EXPECT_EQ(g_last_vector, 33);
}

TEST(IrqTest, SharedIrq) {
    irq_init();
    g_call_count = 0;
    irq_register(1, test_handler);
    irq_register(1, test_handler2);
    irq_dispatch(33);
    EXPECT_EQ(g_call_count, 11);
    EXPECT_EQ(g_last_vector, 33);
}

TEST(IrqTest, NoHandlerNoCrash) {
    irq_init();
    irq_dispatch(32);
    irq_dispatch(0);
    irq_dispatch(255);
}

TEST(IrqTest, TooManyHandlers) {
    irq_init();
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    EXPECT_EQ(irq_register(0, test_handler), -2);
}

TEST(IrqTest, NullHandler) {
    irq_init();
    EXPECT_EQ(irq_register(0, nullptr), -1);
}
