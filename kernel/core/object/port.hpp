#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

class Channel;

class Port : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Port;

    Port() : KernelObject(Type::Port) {}

    auto Accept(handle_t* out_channel) -> int;

    static auto Connect(Port* port, HandleTable& handles, handle_t* out_client_chan) -> int;

private:
    SpinLock lock_;
    struct Conn {
        handle_t channel;
        Conn*    next;
    };
    Conn* head_ = nullptr;
    Conn* tail_ = nullptr;
};

// Name Registry (global)
auto port_register_name(const char* name, Port* port) -> void;
auto port_lookup_name(const char* name) -> Port*;
