# Phase 6: Object Manager + IPC Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Object Manager (KernelObject, handle table, rights) and IPC primitives (Channel, Port) with syscall dispatch, enabling thread-to-thread message passing.

**Architecture:** KernelObject intrusive-refcounted base class. Global handle table (placeholder until per-process in Phase 7). Rights bitmask per handle. Channel = bidirectional FIFO with handle transfer. Port = many-to-one service endpoint with flat name registry. Syscall dispatch via existing LSTAR entry point.

**Tech Stack:** C++26 freestanding, kmalloc/slab, SpinLock (new), existing syscall entry (syscall_entry.S)

**Prerequisites from prior phases:** kmalloc/slab (Phase 2), syscall entry + interrupt-safe dispatching (Phase 3), SMP-aware code (Phase 4), preemptive scheduler + threads (Phase 5)

---

## File Structure

```
kernel/core/object/          (NEW directory)
├── object.hpp               KernelObject base class, type enum, refcounting
├── object.cpp
├── rights.hpp               Rights bitmask type
├── handle_table.hpp         Global handle table
├── handle_table.cpp
├── channel.hpp              Channel class
├── channel.cpp
├── port.hpp                 Port class + name registry
├── port.cpp
├── BUILD.bazel

kernel/lib/spinlock.hpp      (NEW) SpinLock utility
kernel/lib/spinlock.cpp

kernel/arch/x86_64/
├── syscall.cpp              (MODIFIED) Add dispatch table + all syscall handlers
├── syscall.hpp              (MODIFIED) Add syscall number constants

kernel/BUILD.bazel           (MODIFIED) Add object deps

test/object/                 (NEW directory)
├── BUILD.bazel
├── object_test.cpp          Handle alloc/free/lookup, rights checks
├── channel_test.cpp         Create, write/read, FIFO order, handle transfer
└── port_test.cpp            Create, register, connect, accept

kernel/core/sched/BUILD.bazel (MODIFIED) Expose sched_hdrs for test stubs
```

---

### Task 1: SpinLock

**Files:**
- Create: `kernel/lib/spinlock.hpp`
- Create: `kernel/lib/spinlock.cpp`

SpinLock is needed by the handle table, Channel, and Port. Simple TAS (test-and-set) lock — no fancy backoff needed for Phase 6 (spin is fine with preemptive scheduler + SMP).

- [ ] **Step 1: Write spinlock.hpp**

```cpp
#pragma once
#include <stdint.h>

class SpinLock {
public:
    void lock();
    void unlock();
    bool try_lock();

private:
    volatile uint32_t locked_{0};
};
```

- [ ] **Step 2: Write spinlock.cpp**

```cpp
#include "kernel/lib/spinlock.hpp"

void SpinLock::lock() {
    while (__sync_lock_test_and_set(&locked_, 1)) {
        while (locked_) {
            asm volatile("pause");
        }
    }
}

void SpinLock::unlock() {
    __sync_lock_release(&locked_);
}

bool SpinLock::try_lock() {
    return __sync_lock_test_and_set(&locked_, 1) == 0;
}
```

- [ ] **Step 3: Add to kernel/lib/BUILD.bazel and commit**

Add `spinlock.hpp` and `spinlock.cpp` to the `klib` cc_library in `kernel/lib/BUILD.bazel`.

---

### Task 2: KernelObject + Rights

**Files:**
- Create: `kernel/core/object/object.hpp`
- Create: `kernel/core/object/object.cpp`
- Create: `kernel/core/object/rights.hpp`
- Test: `test/object/object_test.cpp` (partial — full tests after handle table)

- [ ] **Step 1: Write object.hpp**

