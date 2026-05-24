# C++ Modernization Design

## Overview

A focused modernization of the entire codebase (kernel + ring-3 user-space) to leverage C++26 idioms for safety, conciseness, and maintainability. Targets ~70 files, ~4000 lines changed.

**Key decisions:**

| Decision | Detail |
|----------|--------|
| Compiler | Homebrew Clang 22.1.4 (`/usr/local/Cellar/llvm/22.1.4/bin/clang++`) |
| Target | `x86_64-unknown-elf`, `-ffreestanding -nostdlib` |
| C++ standard | C++26, no exceptions, no RTTI |
| stdlib | libc++ freestanding headers via custom `__config_site` + C runtime stubs — see below |
| Ring-3 sharing | ring-3 programs (`init/`, `shell/`, `devfs/`, `tmpfs/`, `fat32/`) share `kernel/lib/` and libc++ headers |
| Scope | All code: kernel core + arch + ring-3 user-space |

## Compiler Switch: Apple Clang 17 → Homebrew Clang 22

### Rationale

- Apple Clang 17 is the system compiler but too old for libc++ 22 freestanding (libc++ 22 requires Clang 20+)
- Homebrew Clang 22.1.4 is already installed at `/usr/local/Cellar/llvm/22.1.4/`
- Enables libc++ freestanding headers for `x86_64-unknown-elf`

### Toolchain Changes

Update `toolchain/BUILD.bazel` — cross-compilation toolchain paths:

| Tool | Before (Apple Clang) | After (Homebrew Clang) |
|------|---------------------|----------------------|
| `gcc`/`g++` | `/usr/bin/clang` | `/usr/local/Cellar/llvm/22.1.4/bin/clang` |
| `cpp` | `/usr/local/opt/llvm/bin/clang-cpp` | `/usr/local/Cellar/llvm/22.1.4/bin/clang-cpp` |
| C++ include dirs | `/Library/Developer/CommandLineTools/usr/lib/clang/17/include` | Both Apple include dir AND libc++ freestanding |

Host toolchain (for unit tests) stays on Apple Clang — host tests compile for macOS, not bare-metal.

## libc++ Freestanding Configuration

### What's Available

With custom `__config_site` disabling threads and vendor availability, these libc++ headers compile cleanly for `x86_64-unknown-elf`:

| Header | Provides |
|--------|----------|
| `<type_traits>` | `std::is_same_v`, `std::is_pointer_v`, `std::is_arithmetic_v`, `std::remove_reference`, etc. |
| `<concepts>` | `std::same_as`, `std::derived_from`, `std::integral`, custom concepts |
| `<utility>` | `std::move`, `std::forward`, `std::declval`, `std::index_sequence` |
| `<new>` | placement new, `std::align_val_t` |
| `<cstddef>` | `std::size_t`, `std::ptrdiff_t`, `std::byte` |
| `<span>` | `std::span<T>` (C++20) |
| `<limits>` | `std::numeric_limits<T>` |
| `<bit>` | `std::bit_cast`, `std::countl_zero`, `std::has_single_bit` |

### What's NOT Available (needs hosted runtime)

`<optional>`, `<expected>`, `<variant>`, `<vector>`, `<string>`, `<string_view>`, `<array>`, `<functional>` — all require either exception support, heap allocation, or C library functions beyond what we provide.

### Custom `__config_site`

libc++'s default `__config_site` enables threads and vendor availability markup, incompatible with bare-metal. We provide a custom one:

```cpp
// kernel/lib/libcpp/__config_site
#define _LIBCPP_HAS_NO_THREADS 1
#define _LIBCPP_DISABLE_AVAILABILITY 1
#define _LIBCPP_HAS_NO_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_NO_FILESYSTEM 1
#define _LIBCPP_HAS_NO_RANDOM_DEVICE 1
#define _LIBCPP_HAS_NO_LOCALIZATION 1
#define _LIBCPP_HAS_NO_UNICODE 1
#define _LIBCPP_HAS_NO_WIDE_CHARACTERS 1
// ... (full config in kernel/lib/libcpp/__config_site)
```

This directory is placed first in the include path (`-isystem`) so it overrides libc++'s built-in `__config_site`.

### C Runtime Stubs

libc++ headers like `<utility>` → `<cstring>` pull in `memcpy`/`memset`/`memmove`/`memcmp` declarations. We provide implementations:

```cpp
// kernel/lib/crt_stubs.cpp
extern "C" {
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int   memcmp(const void* a, const void* b, size_t n);
}
```

These link into both kernel and ring-3 programs. They are standard C runtime functions any kernel needs anyway.

## Code Style Rules

### 1. Trailing Return Types

All function declarations use trailing return type, unless ABI-incompatible:

