# Phase 6: Object Manager + IPC Design

## Overview

Phase 6 implements the Object Manager and IPC primitives — the architectural backbone of the object-based hybrid kernel. Every kernel resource is a typed object accessed via opaque handles with capability rights. Channels provide point-to-point message passing and Ports provide many-to-one service endpoints. All inter-component communication (kernel↔kernel, kernel↔user, user↔user) flows through this uniform IPC fabric.

## Scope

Object Manager + IPC only. No Process object or VMM (deferred to Phase 7). A global handle table serves as a placeholder until per-process tables in Phase 7.

## Object Manager

### KernelObject Base Class

Intrusive reference-counted base. Each object type is a C++ class inheriting from `KernelObject`.

```cpp
class KernelObject {
public:
    enum class Type : uint8_t {
        Channel, Port, Process, Thread, VMO, Interrupt, Resource
    };

    Type type() const { return type_; }
    uint32_t refcount() const { return ref_count_; }

    void AddRef() { ref_count_++; }
    void Release();  // decrements, destroys `this` when refcount hits 0

protected:
    explicit KernelObject(Type t) : type_(t), ref_count_(1) {}
    virtual ~KernelObject() = default;

private:
    Type type_;
    uint32_t ref_count_;
};
```

No virtual dispatch for operations yet — type-tag switching in the syscall layer is sufficient for Phase 6. Virtual methods (e.g., `vmo_map`) are added per-object-type in later phases.

### Handle Table

Global table for Phase 6 (moves into Process in Phase 7):

```
MAX_HANDLES = 1024
handle_t = uint32_t (0 = invalid)
```

```cpp
struct HandleEntry {
    KernelObject* obj = nullptr;
    Rights rights{};
};

handle_t handle_alloc(KernelObject* obj, Rights rights);
void    handle_free(handle_t h);
KernelObject* handle_lookup(handle_t h, Rights* out_rights = nullptr);
```

- Protected by spinlock
- Flat array with free-list of unused slots
- `handle_lookup` returns nullptr if handle is invalid or rights are insufficient

### Rights

```cpp
struct Rights {
    enum Bit : uint32_t {
        Read      = 1 << 0,
        Write     = 1 << 1,
        Duplicate = 1 << 2,
        Transfer  = 1 << 3,
        // Deferred to Phase 7+:
        // Map, Execute, Enumerate
    };
    uint32_t mask = 0;
};
```

- Each handle carries a rights mask
- `handle_duplicate` can only downgrade rights, never upgrade
- Handle transfer over channels: sender loses the handle, receiver gets it with the same rights
- Default rights table per object type (e.g., Channel always gets Read|Write|Duplicate|Transfer)

### Syscall Dispatch

The existing syscall entry/exit (LSTAR, `syscall_entry.S`) dispatches to:

```cpp
extern "C" uint64_t syscall_handler(uint64_t num, uint64_t a1,
                                     uint64_t a2, uint64_t a3, uint64_t a4);
```

Phase 6 syscalls:

| Number | Name | Signature |
|--------|------|-----------|
| 0 | `debug_print` | (const char* msg) → 0 |
| 1 | `handle_close` | (handle_t h) → 0 |
| 2 | `handle_duplicate` | (handle_t h, Rights rights) → handle_t |
| 10 | `channel_create` | () → (handle_t a, handle_t b) |
| 11 | `channel_write` | (handle_t h, void* data, size_t len, handle_t* handles, size_t n) → 0 |
| 12 | `channel_read` | (handle_t h, void* buf, size_t bufsz, handle_t* handles, size_t* n) → size_t |
| 20 | `port_create` | () → handle_t |
| 21 | `port_register` | (handle_t h, const char* name) → 0 |
| 22 | `port_connect` | (const char* name, handle_t chan) → handle_t |
| 23 | `port_accept` | (handle_t h) → handle_t |

Each syscall looks up the handle, checks rights, then dispatches to object methods.

## IPC Primitives

### Channel

Bidirectional, point-to-point, FIFO. Pairs of handles — writing to one end appears on the other.

```cpp
class Channel : public KernelObject {
public:
    Channel() : KernelObject(Type::Channel) {}

    int Write(const void* data, size_t len, handle_t* handles, size_t num_handles);
    int Read(void* buf, size_t buf_size, size_t* out_len,
             handle_t* handle_buf, size_t* out_num_handles);

private:
    SpinLock lock_;
    struct Message {
        uint8_t* data;
        size_t   data_len;
        handle_t* handles;
        size_t   num_handles;
        Message* next;
    };
    Message* head_ = nullptr;
    Message* tail_ = nullptr;
};
```

- `channel_create()` returns `(a, b)` — each end gets one handle
- Messages are copied via `kmalloc` (zero-copy via VMO comes in Phase 7)
- Handle transfer: sender's handles are removed from sender's table, inserted into receiver's table on `channel_read`
- v0: non-blocking — returns `ERR_EMPTY`/`ERR_FULL`

### Port

Many-to-one service endpoint. Server holds a port, clients connect to it by name.

```cpp
class Port : public KernelObject {
public:
    Port() : KernelObject(Type::Port) {}

    int Accept(handle_t* out_channel);

    static int Connect(Port* port, handle_t* out_channel);

private:
    SpinLock lock_;
    struct Conn {
        handle_t channel;
        Conn* next;
    };
    Conn* head_ = nullptr;
    Conn* tail_ = nullptr;
};
```

- `port_create()` → single handle
- `port_register(name, port)` → registers in global name registry
- `port_connect(name, client_chan)` → creates channel pair, sends one end to port, returns other end to client
- `port_accept(port)` → server gets next connected channel

### Name Registry

Flat `const char*` → Port mapping, global array, spinlock-protected. No hierarchical namespacing in Phase 6.

## Demo

Two scheduler threads communicate via IPC:

```
Thread "ipc-server":
  port = port_create()
  port_register("demo", port)
  chan = port_accept(port)
  channel_read(chan, buf) → "hello kernel IPC!"
  channel_write(chan, "ACK")

Thread "ipc-client":
  port_wait("demo")               // demo-internal polling helper, not a syscall
  chan = port_connect("demo")
  channel_write(chan, "hello kernel IPC!")
  channel_read(chan, reply) → "ACK"
```

Expected boot output:
```
[server] got: hello kernel IPC!
[client] reply: ACK
```

## File Structure

```
kernel/core/object/
├── object.hpp           # KernelObject base, Type enum, refcounting
├── object.cpp
├── handle_table.hpp     # Handle table
├── handle_table.cpp
├── rights.hpp           # Rights bitmask
├── channel.hpp          # Channel class
├── channel.cpp
├── port.hpp             # Port class + name registry
├── port.cpp
├── BUILD.bazel
kernel/arch/x86_64/
├── syscall.cpp          # Modified: dispatch table + handlers
test/object/
├── BUILD.bazel
├── object_test.cpp
├── channel_test.cpp
├── port_test.cpp
```

## Prerequisites

- **SpinLock**: A simple TAS (test-and-set) spinlock. Used by handle table, Channel, and Port. ~10 lines of code, implemented as part of Task 1.
- **No allocations larger than 2048 bytes**: Channel message data uses `kmalloc`, so messages are limited to 2048 bytes per write. Larger payloads come via VMO in Phase 7.

## Dependencies

- **Incoming**: Phase 1 (boot, IDT), Phase 2 (kmalloc/slab), Phase 3 (syscall entry), Phase 5 (scheduler, threads)
- **Outgoing**: Phase 7 (VMM + Process objects), Phase 8 (VFS built on IPC)
