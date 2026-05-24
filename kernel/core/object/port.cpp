#include <stddef.h>
#include "kernel/core/object/port.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/mm/slab.hpp"

// Placement new (freestanding — no <new> header provided by our cross toolchain)
inline void* operator new(size_t, void* p) noexcept { return p; }

auto Port::Accept(handle_t* out_channel) -> int {
    lock_.lock();
    Conn* conn = head_;
    if (conn) {
        head_ = conn->next;
        if (!head_) tail_ = nullptr;
    }
    lock_.unlock();

    if (!conn) return -2; // ERR_EMPTY

    *out_channel = conn->channel;
    kfree(conn);
    return 0;
}

auto Port::Connect(Port* port, HandleTable& handles, handle_t* out_client_chan) -> int {
    auto* ch = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!ch) return -1;
    new (ch) Channel();

    handle_t a = handles.Alloc(ch, Rights{.mask = Rights::Read | Rights::Write |
                                                 Rights::Duplicate | Rights::Transfer});  // endpoint A
    Rights rb = Rights{.mask = Rights::Read | Rights::Write |
                              Rights::Duplicate | Rights::Transfer};
    rb.mask |= Rights::ChannelEndpointB;
    handle_t b = handles.Alloc(ch, rb);  // endpoint B
    ch->Release(); // handles own the ref

    auto* conn = static_cast<Conn*>(kmalloc(sizeof(Conn)));
    if (!conn) {
        handles.Free(a);
        handles.Free(b);
        return -1;
    }
    conn->channel = b;
    conn->next = nullptr;

    port->lock_.lock();
    if (!port->head_) {
        port->head_ = port->tail_ = conn;
    } else {
        port->tail_->next = conn;
        port->tail_ = conn;
    }
    port->lock_.unlock();

    *out_client_chan = a;
    return 0;
}

// ── Name Registry ────────────────────────────────────────────────

namespace {
    constexpr int MAX_NAMES = 64;
    struct NameEntry {
        const char* name;
        Port*       port;
    };
    NameEntry g_names[MAX_NAMES];
    int g_name_count = 0;
    SpinLock g_name_lock;
}

auto port_register_name(const char* name, Port* port) -> void {
    g_name_lock.lock();
    if (g_name_count < MAX_NAMES) {
        g_names[g_name_count].name = name;
        g_names[g_name_count].port = port;
        g_name_count++;
    }
    g_name_lock.unlock();
}

auto port_lookup_name(const char* name) -> Port* {
    g_name_lock.lock();
    for (int i = 0; i < g_name_count; i++) {
        const char* n = g_names[i].name;
        const char* q = name;
        while (*n && *q && *n == *q) { n++; q++; }
        if (*n == '\0' && *q == '\0') {
            Port* p = g_names[i].port;
            g_name_lock.unlock();
            return p;
        }
    }
    g_name_lock.unlock();
    return nullptr;
}