```cpp
// ✅ Correct
auto pml4_index(uint64_t va) noexcept -> uint16_t;
auto buddy_alloc_pages(size_t order) -> void*;

// ❌ Incorrect
uint16_t pml4_index(uint64_t va);

// Exception: extern "C" / assembly symbols
extern "C" void switch_to(Thread* prev, Thread* next);
extern "C" void syscall_entry();
```

### 2. No `Type var(init)` — "Most Vexing Parse" Elimination

```cpp
// ✅ Correct
auto msg = FileMsg{};
auto lock = ScopedLock{my_lock};
auto arr = std::array<char, 32>{};

// ❌ Incorrect
FileMsg msg();
```

### 3. `using` Only, No `typedef`

### 4. Class/Struct Members Use Trailing Return

```cpp
class Channel : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Channel;
    auto Write(const void* data, size_t len,
               const handle_t* handles, size_t num_handles,
               bool from_endpoint_b = false) -> int;
};
```

### 5. `auto` for Redundant Type Names

Use `auto` when the type appears explicitly in the same expression:

```cpp
auto* ch = static_cast<Channel*>(obj);
auto result = typed_lookup<Channel>(handles, h, rights);
```

Don't use `auto` when the type is non-obvious:

```cpp
uint64_t pml4_phys = paging_kernel_pml4_template();
```

## Core Utility Layer (`kernel/lib/`)

### Result<T>

```cpp
// kernel/lib/result.hpp
namespace km {

template <typename T>
class Result {
public:
    static auto Ok(T val) -> Result { return Result(std::move(val), true); }
    static auto Err(int error) -> Result { return Result(error); }

    explicit operator bool() const { return ok_; }
    auto value() -> T& { return value_; }
    auto take_value() -> T&& { return std::move(value_); }
    auto error() const -> int { return error_; }

    Result(Result&&) noexcept;
    auto operator=(Result&&) noexcept -> Result&;
    Result(const Result&) = delete;
    auto operator=(const Result&) = delete;
    ~Result();

private:
    Result(T val, bool ok) : value_(std::move(val)), ok_(ok) {}
    explicit Result(int err) : error_(err), ok_(false) {}

    union { T value_; int error_; };
    bool ok_;
};

} // namespace km
```

Note: uses `std::move` from `<utility>`, NOT hand-rolled `km::move`.

### RAII Guards

```cpp
// kernel/lib/scoped_lock.hpp
template <typename Lock>
class ScopedLock {
    Lock& lock_;
public:
    explicit ScopedLock(Lock& l) noexcept : lock_(l) { lock_.lock(); }
    ~ScopedLock() { lock_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    auto operator=(const ScopedLock&) = delete;
};
```

```cpp
// kernel/lib/scoped_mem.hpp
class ScopedMem {
    void* ptr_ = nullptr;
public:
    explicit ScopedMem(void* p) noexcept : ptr_(p) {}
    ~ScopedMem() { if (ptr_) kfree(ptr_); }
    ScopedMem(ScopedMem&& other) noexcept;
    auto operator=(ScopedMem&& other) noexcept -> ScopedMem&;
    ScopedMem(const ScopedMem&) = delete;
    auto operator=(const ScopedMem&) = delete;
    auto release() noexcept -> void*;
    auto get() const noexcept -> void* { return ptr_; }
};
```

```cpp
// kernel/core/object/handle_table.hpp
class ScopedHandle {
    HandleTable* table_ = nullptr;
    handle_t handle_ = INVALID_HANDLE;
public:
    ScopedHandle(HandleTable& t, handle_t h) noexcept : table_(&t), handle_(h) {}
    ~ScopedHandle();
    ScopedHandle(ScopedHandle&& other) noexcept;
    auto operator=(ScopedHandle&& other) noexcept -> ScopedHandle&;
    ScopedHandle(const ScopedHandle&) = delete;
    auto operator=(const ScopedHandle&) = delete;
    auto get() const noexcept -> handle_t { return handle_; }
    auto release() noexcept -> handle_t;
};
```

### copy_bytes / zero_bytes

```cpp
// kernel/lib/bitwise.hpp
template <typename T>
auto copy_bytes(T* dst, const T* src, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = src[i];
}

template <typename T>
auto zero_bytes(T* dst, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = T{};
}
```

## Object Manager Layer

### typed_lookup<T>

Eliminates repeated Lookup → type-check → cast pattern in every syscall handler:

```cpp
// kernel/core/object/handle_table.hpp

template <typename T>
auto typed_lookup(HandleTable& table, handle_t h, Rights needed = {})
    -> km::Result<T*> {
    auto* obj = table.Lookup(h, needed);
    if (!obj || obj->type() != T::kType) return km::Result<T*>::Err(-1);
    return km::Result<T*>::Ok(static_cast<T*>(obj));
}
```

Each `KernelObject` subclass adds:

```cpp
class Channel : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Channel;
};
```

### Message Move Semantics

