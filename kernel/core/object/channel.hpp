#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

class Channel : public KernelObject {
public:
    Channel() : KernelObject(Type::Channel) {}

    struct Message {
        uint8_t*  data;
        size_t    data_len;
        handle_t* handles;
        size_t    num_handles;
        Message*  next;
    };

    int Write(const void* data, size_t len,
              const handle_t* handles, size_t num_handles);
    int Read(void* buf, size_t buf_size, size_t* out_len,
             handle_t* handle_buf, size_t buf_capacity, size_t* out_num_handles);

private:
    SpinLock lock_;
    Message* head_ = nullptr;
    Message* tail_ = nullptr;

    void Enqueue(Message* msg);
    Message* Dequeue();
};
