#include "kernel/core/object/channel.hpp"
#include "kernel/core/mm/slab.hpp"

// Placement new: use system <new> when available, otherwise provide our own.
#if __has_include(<new>)
#include <new>
#else
inline void* operator new(size_t, void* p) noexcept { return p; }
#endif

#include "kernel/lib/scoped_mem.hpp"
#include "kernel/lib/bitwise.hpp"

auto Channel::Enqueue(Message* msg, Message** head, Message** tail) -> void {
    msg->next = nullptr;
    if (!*head) {
        *head = *tail = msg;
    } else {
        (*tail)->next = msg;
        *tail = msg;
    }
}

auto Channel::Dequeue(Message** head, Message** tail) -> Channel::Message* {
    if (!*head) return nullptr;
    Message* msg = *head;
    *head = (*head)->next;
    if (!*head) *tail = nullptr;
    return msg;
}

auto Channel::Write(const void* data, size_t len,
                    const handle_t* handles, size_t num_handles,
                    bool from_endpoint_b) -> int {
    auto* raw = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (!raw) return -1;

    // Placement-new construct Message in allocated memory
    new (raw) Message{};

    raw->data_len = len;
    raw->num_handles = num_handles;

    if (len > 0) {
        auto* buf = static_cast<uint8_t*>(kmalloc(len));
        if (!buf) {
            raw->~Message();
            kfree(raw);
            return -1;
        }
        raw->data_mem = km::ScopedMem{buf};
        km::copy_bytes(buf, static_cast<const uint8_t*>(data), len);
    }

    if (num_handles > 0) {
        auto* hbuf = static_cast<handle_t*>(kmalloc(sizeof(handle_t) * num_handles));
        if (!hbuf) {
            raw->~Message();
            kfree(raw);
            return -1;
        }
        raw->handle_mem = km::ScopedMem{hbuf};
        raw->handles = hbuf;
        km::copy_bytes(hbuf, handles, num_handles);
    }

    lock_.lock();
    if (from_endpoint_b) Enqueue(raw, &head_a_, &tail_a_);
    else                 Enqueue(raw, &head_b_, &tail_b_);
    lock_.unlock();

    return 0;
}

auto Channel::Read(void* buf, size_t buf_size, size_t* out_len,
                   handle_t* handle_buf, size_t buf_capacity,
                   size_t* out_num_handles,
                   bool from_endpoint_b) -> int {
    lock_.lock();
    auto* msg = from_endpoint_b
        ? Dequeue(&head_b_, &tail_b_)
        : Dequeue(&head_a_, &tail_a_);
    lock_.unlock();

    if (!msg) return -2;

    if (out_len) *out_len = msg->data_len;

    if (buf && msg->data_mem.get() && msg->data_len > 0) {
        auto copy_len = msg->data_len < buf_size ? msg->data_len : buf_size;
        km::copy_bytes(static_cast<uint8_t*>(buf),
                       static_cast<const uint8_t*>(msg->data_mem.get()), copy_len);
    }

    if (out_num_handles) *out_num_handles = msg->num_handles;
    if (handle_buf && msg->handles && msg->num_handles > 0) {
        auto copy_len = msg->num_handles < buf_capacity ? msg->num_handles : buf_capacity;
        km::copy_bytes(handle_buf, msg->handles, copy_len);
    }

    // ScopedMem destructors in ~Message() free data and handle buffers
    msg->~Message();
    kfree(msg);
    return 0;
}