```cpp
struct Message {
    km::ScopedMem data_mem;
    km::ScopedMem handle_mem;
    size_t    data_len = 0;
    handle_t* handles = nullptr;
    size_t    num_handles = 0;
    Message*  next = nullptr;

    Message(Message&&) = default;
    auto operator=(Message&&) -> Message& = default;
    Message(const Message&) = delete;
    auto operator=(const Message&) = delete;
};
```

`Channel::Write` drops from ~30 lines of manual malloc/copy/cleanup to ~10 lines.

## noexcept Annotation

All pure-computation, no-failure functions annotated `noexcept`:

| Category | Examples |
|----------|----------|
| Page table helpers | `pml4_index()`, `pdpt_index()`, `make_pte()`, `pte_phys_addr()` |
| Phys/virt translation | `phys_to_virt()`, `virt_to_phys()` |
| Object accessors | `type()`, `refcount()`, `size()`, `num_pages()` |
| Span/Result accessors | `value()`, `error()`, `get()`, `release()` |

## Ring-3 User-Space

### Shared Type Definitions

Create `kernel/lib/user_types.hpp` to eliminate per-file `using int32_t = int; ...` boilerplate:

```cpp
// kernel/lib/user_types.hpp
#pragma once

using int32_t = int;
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using uint64_t = unsigned long long;
using size_t = decltype(sizeof(0));
using nullptr_t = decltype(nullptr);
```

All ring-3 programs include this instead of re-declaring. Ring-3 code also gets libc++ headers via the same include path.

### Syscall Dispatch

Replace C function-pointer table in `syscall.cpp` with a typed `SyscallArgs` struct:

```cpp
struct SyscallArgs { uint64_t a1, a2, a3, a4; };
```

Handlers receive `SyscallArgs` directly, with per-handler argument unpacking once at the top.

## File Change Summary

### New Files

| File | Purpose |
|------|---------|
| `kernel/lib/libcpp/__config_site` | libc++ freestanding configuration |
| `kernel/lib/crt_stubs.cpp` | `memcpy`/`memset`/`memmove`/`memcmp` implementations |
| `kernel/lib/crt_stubs.hpp` | C runtime function declarations |
| `kernel/lib/result.hpp` | `km::Result<T>` error propagation |
| `kernel/lib/scoped_lock.hpp` | `km::ScopedLock<Lock>` |
| `kernel/lib/scoped_mem.hpp` | `km::ScopedMem` RAII kfree |
| `kernel/lib/bitwise.hpp` | `km::copy_bytes`, `km::zero_bytes` |
| `kernel/lib/user_types.hpp` | Shared ring-3 type aliases |

### Modified Files

| Layer | Files | Main Changes |
|-------|-------|--------------|
| Toolchain | `toolchain/BUILD.bazel` | Compiler paths to Homebrew Clang 22, add libc++ include paths |
| Code style | ~60 `.hpp`/`.cpp` across `kernel/` | Trailing return types, `auto`, `noexcept`, `{}` init |
| Object manager | `handle_table.hpp`, `object.hpp`, `channel.hpp`, `channel.cpp`, `process.hpp`, `process.cpp`, `vmo.hpp` | `typed_lookup<T>`, `kType` constants, Message move, ScopedHandle |
| Syscall dispatch | `syscall.hpp`, `syscall.cpp` | `SyscallArgs`, handler signatures |
| Ring-3 programs | `init.cpp`, `shell.cpp`, `devfs.cpp`, `tmpfs.cpp`, `fat32.cpp` | `#include "kernel/lib/user_types.hpp"`, style rules, libc++ headers |
| Memory layer | `slab.hpp`, `slab.cpp`, `vmm.cpp`, `paging.hpp` | `noexcept`, `copy_bytes` replace manual loops |
| Build | `kernel/BUILD.bazel`, ring-3 BUILD files | Add `crt_stubs.cpp`, new lib targets to deps |

## Implementation Order

1. **Toolchain switch** — Homebrew Clang 22 paths, `__config_site`, include dirs
2. **C runtime stubs** — `crt_stubs.cpp` + header
3. **New utilities** — `result.hpp`, `scoped_lock.hpp`, `scoped_mem.hpp`, `bitwise.hpp`, `user_types.hpp`
4. **Code style pass** — trailing return types, `auto`, `noexcept`, `{}` init across all files
5. **Object manager** — `kType` constants, `typed_lookup<T>`, Message move semantics, ScopedHandle
6. **Syscall dispatch** — `SyscallArgs` struct, handler refactor
7. **Ring-3 programs** — shared types, style alignment, libc++ includes
8. **Build & verify** — `bazel build //kernel:kernel && bash scripts/run.sh`

## Estimated Impact

| Metric | Estimate |
|--------|----------|
| New files | 8 |
| Modified files | ~70 |
| Lines added | ~800 |
| Lines modified | ~3500 |
| Lines deleted | ~600 |
| Net change | ~4300 lines |
