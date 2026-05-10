#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

class Channel;

class Port : public KernelObject {
public:
    Port() : KernelObject(Type::Port) {}

    int Accept(handle_t* out_channel);

    static int Connect(Port* port, HandleTable& handles, handle_t* out_client_chan);

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
void    port_register_name(const char* name, Port* port);
Port*   port_lookup_name(const char* name);