```cpp
#pragma once
#include <stdint.h>

class KernelObject {
public:
    enum class Type : uint8_t {
        Channel,
        Port,
        // Future: Process, Thread, VMO, Interrupt, Resource
    };

    Type type() const { return type_; }
    uint32_t refcount() const { return ref_count_; }

    void AddRef() { ref_count_++; }
    void Release();

protected:
    explicit KernelObject(Type t) : type_(t), ref_count_(1) {}
    virtual ~KernelObject() = default;

private:
    Type type_;
    uint32_t ref_count_;
};
```

- [ ] **Step 2: Write rights.hpp**

```cpp
#pragma once
#include <stdint.h>

struct Rights {
    enum Bit : uint32_t {
        Read      = 1 << 0,
        Write     = 1 << 1,
        Duplicate = 1 << 2,
        Transfer  = 1 << 3,
    };
    uint32_t mask = 0;

    bool has(Rights needed) const {
        return (mask & needed.mask) == needed.mask;
    }
};
```

- [ ] **Step 3: Write object.cpp (Release implementation)**

```cpp
#include "kernel/core/object/object.hpp"
#include "kernel/core/mm/slab.hpp"

void KernelObject::Release() {
    if (--ref_count_ == 0) {
        kfree(this);
    }
}
```

- [ ] **Step 4: Write object_test.cpp — test lifecycle**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/object/object.hpp"

// Concrete subclass for testing
class TestObj : public KernelObject {
public:
    TestObj() : KernelObject(Type::Channel) {} // Channel type for test
};

// Stub kmalloc/kfree for host tests
namespace {
    uint8_t g_mem[4096];
    size_t g_used = 0;
}

void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}

TEST(ObjectTest, AddRefIncrements) {
    g_used = 0;
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    EXPECT_EQ(obj->refcount(), 1u);
    obj->AddRef();
    EXPECT_EQ(obj->refcount(), 2u);
    obj->Release();
    EXPECT_EQ(obj->refcount(), 1u);
    obj->Release(); // destroys
}

TEST(ObjectTest, TypeIsCorrect) {
    g_used = 0;
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    EXPECT_EQ(obj->type(), KernelObject::Type::Channel);
    obj->Release();
}

TEST(RightsTest, HasAllBits) {
    Rights r{.mask = Rights::Read | Rights::Write};
    EXPECT_TRUE(r.has(Rights{.mask = Rights::Read}));
    EXPECT_TRUE(r.has(Rights{.mask = Rights::Read | Rights::Write}));
    EXPECT_FALSE(r.has(Rights{.mask = Rights::Duplicate}));
}

