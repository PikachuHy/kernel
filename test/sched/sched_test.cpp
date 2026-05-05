#include <gtest/gtest.h>
#include "kernel/core/sched/sched.hpp"

TEST(RunQueueTest, InitEmpty) {
    RunQueue rq;
    rq.init();
    EXPECT_EQ(rq.count, 0u);
    EXPECT_EQ(rq.bitmap, 0u);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, PushPopSingle) {
    RunQueue rq;
    rq.init();
    Thread t{};
    t.priority = 2;
    rq.push(&t);
    EXPECT_EQ(rq.count, 1u);
    EXPECT_EQ(rq.pop(), &t);
    EXPECT_EQ(rq.count, 0u);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, PushPopMultipleSamePriority) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{}, t3{};
    t1.priority = 3; t2.priority = 3; t3.priority = 3;
    rq.push(&t1);
    rq.push(&t2);
    rq.push(&t3);
    EXPECT_EQ(rq.count, 3u);

    Thread* a = rq.pop();
    Thread* b = rq.pop();
    Thread* c = rq.pop();
    // LIFO order within same priority
    EXPECT_EQ(a, &t3);
    EXPECT_EQ(b, &t2);
    EXPECT_EQ(c, &t1);
    EXPECT_EQ(rq.pop(), nullptr);
    EXPECT_EQ(rq.count, 0u);
}

TEST(RunQueueTest, PriorityOrdering) {
    RunQueue rq;
    rq.init();
    Thread t_low{}, t_mid{}, t_high{};
    t_low.priority  = 7;
    t_mid.priority  = 3;
    t_high.priority = 0;

    rq.push(&t_low);
    rq.push(&t_mid);
    rq.push(&t_high);

    EXPECT_EQ(rq.pop(), &t_high);
    EXPECT_EQ(rq.pop(), &t_mid);
    EXPECT_EQ(rq.pop(), &t_low);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, InterleavedPriorities) {
    RunQueue rq;
    rq.init();
    Thread t0a{}, t0b{}, t1a{}, t7a{};
    t0a.priority = 0; t0b.priority = 0;
    t1a.priority = 1;
    t7a.priority = 7;

    rq.push(&t7a);
    rq.push(&t1a);
    rq.push(&t0b);
    rq.push(&t0a);

    // t0a pushed last, at head of prio 0
    EXPECT_EQ(rq.pop(), &t0a);
    EXPECT_EQ(rq.pop(), &t0b);
    EXPECT_EQ(rq.pop(), &t1a);
    EXPECT_EQ(rq.pop(), &t7a);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, RemoveFromHead) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{}, t3{};
    t1.priority = 2; t2.priority = 2; t3.priority = 2;
    rq.push(&t1);
    rq.push(&t2);
    rq.push(&t3);

    rq.remove(&t3);
    EXPECT_EQ(rq.count, 2u);
    EXPECT_EQ(rq.pop(), &t2);
    EXPECT_EQ(rq.pop(), &t1);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, RemoveFromMiddle) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{}, t3{};
    t1.priority = 4; t2.priority = 4; t3.priority = 4;
    rq.push(&t1);
    rq.push(&t2);
    rq.push(&t3);

    rq.remove(&t2);
    EXPECT_EQ(rq.count, 2u);
    // After remove(t2): t3->t1
    EXPECT_EQ(rq.pop(), &t3);
    EXPECT_EQ(rq.pop(), &t1);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, RemoveOnlyElemClearsBitmap) {
    RunQueue rq;
    rq.init();
    Thread t{};
    t.priority = 5;
    rq.push(&t);
    EXPECT_TRUE((rq.bitmap & (1u << 5)) != 0u);

    rq.remove(&t);
    EXPECT_EQ(rq.count, 0u);
    EXPECT_EQ(rq.bitmap, 0u);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, StealFromBack) {
    RunQueue rq;
    rq.init();
    Thread t_low{}, t_high{};
    t_low.priority  = 7;
    t_high.priority = 0;

    rq.push(&t_low);
    rq.push(&t_high);

    Thread* stolen = rq.steal_one();
    EXPECT_EQ(stolen, &t_low);
    EXPECT_EQ(rq.count, 1u);
    EXPECT_EQ(rq.pop(), &t_high);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, StealEmpty) {
    RunQueue rq;
    rq.init();
    EXPECT_EQ(rq.steal_one(), nullptr);
}

TEST(ThreadTest, SizeWithinSlabLimit) {
    EXPECT_TRUE(sizeof(Thread) <= 2048u);
}

TEST(ThreadTest, RspOffset) {
    EXPECT_EQ(offsetof(Thread, rsp), 0u);
}

TEST(RunQueueTest, PriorityMask) {
    RunQueue rq;
    rq.init();
    Thread t{};
    t.priority = 9;
    rq.push(&t);
    EXPECT_EQ(rq.count, 1u);
    EXPECT_TRUE((rq.bitmap & (1u << 1)) != 0u);
    EXPECT_EQ(rq.pop(), &t);
}

TEST(RunQueueTest, StressManyThreads) {
    RunQueue rq;
    rq.init();
    Thread threads[32];
    for (int i = 0; i < 32; i++) {
        threads[i].priority = static_cast<uint8_t>(i % 8);
        rq.push(&threads[i]);
    }
    EXPECT_EQ(rq.count, 32u);

    int popped = 0;
    while (Thread* t = rq.pop()) {
        EXPECT_TRUE(t != nullptr);
        popped++;
    }
    EXPECT_EQ(popped, 32);
    EXPECT_EQ(rq.count, 0u);
    EXPECT_EQ(rq.bitmap, 0u);
}
