#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

class Channel : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Channel;

    Channel() : KernelObject(Type::Channel) {}

    struct Message {
        uint8_t*  data;
        size_t    data_len;
        handle_t* handles;
        size_t    num_handles;
        Message*  next;
    };

    // Dual-queue channel: each endpoint has its own receive queue.
    //   from_endpoint_b=false  → writing from endpoint A (enqueues on B's rx)
    //   from_endpoint_b=true   → writing from endpoint B (enqueues on A's rx)
    //
    // For reads:
    //   from_endpoint_b=false  → reading from endpoint A (dequeues from A's rx)
    //   from_endpoint_b=true   → reading from endpoint B (dequeues from B's rx)
    //
    // Kernel code that holds a Channel* directly (no handle) passes
    // from_endpoint_b=false, treating itself as endpoint A.
    auto Write(const void* data, size_t len,
               const handle_t* handles, size_t num_handles,
               bool from_endpoint_b = false) -> int;
    auto Read(void* buf, size_t buf_size, size_t* out_len,
              handle_t* handle_buf, size_t buf_capacity,
              size_t* out_num_handles,
              bool from_endpoint_b = false) -> int;

private:
    SpinLock lock_;

    // ── Per-endpoint queues ──────────────────────────────────────────
    // queue_a_ holds messages written by endpoint B → read by endpoint A.
    // queue_b_ holds messages written by endpoint A → read by endpoint B.
    Message* head_a_ = nullptr;   // rx queue for endpoint A
    Message* tail_a_ = nullptr;
    Message* head_b_ = nullptr;   // rx queue for endpoint B
    Message* tail_b_ = nullptr;

    auto Enqueue(Message* msg, Message** head, Message** tail) -> void;
    auto Dequeue(Message** head, Message** tail) -> Message*;
};