TEST(RightsTest, EmptyMask) {
    Rights r{};
    EXPECT_TRUE(r.has(Rights{}));
}
```

- [ ] **Step 5: Create test/object/BUILD.bazel for object_test**

```python
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "object_test",
    size = "small",
    srcs = ["object_test.cpp"],
    deps = [
        "//kernel/core/object:object_hdrs",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

- [ ] **Step 6: Create kernel/core/object/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "object",
    srcs = ["object.cpp"],
    hdrs = ["object.hpp", "rights.hpp"],
    deps = ["//kernel/core:mm:mm"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "object_hdrs",
    hdrs = ["object.hpp", "rights.hpp"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 7: Build and run test**

```bash
bazel test //test/object:object_test
```

- [ ] **Step 8: Commit**

```bash
git add kernel/core/object/ test/object/
git commit -m "feat(object): add KernelObject base class and Rights"
```

---

### Task 3: Handle Table

**Files:**
- Create: `kernel/core/object/handle_table.hpp`
- Create: `kernel/core/object/handle_table.cpp`
- Modify: `test/object/object_test.cpp` — add handle table tests
- Modify: `kernel/core/object/BUILD.bazel`

- [ ] **Step 1: Write handle_table.hpp**

```cpp
#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"

using handle_t = uint32_t;
constexpr handle_t INVALID_HANDLE = 0;
constexpr int MAX_HANDLES = 1024;

struct HandleEntry {
    KernelObject* obj = nullptr;
    Rights rights{};
};

void    handle_table_init();
handle_t handle_alloc(KernelObject* obj, Rights rights);
void    handle_free(handle_t h);
KernelObject* handle_lookup(handle_t h, Rights needed = Rights{}, Rights* out_rights = nullptr);
```

- [ ] **Step 2: Write handle_table.cpp**

```cpp
#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

namespace {
    HandleEntry g_table[MAX_HANDLES];
    handle_t g_free_head = 1; // slot 0 = invalid
    SpinLock g_lock;

    void init_free_list() {
        for (handle_t i = 1; i < MAX_HANDLES - 1; i++) {
            g_table[i].obj = reinterpret_cast<KernelObject*>(static_cast<uintptr_t>(i + 1));
        }
    }
}

void handle_table_init() {
    init_free_list();
}

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    g_lock.lock();
    if (g_free_head == 0 || g_free_head >= MAX_HANDLES) {
        g_lock.unlock();
        return INVALID_HANDLE;
    }
    handle_t h = g_free_head;
    g_free_head = static_cast<handle_t>(
        reinterpret_cast<uintptr_t>(g_table[h].obj));
    g_table[h].obj = obj;
    g_table[h].rights = rights;
    obj->AddRef();
    g_lock.unlock();
    return h;
}

void handle_free(handle_t h) {
    if (h == 0 || h >= MAX_HANDLES) return;
    g_lock.lock();
    KernelObject* obj = g_table[h].obj;
    if (obj) {
        g_table[h].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(g_free_head));
        g_table[h].rights = Rights{};
        g_free_head = h;
        obj->Release();
    }
    g_lock.unlock();
}

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    if (h == 0 || h >= MAX_HANDLES) return nullptr;
    g_lock.lock();
    KernelObject* obj = g_table[h].obj;
    Rights rights = g_table[h].rights;
    g_lock.unlock();
    if (!obj) return nullptr;
    if (needed.mask != 0 && !rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = rights;
    return obj;
}
```

- [ ] **Step 3: Add handle table tests to object_test.cpp**

```cpp
TEST(HandleTableTest, AllocAndLookup) {
    g_used = 0;
    handle_table_init();
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    handle_t h = handle_alloc(obj, Rights{.mask = Rights::Read | Rights::Write});
    EXPECT_NE(h, INVALID_HANDLE);

    KernelObject* found = handle_lookup(h);
    EXPECT_EQ(found, obj);
    EXPECT_EQ(found->refcount(), 2u); // obj + handle

    handle_free(h);
    EXPECT_EQ(handle_lookup(h), nullptr);
}

TEST(HandleTableTest, RightsCheck) {
    g_used = 0;
    handle_table_init();
    TestObj* obj = new (kmalloc(sizeof(TestObj))) TestObj();
    handle_t h = handle_alloc(obj, Rights{.mask = Rights::Read});

    // Lookup with no rights check always succeeds
    EXPECT_NE(handle_lookup(h, Rights{}), nullptr);
    // Read passes
    EXPECT_NE(handle_lookup(h, Rights{.mask = Rights::Read}), nullptr);
    // Write fails
    EXPECT_EQ(handle_lookup(h, Rights{.mask = Rights::Write}), nullptr);

    handle_free(h);
}

TEST(HandleTableTest, InvalidHandle) {
    handle_table_init();
    EXPECT_EQ(handle_lookup(0), nullptr);
    EXPECT_EQ(handle_lookup(9999), nullptr);
}
```

- [ ] **Step 4: Run tests**

```bash
bazel test //test/object:object_test
```

- [ ] **Step 5: Commit**

```bash
git add kernel/core/object/ test/object/object_test.cpp
git commit -m "feat(object): add global handle table"
```

---

### Task 4: Channel

**Files:**
- Create: `kernel/core/object/channel.hpp`
- Create: `kernel/core/object/channel.cpp`
- Create: `test/object/channel_test.cpp`
- Modify: `kernel/core/object/BUILD.bazel`
- Modify: `test/object/BUILD.bazel`

- [ ] **Step 1: Write channel.hpp**

```cpp
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
```

- [ ] **Step 2: Write channel.cpp**

```cpp
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
    if (!msg) return -1; // ERR_NO_MEMORY

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
        // Transfer handles: remove from sender's table
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

    if (!msg) return -2; // ERR_EMPTY

    if (out_len) *out_len = msg->data_len;

    // Copy data
    if (buf && msg->data && msg->data_len > 0) {
        size_t copy = msg->data_len < buf_size ? msg->data_len : buf_size;
        for (size_t i = 0; i < copy; i++) {
            static_cast<uint8_t*>(buf)[i] = msg->data[i];
        }
    }

    // Transfer handles to receiver
    if (out_num_handles) *out_num_handles = msg->num_handles;
    if (handle_buf && msg->handles && msg->num_handles > 0) {
        size_t copy = msg->num_handles < buf_capacity
                          ? msg->num_handles : buf_capacity;
        for (size_t i = 0; i < copy; i++) {
            handle_buf[i] = msg->handles[i];
        }
    }

    // Clean up message
    if (msg->data) kfree(msg->data);
    if (msg->handles) kfree(msg->handles);
    kfree(msg);
    return 0;
}
```

- [ ] **Step 3: Write channel_test.cpp**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/object/channel.hpp"

// Stubs for host testing
namespace {
    uint8_t g_mem[8192];
    size_t g_used = 0;
}
void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}

// Null out handle table ops for channel test (we test pure channel logic)
void handle_table_init() {}
handle_t handle_alloc(KernelObject*, Rights) { return 1; }
void handle_free(handle_t) {}

TEST(ChannelTest, WriteAndReadData) {
    g_used = 0;
    Channel ch;
    const char* msg = "hello";
    EXPECT_EQ(ch.Write(msg, 6, nullptr, 0), 0);

    char buf[32] = {};
    size_t len = 0;
    EXPECT_EQ(ch.Read(buf, sizeof(buf), &len, nullptr, 0, nullptr), 0);
    EXPECT_EQ(len, 6u);
    EXPECT_STREQ(buf, "hello");
}

TEST(ChannelTest, EmptyRead) {
    g_used = 0;
    Channel ch;
    size_t len;
    EXPECT_EQ(ch.Read(nullptr, 0, &len, nullptr, 0, nullptr), -2); // ERR_EMPTY
}

TEST(ChannelTest, FifoOrder) {
    g_used = 0;
    Channel ch;
    ch.Write("a", 2, nullptr, 0);
    ch.Write("b", 2, nullptr, 0);
    ch.Write("c", 2, nullptr, 0);

    char buf[4];
    size_t len;
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "a");
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "b");
    ch.Read(buf, 4, &len, nullptr, 0, nullptr);
    EXPECT_STREQ(buf, "c");
}
```

- [ ] **Step 4: Add to BUILD.bazel files and run tests**

```bash
bazel test //test/object:channel_test
```

- [ ] **Step 5: Commit**

```bash
git add kernel/core/object/ test/object/
git commit -m "feat(object): add Channel (bidirectional IPC)"
```

---

### Task 5: Port + Name Registry

**Files:**
- Create: `kernel/core/object/port.hpp`
- Create: `kernel/core/object/port.cpp`
- Create: `test/object/port_test.cpp`
- Modify: `kernel/core/object/BUILD.bazel`
- Modify: `test/object/BUILD.bazel`

- [ ] **Step 1: Write port.hpp**

```cpp
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

    // Called by port_connect: creates channel pair, enqueues server end
    static int Connect(Port* port, handle_t* out_client_chan);

