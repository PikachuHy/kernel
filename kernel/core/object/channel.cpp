#include "kernel/core/object/channel.hpp"
#include "kernel/core/mm/slab.hpp"

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
    auto* msg = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (!msg) return -1;

    msg->data = nullptr;
    msg->handles = nullptr;
    msg->data_len = len;
    msg->num_handles = num_handles;

    if (len > 0) {
        msg->data = static_cast<uint8_t*>(kmalloc(len));
        if (!msg->data) { kfree(msg); return -1; }
        for (size_t i = 0; i < len; i++) {
            msg->data[i] = static_cast<const uint8_t*>(data)[i];
        }
    }

    if (num_handles > 0) {
        msg->handles = static_cast<handle_t*>(
            kmalloc(sizeof(handle_t) * num_handles));
        if (!msg->handles) {
            if (msg->data) kfree(msg->data);
            kfree(msg);
            return -1;
        }
        for (size_t i = 0; i < num_handles; i++) {
            msg->handles[i] = handles[i];
        }
    }

    // Route: A writes → B's rx queue (queue_b_), B writes → A's rx queue (queue_a_)
    lock_.lock();
    if (from_endpoint_b) {
        Enqueue(msg, &head_a_, &tail_a_);
    } else {
        Enqueue(msg, &head_b_, &tail_b_);
    }
    lock_.unlock();
    return 0;
}

auto Channel::Read(void* buf, size_t buf_size, size_t* out_len,
                   handle_t* handle_buf, size_t buf_capacity,
                   size_t* out_num_handles,
                   bool from_endpoint_b) -> int {
    lock_.lock();
    Message* msg = from_endpoint_b
        ? Dequeue(&head_b_, &tail_b_)   // B reads from queue_b_ (messages from A)
        : Dequeue(&head_a_, &tail_a_);  // A reads from queue_a_ (messages from B)
    lock_.unlock();

    if (!msg) return -2;

    if (out_len) *out_len = msg->data_len;

    if (buf && msg->data && msg->data_len > 0) {
        size_t copy = msg->data_len < buf_size ? msg->data_len : buf_size;
        for (size_t i = 0; i < copy; i++) {
            static_cast<uint8_t*>(buf)[i] = msg->data[i];
        }
    }

    if (out_num_handles) *out_num_handles = msg->num_handles;
    if (handle_buf && msg->handles && msg->num_handles > 0) {
        size_t copy = msg->num_handles < buf_capacity
                          ? msg->num_handles : buf_capacity;
        for (size_t i = 0; i < copy; i++) {
            handle_buf[i] = msg->handles[i];
        }
    }

    if (msg->data) kfree(msg->data);
    if (msg->handles) kfree(msg->handles);
    kfree(msg);
    return 0;
}
