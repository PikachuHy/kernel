#include "kernel/core/object/channel.hpp"
#include "kernel/core/mm/slab.hpp"

void Channel::Enqueue(Message* msg) {
    msg->next = nullptr;
    if (!head_) {
        head_ = tail_ = msg;
    } else {
        tail_->next = msg;
        tail_ = msg;
    }
}

Channel::Message* Channel::Dequeue() {
    if (!head_) return nullptr;
    Message* msg = head_;
    head_ = head_->next;
    if (!head_) tail_ = nullptr;
    return msg;
}

int Channel::Write(const void* data, size_t len,
                    const handle_t* handles, size_t num_handles) {
    Message* msg = static_cast<Message*>(kmalloc(sizeof(Message)));
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

    lock_.lock();
    Enqueue(msg);
    lock_.unlock();
    return 0;
}

int Channel::Read(void* buf, size_t buf_size, size_t* out_len,
                  handle_t* handle_buf, size_t buf_capacity,
                  size_t* out_num_handles) {
    lock_.lock();
    Message* msg = Dequeue();
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