private:
    SpinLock lock_;
    struct Conn {
        handle_t channel;  // server end of the new channel
        Conn*    next;
    };
    Conn* head_ = nullptr;
    Conn* tail_ = nullptr;
};

// Name Registry (global)
void    port_register_name(const char* name, Port* port);
Port*   port_lookup_name(const char* name);
```

- [ ] **Step 2: Write port.cpp**

```cpp
#include "kernel/core/object/port.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/mm/slab.hpp"

int Port::Accept(handle_t* out_channel) {
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

int Port::Connect(Port* port, handle_t* out_client_chan) {
    // Create a channel pair
    Channel* ch = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!ch) return -1;
    new (ch) Channel();

    handle_t a = handle_alloc(ch, Rights{.mask = Rights::Read | Rights::Write |
                                                 Rights::Duplicate | Rights::Transfer});
    handle_t b = handle_alloc(ch, Rights{.mask = Rights::Read | Rights::Write |
                                                 Rights::Duplicate | Rights::Transfer});
    ch->Release(); // handles own the ref

    // Enqueue server end (b) in the port
    Conn* conn = static_cast<Conn*>(kmalloc(sizeof(Conn)));
    if (!conn) {
        handle_free(a);
        handle_free(b);
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

void port_register_name(const char* name, Port* port) {
    g_name_lock.lock();
    if (g_name_count < MAX_NAMES) {
        g_names[g_name_count].name = name;
        g_names[g_name_count].port = port;
        g_name_count++;
    }
    g_name_lock.unlock();
}

Port* port_lookup_name(const char* name) {
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
```

- [ ] **Step 3: Write port_test.cpp**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/object/port.hpp"
#include "kernel/core/object/channel.hpp"

// Stubs (same as channel_test)
namespace { uint8_t g_mem[16384]; size_t g_used = 0; }
void* kmalloc(size_t n) {
    if (g_used + n > sizeof(g_mem)) return nullptr;
    void* p = &g_mem[g_used];
    g_used += n;
    return p;
}
void kfree(void*) {}
void handle_table_init() {}
handle_t handle_alloc(KernelObject* obj, Rights r) {
    static handle_t next = 1;
    (void)obj; (void)r;
    return next++;
}
void handle_free(handle_t) {}
KernelObject* handle_lookup(handle_t, Rights) { return nullptr; }

TEST(PortTest, CreateAndAccept) {
    g_used = 0;
    Port port;

    handle_t client_chan;
    EXPECT_EQ(Port::Connect(&port, &client_chan), 0);
    EXPECT_NE(client_chan, INVALID_HANDLE);

    handle_t server_chan;
    EXPECT_EQ(port.Accept(&server_chan), 0);
    EXPECT_NE(server_chan, INVALID_HANDLE);
}

TEST(PortTest, AcceptEmpty) {
    g_used = 0;
    Port port;
    handle_t chan;
    EXPECT_EQ(port.Accept(&chan), -2); // ERR_EMPTY
}

TEST(PortTest, NameRegistry) {
    g_used = 0;
    Port port;
    port_register_name("test/echo", &port);
    EXPECT_EQ(port_lookup_name("test/echo"), &port);
    EXPECT_EQ(port_lookup_name("nonexistent"), nullptr);
}

TEST(PortTest, MultipleConnections) {
    g_used = 0;
    Port port;
    handle_t c1, c2, s1, s2;
    EXPECT_EQ(Port::Connect(&port, &c1), 0);
    EXPECT_EQ(Port::Connect(&port, &c2), 0);
    EXPECT_EQ(port.Accept(&s1), 0);
    EXPECT_EQ(port.Accept(&s2), 0);
    EXPECT_NE(s1, s2); // different channels
}
```

- [ ] **Step 4: Run tests**

```bash
bazel test //test/object:port_test
```

- [ ] **Step 5: Commit**

```bash
git add kernel/core/object/ test/object/
git commit -m "feat(object): add Port and name registry"
```

---

### Task 6: Syscall Dispatch

**Files:**
- Modify: `kernel/arch/x86_64/syscall.cpp`
- Modify: `kernel/arch/x86_64/syscall.hpp`
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add object deps

- [ ] **Step 1: Add syscall numbers to syscall.hpp**

In `syscall.hpp`, add:

```cpp
// Syscall numbers
constexpr uint64_t SYSCALL_DEBUG_PRINT    = 0;
constexpr uint64_t SYSCALL_HANDLE_CLOSE   = 1;
constexpr uint64_t SYSCALL_HANDLE_DUP     = 2;
constexpr uint64_t SYSCALL_CHANNEL_CREATE = 10;
constexpr uint64_t SYSCALL_CHANNEL_WRITE  = 11;
constexpr uint64_t SYSCALL_CHANNEL_READ   = 12;
constexpr uint64_t SYSCALL_PORT_CREATE    = 20;
constexpr uint64_t SYSCALL_PORT_REGISTER  = 21;
constexpr uint64_t SYSCALL_PORT_CONNECT   = 22;
constexpr uint64_t SYSCALL_PORT_ACCEPT    = 23;
```

- [ ] **Step 2: Add dispatch table to syscall.cpp**

Add after existing includes:

```cpp
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/port.hpp"
#include "kernel/lib/klog.hpp"
```

Replace the existing `syscall_dispatcher` and `syscall_init`:

```cpp
namespace {

// ── Syscall dispatch table ──────────────────────────────────────

uint64_t sys_debug_print(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    klog(reinterpret_cast<const char*>(a1));
    return 0;
}

uint64_t sys_handle_close(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    handle_free(static_cast<handle_t>(a1));
    return 0;
}

uint64_t sys_handle_dup(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    handle_t h = static_cast<handle_t>(a1);
    Rights needed{.mask = Rights::Duplicate};
    Rights existing;
    KernelObject* obj = handle_lookup(h, needed, &existing);
    if (!obj) return INVALID_HANDLE;

    Rights new_rights{.mask = static_cast<uint32_t>(a2)};
    // Downgrade: new rights cannot exceed existing
    new_rights.mask &= existing.mask;

    return handle_alloc(obj, new_rights);
}

uint64_t sys_channel_create(uint64_t, uint64_t, uint64_t, uint64_t) {
    Channel* ch = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!ch) return INVALID_HANDLE;
    new (ch) Channel();

    Rights full{.mask = Rights::Read | Rights::Write |
                       Rights::Duplicate | Rights::Transfer};
    handle_t a = handle_alloc(ch, full);
    handle_t b = handle_alloc(ch, full);
    ch->Release(); // handles own refs

    // Return pair: (a << 32) | b
    return (static_cast<uint64_t>(a) << 32) | b;
}

uint64_t sys_channel_write(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = handle_lookup(h, Rights{.mask = Rights::Write});
    if (!obj || obj->type() != KernelObject::Type::Channel) return -1;

    // Args packed into struct pointed to by a2 (4-register syscall ABI
    // can't pass 5 scalars; struct packs data + handles args together)
    struct ChannelWriteArgs {
        const void* data; size_t data_len;
        const handle_t* handles; size_t num_handles;
    };
    auto* args = reinterpret_cast<const ChannelWriteArgs*>(a2);

    Channel* ch = static_cast<Channel*>(obj);
    return ch->Write(args->data, args->data_len,
                     args->handles, args->num_handles);
}

uint64_t sys_channel_read(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t, uint64_t) {
    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = handle_lookup(h, Rights{.mask = Rights::Read});
    if (!obj || obj->type() != KernelObject::Type::Channel) return -1;

    Channel* ch = static_cast<Channel*>(obj);
    size_t out_len;
    int rc = ch->Read(reinterpret_cast<void*>(a2), a3, &out_len,
                      nullptr, 0, nullptr);
    return (rc == 0) ? out_len : rc;
}

uint64_t sys_port_create(uint64_t, uint64_t, uint64_t, uint64_t) {
    Port* port = static_cast<Port*>(kmalloc(sizeof(Port)));
    if (!port) return INVALID_HANDLE;
    new (port) Port();

    Rights r{.mask = Rights::Read | Rights::Write |
                     Rights::Duplicate | Rights::Transfer};
    handle_t h = handle_alloc(port, r);
    port->Release();
    return h;
}

uint64_t sys_port_register(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = handle_lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Port) return -1;

    port_register_name(reinterpret_cast<const char*>(a2),
                       static_cast<Port*>(obj));
    return 0;
}

uint64_t sys_port_connect(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    const char* name = reinterpret_cast<const char*>(a1);
    handle_t client_chan = static_cast<handle_t>(a2);
    Port* port = port_lookup_name(name);
    if (!port) return -1; // ERR_NOT_FOUND

    handle_t new_chan;
    int rc = Port::Connect(port, &new_chan);
    if (rc != 0) return rc;
    return new_chan;
}

uint64_t sys_port_accept(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = handle_lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Port) return INVALID_HANDLE;

    handle_t out_chan;
    int rc = static_cast<Port*>(obj)->Accept(&out_chan);
    return (rc == 0) ? out_chan : rc;
}

using syscall_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

constexpr int MAX_SYSCALL = 24;
syscall_fn_t g_syscall_table[MAX_SYSCALL];

void init_syscall_table() {
    g_syscall_table[SYSCALL_DEBUG_PRINT]    = sys_debug_print;
    g_syscall_table[SYSCALL_HANDLE_CLOSE]   = sys_handle_close;
    g_syscall_table[SYSCALL_HANDLE_DUP]     = sys_handle_dup;
    g_syscall_table[SYSCALL_CHANNEL_CREATE] = sys_channel_create;
    g_syscall_table[SYSCALL_CHANNEL_WRITE]  = sys_channel_write;
    g_syscall_table[SYSCALL_CHANNEL_READ]   = sys_channel_read;
    g_syscall_table[SYSCALL_PORT_CREATE]    = sys_port_create;
    g_syscall_table[SYSCALL_PORT_REGISTER]  = sys_port_register;
    g_syscall_table[SYSCALL_PORT_CONNECT]   = sys_port_connect;
    g_syscall_table[SYSCALL_PORT_ACCEPT]    = sys_port_accept;
}

extern "C" uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4) {
    if (num >= MAX_SYSCALL || !g_syscall_table[num]) {
        klog("Syscall #"); klog_hex(num); klog(": invalid\n");
        return 0xFFFFFFFFFFFFFFFFULL; // ERR_INVALID
    }
    return g_syscall_table[num](a1, a2, a3, a4);
}

} // namespace
```

- [ ] **Step 3: Update syscall_init to call init_syscall_table + handle_table_init**

```cpp
void syscall_init() {
    handle_table_init();
    init_syscall_table();

    // ... existing MSR setup code stays the same
}
```

- [ ] **Step 4: Update kernel/arch/x86_64/BUILD.bazel** — add `//kernel/core/object:object` to deps of `arch`

- [ ] **Step 5: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/syscall.cpp kernel/arch/x86_64/syscall.hpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat(syscall): add Phase 6 dispatch table and syscall handlers"
```

---

### Task 7: Boot Demo

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp`

- [ ] **Step 1: Add IPC demo threads to boot.cpp**

After the Phase 5 scheduler demo section, add:

```cpp
// ── Phase 6: Object Manager + IPC demo ──
klog("=== Phase 6: Object Manager + IPC ===\n\n");

Thread* ipc_server = thread_create([](){
    // Create port and register it
    Port* port = static_cast<Port*>(kmalloc(sizeof(Port)));
    new (port) Port();
    handle_t port_h = handle_alloc(port,
        Rights{.mask = Rights::Read | Rights::Write | Rights::Duplicate | Rights::Transfer});
    port->Release();

    port_register_name("demo", port);

    klog("[ipc-server] Port 'demo' registered, waiting for connections...\n");

    while (1) {
        handle_t client_chan;
        if (port->Accept(&client_chan) == 0) {
            klog("[ipc-server] Client connected (chan ");
            klog_hex(client_chan);
            klog(")\n");

            char buf[128];
            size_t len;
            int rc;
            // Poll-read (non-blocking, Phase 6 behavior)
            while ((rc = static_cast<Channel*>(
                handle_lookup(client_chan, Rights{.mask = Rights::Read}))
                ->Read(buf, sizeof(buf), &len, nullptr, 0, nullptr)) == -2) {
                thread_yield();
            }

            klog("[ipc-server] Got message: '");
            klog(buf);
            klog("' (");
            klog_hex(len);
            klog(" bytes)\n");

            // Send ACK
            const char* ack = "ACK from server";
            static_cast<Channel*>(
                handle_lookup(client_chan, Rights{.mask = Rights::Write}))
                ->Write(ack, 15, nullptr, 0);

            handle_free(client_chan);
        }
    }
}, "ipc-server", 1);

Thread* ipc_client = thread_create([](){
    // Wait for server to register port
    klog("[ipc-client] Looking for 'demo' port...\n");
    Port* port = nullptr;
    while (!(port = port_lookup_name("demo"))) {
        thread_yield();
    }

    // Connect to port
    handle_t my_chan;
    Port::Connect(port, &my_chan);
    klog("[ipc-client] Connected (chan ");
    klog_hex(my_chan);
    klog(")\n");

    // Write message
    const char* msg = "hello kernel IPC!";
    static_cast<Channel*>(
        handle_lookup(my_chan, Rights{.mask = Rights::Write}))
        ->Write(msg, 18, nullptr, 0);

    // Read reply
    char reply[64];
    size_t len;
    int rc;
    while ((rc = static_cast<Channel*>(
        handle_lookup(my_chan, Rights{.mask = Rights::Read}))
        ->Read(reply, sizeof(reply), &len, nullptr, 0, nullptr)) == -2) {
        thread_yield();
    }

    klog("[ipc-client] Reply: '");
    klog(reply);
    klog("'\n");
}, "ipc-client", 2);

if (ipc_server) thread_start(ipc_server);
if (ipc_client) thread_start(ipc_client);
```

- [ ] **Step 2: Add necessary includes to boot.cpp**

```cpp
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/port.hpp"
```

- [ ] **Step 3: Build and boot**

```bash
bazel build //kernel:kernel && bash scripts/run.sh
```

Expected output:
```
[ipc-server] Port 'demo' registered, waiting for connections...
[ipc-client] Looking for 'demo' port...
[ipc-client] Connected (chan 0x...)
[ipc-server] Client connected (chan 0x...)
[ipc-server] Got message: 'hello kernel IPC!'
[ipc-client] Reply: 'ACK from server'
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp
git commit -m "feat: wire Phase 6 IPC demo into boot sequence"
```

---

### Task 8: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md**

Add Phase 6 to the phase table:

```markdown
| 6: Object Manager + IPC | `docs/superpowers/plans/2026-05-05-phase-6-object-ipc.md` | Done |
```

Update architecture tree to add `core/object/`:

```
├── core/
│   ├── mm/             # pmm, bitmap_alloc, buddy, slab, new_delete
│   ├── sched/          # scheduler — thread, run queue, context switch
│   └── object/         # KernelObject, handle table, rights, channel, port
```

Add test command:

```bash
bazel test //test/object:all
```

Update known issues: add note that handle table is global (moves to per-process in Phase 7).

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 6 completion"
```

---

## Test Verification

After all tasks complete, run the full test suite:

```bash
bazel test //test/mm:all //test/irq:all //test/sched:all //test/object:all
bazel build //kernel:kernel
bash scripts/run.sh
```
