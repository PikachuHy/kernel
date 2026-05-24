# C++ Modernization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Modernize the entire C++26 kernel + ring-3 codebase with trailing return types, `noexcept`, `auto` deduction, RAII guards, `Result<T>`, `typed_lookup<T>`, and libc++ freestanding headers.

**Architecture:** Switch compiler to Homebrew Clang 22.1.4, configure libc++ freestanding via custom `__config_site`, provide C runtime stubs (`memcpy`/`memset`/`memmove`/`memcmp`), add `km::Result<T>` + RAII scoped guards to `kernel/lib/`, then mechanically apply code style rules across all ~70 files.

**Tech Stack:** C++26, Homebrew Clang 22.1.4, libc++ freestanding headers, Bazel 9, x86_64-unknown-elf target

---

## Code Style Transformation Reference

All style tasks use these patterns. Apply per file as listed.

### Pattern A: Trailing Return Types

```
BEFORE:  uint16_t pml4_index(uint64_t va) { return (va >> 39) & 0x1FF; }
AFTER:   auto pml4_index(uint64_t va) noexcept -> uint16_t { return (va >> 39) & 0x1FF; }

BEFORE:  void buddy_init(uint64_t hhdm_offset, uint64_t page_array_phys);
AFTER:   auto buddy_init(uint64_t hhdm_offset, uint64_t page_array_phys) -> void;

BEFORE:  int Write(...);
AFTER:   auto Write(...) -> int;
```

**Exceptions (keep as-is):** `extern "C"` functions, assembly labels, `main()`-like entry points

### Pattern B: noexcept on pure functions

Add `noexcept` to: inline helpers, const accessors, phys/virt translation, math functions. Do NOT add to: functions that allocate memory, take locks, call into unknown code, or I/O.

### Pattern C: `auto` for redundant casts

```
BEFORE:  Channel* ch = static_cast<Channel*>(obj);
AFTER:   auto* ch = static_cast<Channel*>(obj);

BEFORE:  HandleEntry* entries = static_cast<HandleEntry*>(phys_to_virt(phys));
AFTER:   auto* entries = static_cast<HandleEntry*>(phys_to_virt(phys));
```

### Pattern D: `{}` initialization

```
BEFORE:  FileMsg msg;
AFTER:   auto msg = FileMsg{};

BEFORE:  Rights r{.mask = Rights::Read};
AFTER:   auto r = Rights{.mask = Rights::Read};
```

---

### Task 1: Toolchain Switch + libc++ Freestanding Config

**Files:**
- Create: `kernel/lib/libcpp/__config_site`
- Modify: `toolchain/BUILD.bazel:48-61` (macOS cross-compilation)
- Modify: `toolchain/BUILD.bazel:113-126` (Linux cross-compilation)

- [ ] **Step 1: Create the custom `__config_site` header**

```bash
mkdir -p kernel/lib/libcpp
```

```cpp
// kernel/lib/libcpp/__config_site
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE

#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __1
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0

// Disable threads — bare-metal freestanding has no thread runtime
#define _LIBCPP_HAS_NO_THREADS 1

#define _LIBCPP_HAS_NO_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_NO_TERMINAL 1
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0

// Disable vendor availability markup — not applicable to bare-metal
#define _LIBCPP_DISABLE_AVAILABILITY 1

#define _LIBCPP_HAS_NO_FILESYSTEM 1
#define _LIBCPP_HAS_NO_RANDOM_DEVICE 1
#define _LIBCPP_HAS_NO_LOCALIZATION 1
#define _LIBCPP_HAS_NO_UNICODE 1
#define _LIBCPP_HAS_NO_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_NO_TIME_ZONE_DATABASE 1
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0

#define _LIBCPP_HARDENING_MODE_DEFAULT 2
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT 2

#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0

#endif
```

- [ ] **Step 2: Update macOS cross-compilation toolchain in `toolchain/BUILD.bazel`**

Replace lines 48-61 (the `x86_64_elf_config` block's tool_paths and include dirs):

Old:
```python
    tool_paths = {
        "gcc": "/usr/bin/clang",
        "g++": "/usr/bin/clang++",
        "cpp": "/usr/local/opt/llvm/bin/clang-cpp",
        ...
    },
    cxx_builtin_include_directories = [
        "/Library/Developer/CommandLineTools/usr/lib/clang/17/include",
    ],
```

New:
```python
    tool_paths = {
        "gcc": "/usr/local/Cellar/llvm/22.1.4/bin/clang",
        "g++": "/usr/local/Cellar/llvm/22.1.4/bin/clang++",
        "cpp": "/usr/local/Cellar/llvm/22.1.4/bin/clang-cpp",
        "ar": "/usr/local/Cellar/llvm/22.1.4/bin/llvm-ar",
        "nm": "/usr/local/Cellar/llvm/22.1.4/bin/llvm-nm",
        "ld": "/usr/local/bin/ld.lld",
        "objcopy": "/usr/local/Cellar/llvm/22.1.4/bin/llvm-objcopy",
        "objdump": "/usr/local/Cellar/llvm/22.1.4/bin/llvm-objdump",
        "strip": "/usr/local/Cellar/llvm/22.1.4/bin/llvm-strip",
    },
    cxx_builtin_include_directories = [
        "/usr/local/Cellar/llvm/22.1.4/lib/clang/22/include",
    ],
```

Also add to compile_flags (append to `_CROSS_COMPILE_FLAGS`):
```python
_CROSS_COMPILE_FLAGS = [
    "-target", "x86_64-unknown-elf",
    "-ffreestanding", "-nostdlib",
    "-std=c++26",
    "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wno-c99-designator",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mgeneral-regs-only", "-mcmodel=kernel",
]
```

- [ ] **Step 3: Update Linux cross-compilation toolchain in `toolchain/BUILD.bazel`**

Replace lines 113-126 (the `x86_64_elf_linux_config` tool_paths and include dirs):

Old:
```python
    tool_paths = {
        "gcc": "/usr/bin/clang",
        "g++": "/usr/bin/clang++",
        "cpp": "/usr/bin/clang++",
        "ar": "/usr/lib/llvm-21/bin/llvm-ar",
        ...
    },
    cxx_builtin_include_directories = [
        "/usr/lib/llvm-21/lib/clang/21/include",
    ],
```

New:
```python
    tool_paths = {
        "gcc": "/usr/bin/clang-22",
        "g++": "/usr/bin/clang++-22",
        "cpp": "/usr/bin/clang++-22",
        "ar": "/usr/lib/llvm-22/bin/llvm-ar",
        "nm": "/usr/lib/llvm-22/bin/llvm-nm",
        "ld": "/usr/bin/ld.lld",
        "objcopy": "/usr/bin/llvm-objcopy-22",
        "objdump": "/usr/bin/llvm-objdump-22",
        "strip": "/usr/lib/llvm-22/bin/llvm-strip",
    },
    cxx_builtin_include_directories = [
        "/usr/lib/llvm-22/lib/clang/22/include",
    ],
```

(Note: Linux paths are best-effort — the macOS host is the primary dev environment. Adjust based on actual Linux llvm-22 installation.)

- [ ] **Step 4: Add libc++ include paths to compile flags**

Add `-isystem` flags to `_CROSS_COMPILE_FLAGS` to include libc++ freestanding headers:

```python
_CROSS_COMPILE_FLAGS = [
    "-target", "x86_64-unknown-elf",
    "-ffreestanding", "-nostdlib",
    "-std=c++26",
    "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wno-c99-designator",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mgeneral-regs-only", "-mcmodel=kernel",
]
```

Note: The libc++ include paths will be added via each `cc_library`/`cc_binary` target's `copts` since Bazel's toolchain doesn't directly support per-target `-isystem` ordering. We'll use a different approach — add the include dirs to `cxx_builtin_include_directories`:

For macOS:
```python
    cxx_builtin_include_directories = [
        "kernel/lib/libcpp",                                          # custom __config_site (must be first)
        "/usr/local/Cellar/llvm/22.1.4/include/c++/v1",              # libc++ headers
        "/usr/local/Cellar/llvm/22.1.4/lib/clang/22/include",        # clang builtins
    ],
```

For Linux:
```python
    cxx_builtin_include_directories = [
        "kernel/lib/libcpp",                     # custom __config_site (must be first)
        "/usr/lib/llvm-22/include/c++/v1",       # libc++ headers
        "/usr/lib/llvm-22/lib/clang/22/include", # clang builtins
    ],
```

- [ ] **Step 5: Commit**

```bash
git add kernel/lib/libcpp/__config_site toolchain/BUILD.bazel
git commit -m "build: switch to Homebrew Clang 22.1.4 with libc++ freestanding"
```

---

### Task 2: C Runtime Stubs

**Files:**
- Create: `kernel/lib/crt_stubs.hpp`
- Create: `kernel/lib/crt_stubs.cpp`
- Modify: `kernel/lib/BUILD.bazel`

- [ ] **Step 1: Write the header**

```cpp
// kernel/lib/crt_stubs.hpp
#pragma once
#include <stddef.h>

extern "C" {

auto memcpy(void* dst, const void* src, size_t n) -> void*;
auto memmove(void* dst, const void* src, size_t n) -> void*;
auto memset(void* dst, int c, size_t n) -> void*;
auto memcmp(const void* a, const void* b, size_t n) -> int;

}
```

- [ ] **Step 2: Write the implementation**

```cpp
// kernel/lib/crt_stubs.cpp
#include "kernel/lib/crt_stubs.hpp"

extern "C" {

auto memcpy(void* dst, const void* src, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

auto memmove(void* dst, const void* src, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

auto memset(void* dst, int c, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; i++) d[i] = static_cast<unsigned char>(c);
    return dst;
}

auto memcmp(const void* a, const void* b, size_t n) -> int {
    auto* pa = static_cast<const unsigned char*>(a);
    auto* pb = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
    }
    return 0;
}

}
```

- [ ] **Step 3: Update `kernel/lib/BUILD.bazel`**

Old:
```python
cc_library(
    name = "klib",
    srcs = [
        "klog.cpp",
        "panic.cpp",
        "serial.cpp",
        "spinlock.cpp",
    ],
    hdrs = [
        "klog.hpp",
        "panic.hpp",
        "serial.hpp",
        "spinlock.hpp",
    ],
    deps = [
        "//third_party/limine:limine",
    ],
    visibility = ["//visibility:public"],
)
```

New:
```python
cc_library(
    name = "klib",
    srcs = [
        "crt_stubs.cpp",
        "klog.cpp",
        "panic.cpp",
        "serial.cpp",
        "spinlock.cpp",
    ],
    hdrs = [
        "crt_stubs.hpp",
        "klog.hpp",
        "panic.hpp",
        "serial.hpp",
        "spinlock.hpp",
    ],
    deps = [
        "//third_party/limine:limine",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Commit**

```bash
git add kernel/lib/crt_stubs.hpp kernel/lib/crt_stubs.cpp kernel/lib/BUILD.bazel
git commit -m "feat: add C runtime stubs (memcpy/memset/memmove/memcmp) for libc++"
```

---

### Task 3: New Utility Headers

**Files:**
- Create: `kernel/lib/result.hpp`
- Create: `kernel/lib/scoped_lock.hpp`
- Create: `kernel/lib/scoped_mem.hpp`
- Create: `kernel/lib/bitwise.hpp`
- Create: `kernel/lib/user_types.hpp`
- Modify: `kernel/lib/BUILD.bazel`

- [ ] **Step 1: Write `kernel/lib/result.hpp`**

```cpp
// kernel/lib/result.hpp
#pragma once
#include <utility>     // std::move
#include <type_traits> // std::is_trivially_destructible_v

namespace km {

template <typename T>
class Result {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Result<T> supports only trivially-destructible types (no heap cleanup)");

public:
    static auto Ok(T val) noexcept -> Result { return Result(std::move(val), true); }
    static auto Err(int error) noexcept -> Result { return Result(error); }

    Result(Result&& other) noexcept : ok_(other.ok_) {
        if (ok_) new (&value_) T(std::move(other.value_));
        else error_ = other.error_;
    }

    auto operator=(Result&& other) noexcept -> Result& {
        if (this != &other) {
            if (ok_) value_.~T();
            ok_ = other.ok_;
            if (ok_) new (&value_) T(std::move(other.value_));
            else error_ = other.error_;
        }
        return *this;
    }

    Result(const Result&) = delete;
    auto operator=(const Result&) = delete;

    ~Result() { if (ok_) value_.~T(); }

    explicit operator bool() const noexcept { return ok_; }
    auto value() noexcept -> T& { return value_; }
    auto value() const noexcept -> const T& { return value_; }
    auto take_value() noexcept -> T&& { return std::move(value_); }
    auto error() const noexcept -> int { return error_; }

private:
    union { T value_; int error_; };
    bool ok_;

    Result(T val, bool ok) noexcept : value_(std::move(val)), ok_(ok) {}
    explicit Result(int err) noexcept : error_(err), ok_(false) {}
};

} // namespace km
```

- [ ] **Step 2: Write `kernel/lib/scoped_lock.hpp`**

```cpp
// kernel/lib/scoped_lock.hpp
#pragma once

namespace km {

template <typename Lock>
class ScopedLock {
    Lock& lock_;
public:
    explicit ScopedLock(Lock& l) noexcept : lock_(l) { lock_.lock(); }
    ~ScopedLock() { lock_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    auto operator=(const ScopedLock&) = delete;
};

} // namespace km
```

- [ ] **Step 3: Write `kernel/lib/scoped_mem.hpp`**

```cpp
// kernel/lib/scoped_mem.hpp
#pragma once
#include <stddef.h>

// Forward declare: kmalloc/kfree from slab.hpp
extern void  kfree(void* ptr);

namespace km {

class ScopedMem {
    void* ptr_ = nullptr;
public:
    explicit ScopedMem(void* p) noexcept : ptr_(p) {}
    ~ScopedMem() { if (ptr_) kfree(ptr_); }

    ScopedMem(ScopedMem&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    auto operator=(ScopedMem&& other) noexcept -> ScopedMem& {
        if (this != &other) { if (ptr_) kfree(ptr_); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }

    ScopedMem(const ScopedMem&) = delete;
    auto operator=(const ScopedMem&) = delete;

    auto release() noexcept -> void* { auto* p = ptr_; ptr_ = nullptr; return p; }
    auto get() const noexcept -> void* { return ptr_; }
};

} // namespace km
```

- [ ] **Step 4: Write `kernel/lib/bitwise.hpp`**

```cpp
// kernel/lib/bitwise.hpp
#pragma once
#include <stddef.h>

namespace km {

template <typename T>
auto copy_bytes(T* dst, const T* src, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = src[i];
}

template <typename T>
auto zero_bytes(T* dst, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = T{};
}

} // namespace km
```

- [ ] **Step 5: Write `kernel/lib/user_types.hpp`**

```cpp
// kernel/lib/user_types.hpp
// Shared type aliases for ring-3 user-space programs (freestanding — no stdlib)
#pragma once

using int32_t   = int;
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using size_t    = decltype(sizeof(0));
using nullptr_t = decltype(nullptr);
```

- [ ] **Step 6: Update `kernel/lib/BUILD.bazel` to expose new headers**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "klib",
    srcs = [
        "crt_stubs.cpp",
        "klog.cpp",
        "panic.cpp",
        "serial.cpp",
        "spinlock.cpp",
    ],
    hdrs = [
        "crt_stubs.hpp",
        "klog.hpp",
        "panic.hpp",
        "serial.hpp",
        "spinlock.hpp",
    ],
    deps = [
        "//third_party/limine:limine",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "utils",
    hdrs = [
        "result.hpp",
        "scoped_lock.hpp",
        "scoped_mem.hpp",
        "bitwise.hpp",
        "user_types.hpp",
    ],
    deps = [
        ":klib",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 7: Commit**

```bash
git add kernel/lib/result.hpp kernel/lib/scoped_lock.hpp kernel/lib/scoped_mem.hpp \
        kernel/lib/bitwise.hpp kernel/lib/user_types.hpp kernel/lib/BUILD.bazel
git commit -m "feat: add utility headers (Result, ScopedLock, ScopedMem, bitwise, user_types)"
```

---

### Task 4: Code Style — `kernel/lib/` Headers & Source

**Files to modify:**
- `kernel/lib/spinlock.hpp`
- `kernel/lib/klog.hpp`
- `kernel/lib/panic.hpp`
- `kernel/lib/serial.hpp`
- `kernel/lib/klog.cpp`
- `kernel/lib/panic.cpp`
- `kernel/lib/serial.cpp`
- `kernel/lib/spinlock.cpp`

- [ ] **Step 1: `kernel/lib/spinlock.hpp`** — trailing return + noexcept

```cpp
// kernel/lib/spinlock.hpp
#pragma once
#include <stdint.h>

class SpinLock {
public:
    auto lock() -> void;
    auto unlock() -> void;
    auto try_lock() -> bool;

private:
    volatile uint32_t locked_{0};
};
```

- [ ] **Step 2: `kernel/lib/klog.hpp`** — trailing return, remove C-style forward declaration

```cpp
// kernel/lib/klog.hpp
#pragma once
#include <stdint.h>

struct limine_framebuffer;

auto klog_init(limine_framebuffer* fb) -> void;
auto klog(const char* msg) -> void;
auto klog_hex(uint64_t val) -> void;

// Re-init with a new framebuffer (for SMP bringup)
auto klog_reinit(limine_framebuffer* fb) -> void;
```

- [ ] **Step 3: `kernel/lib/panic.hpp`** — trailing return + [[noreturn]]

```cpp
// kernel/lib/panic.hpp
#pragma once

[[noreturn]] auto kpanic(const char* msg) -> void;
#define KPANIC(msg) kpanic(msg)
```

- [ ] **Step 4: `kernel/lib/serial.hpp`** — trailing return

```cpp
// kernel/lib/serial.hpp
#pragma once
#include <stdint.h>

auto serial_init() -> void;
auto serial_putc(char c) -> void;
auto serial_puts(const char* s) -> void;
auto serial_getc() -> uint8_t;
auto serial_has_data() -> bool;
```

- [ ] **Step 5: `kernel/lib/spinlock.cpp`** — trailing return

Current:
```cpp
#include "kernel/lib/spinlock.hpp"

void SpinLock::lock() { ... }
void SpinLock::unlock() { ... }
bool SpinLock::try_lock() { ... }
```

Change to:
```cpp
#include "kernel/lib/spinlock.hpp"

auto SpinLock::lock() -> void { ... }
auto SpinLock::unlock() -> void { ... }
auto SpinLock::try_lock() -> bool { ... }
```

(The function bodies don't change — only the signature format.)

- [ ] **Step 6: `kernel/lib/klog.cpp`** — update function signatures to trailing return

Replace `void klog_init(` → `auto klog_init(`
Replace `void klog_reinit(` → `auto klog_reinit(`
Replace `void klog(` → `auto klog(`
Replace `void klog_hex(` → `auto klog_hex(`

Add `-> void` before each opening brace. (Use editor search-and-replace.)

- [ ] **Step 7: `kernel/lib/panic.cpp`** — trailing return

Replace `void kpanic(` → `auto kpanic(` with `-> void` before brace.

- [ ] **Step 8: `kernel/lib/serial.cpp`** — trailing return on all functions

Apply Pattern A to all five functions.

- [ ] **Step 9: Commit**

```bash
git add kernel/lib/
git commit -m "style: trailing return types + noexcept on kernel/lib/"
```

---

### Task 5: Code Style — `kernel/arch/x86_64/` Headers

**Files to modify:**
- `kernel/arch/x86_64/paging.hpp`
- `kernel/arch/x86_64/gdt.hpp`
- `kernel/arch/x86_64/idt.hpp`
- `kernel/arch/x86_64/irq.hpp`
- `kernel/arch/x86_64/timer.hpp`
- `kernel/arch/x86_64/apic.hpp`
- `kernel/arch/x86_64/smp.hpp`
- `kernel/arch/x86_64/msr.hpp`
- `kernel/arch/x86_64/io.hpp`
- `kernel/arch/x86_64/usermode.hpp`
- `kernel/arch/x86_64/acpi.hpp`
- `kernel/arch/x86_64/pci.hpp`
- `kernel/arch/x86_64/syscall.hpp`

- [ ] **Step 1: `kernel/arch/x86_64/paging.hpp`** — trailing return + noexcept on inline helpers

Apply Pattern A (trailing return) and Pattern B (noexcept) to all inline functions:

```cpp
#pragma once
#include <stdint.h>

extern uint64_t g_hhdm;

constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t LARGE_PAGE_SIZE = 0x200000;
constexpr uint64_t HUGE_PAGE_SIZE = 0x40000000;
constexpr uint16_t PAGE_TABLE_ENTRIES = 512;

namespace PageFlags {
    constexpr uint64_t Present    = 1ULL << 0;
    // ... (unchanged)
}

struct alignas(PAGE_SIZE) PageTable {
    uint64_t entries[PAGE_TABLE_ENTRIES];
};

inline auto pml4_index(uint64_t va) noexcept -> uint16_t { return (va >> 39) & 0x1FF; }
inline auto pdpt_index(uint64_t va) noexcept -> uint16_t { return (va >> 30) & 0x1FF; }
inline auto pd_index(uint64_t va)   noexcept -> uint16_t { return (va >> 21) & 0x1FF; }
inline auto pt_index(uint64_t va)   noexcept -> uint16_t { return (va >> 12) & 0x1FF; }

constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;

inline auto phys_to_virt(uint64_t phys_addr) noexcept -> void* {
    return reinterpret_cast<void*>(DIRECT_MAP_BASE + phys_addr);
}

inline auto virt_to_phys(const void* virt_addr) noexcept -> uint64_t {
    return reinterpret_cast<uint64_t>(virt_addr) - DIRECT_MAP_BASE;
}

auto paging_init(uint64_t hhdm, uint64_t kernel_phys_base,
                 uint64_t kernel_virt_base, uint64_t kernel_size) -> void;
auto paging_save_kernel_template() -> void;
auto paging_kernel_pml4_template() -> uint64_t;
auto page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) -> bool;
auto page_table_unmap(uint64_t pml4_phys, uint64_t va) -> uint64_t;
auto page_table_lookup(uint64_t pml4_phys, uint64_t va) -> uint64_t;

inline constexpr auto make_pte(uint64_t phys_addr, uint64_t flags) noexcept -> uint64_t {
    return (phys_addr & ~(PAGE_SIZE - 1)) | flags;
}

inline constexpr auto pte_phys_addr(uint64_t entry) noexcept -> uint64_t {
    return entry & ~(PAGE_SIZE - 1);
}
```

- [ ] **Step 2: `kernel/arch/x86_64/gdt.hpp`** — trailing return

```cpp
#pragma once
#include <stdint.h>

struct GDTEntry { uint64_t data; };
struct GDTR { uint16_t limit; uint64_t base; } __attribute__((packed));

auto gdt_init() -> void;
```

- [ ] **Step 3: `kernel/arch/x86_64/idt.hpp`** — trailing return

```cpp
#pragma once

auto idt_init() -> void;
```

- [ ] **Step 4: `kernel/arch/x86_64/irq.hpp`** — trailing return (using already modern)

```cpp
#pragma once
#include <stdint.h>

using irq_handler_t = auto (*)(uint8_t vector) -> bool;

auto irq_init() -> void;
auto irq_register(uint8_t irq, irq_handler_t handler) -> int;
extern "C" auto irq_dispatch(uint8_t vector) -> void;
```

- [ ] **Step 5: `kernel/arch/x86_64/timer.hpp`** — trailing return

```cpp
#pragma once
#include <stdint.h>

auto timer_init() -> void;
```

- [ ] **Step 6: `kernel/arch/x86_64/apic.hpp`** — trailing return on all functions

Apply Pattern A to all function declarations:
```cpp
auto lapic_init(uint64_t base) -> void;
auto lapic_eoi() -> void;
auto lapic_timer_calibrate() -> void;
// ...
```

- [ ] **Step 7: `kernel/arch/x86_64/smp.hpp`** — trailing return

Apply Pattern A to all function declarations. Keep `struct PerCpu` and `struct ScopedSpinlock` member functions with trailing return.

```cpp
struct ScopedSpinlock {
    Spinlock* lock_;
    explicit ScopedSpinlock(Spinlock* l) : lock_(l) { lock_->lock(); }
    ~ScopedSpinlock() { if (lock_) lock_->unlock(); }
    ScopedSpinlock(const ScopedSpinlock&) = delete;
    auto operator=(const ScopedSpinlock&) -> ScopedSpinlock& = delete;
    ScopedSpinlock(ScopedSpinlock&&) = delete;
    auto operator=(ScopedSpinlock&&) -> ScopedSpinlock& = delete;
};
```

- [ ] **Step 8: `kernel/arch/x86_64/msr.hpp`** — keep as-is (simple inline wrappers, trailing return already implicit with `inline`)

Add noexcept:
```cpp
namespace x86 {
inline auto rdmsr(uint32_t msr) noexcept -> uint64_t { ... }
inline auto wrmsr(uint32_t msr, uint64_t val) noexcept -> void { ... }
}
```

- [ ] **Step 9: `kernel/arch/x86_64/io.hpp`** — add noexcept

```cpp
#pragma once
#include <stdint.h>

inline auto outb(uint16_t port, uint8_t val) noexcept -> void {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
inline auto inb(uint16_t port) noexcept -> uint8_t {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
```

- [ ] **Step 10: `kernel/arch/x86_64/usermode.hpp`** — trailing return

```cpp
#pragma once
#include <stdint.h>

struct Thread;
class Process;

auto enter_usermode(Thread* t) -> void;
auto usermode_switch(Thread* prev, Thread* next) -> void;
```

- [ ] **Step 11: `kernel/arch/x86_64/acpi.hpp`** — trailing return

Apply Pattern A to all function declarations.

- [ ] **Step 12: `kernel/arch/x86_64/pci.hpp`** — trailing return

Apply Pattern A to all function declarations.

- [ ] **Step 13: `kernel/arch/x86_64/syscall.hpp`** — trailing return

```cpp
#pragma once
#include <stdint.h>

constexpr int SYSCALL_DEBUG_PRINT    = 0;
constexpr int SYSCALL_HANDLE_CLOSE   = 1;
constexpr int SYSCALL_HANDLE_DUP     = 2;
constexpr int SYSCALL_CHANNEL_CREATE = 10;
constexpr int SYSCALL_CHANNEL_WRITE  = 11;
constexpr int SYSCALL_CHANNEL_READ   = 12;
constexpr int SYSCALL_CHANNEL_READ_HANDLES = 13;
constexpr int SYSCALL_PORT_CREATE    = 20;
constexpr int SYSCALL_PORT_REGISTER  = 21;
constexpr int SYSCALL_PORT_CONNECT   = 22;
constexpr int SYSCALL_PORT_ACCEPT    = 23;
constexpr int SYSCALL_PROCESS_CREATE = 30;
constexpr int SYSCALL_PROCESS_EXIT   = 31;
constexpr int SYSCALL_VMO_CREATE     = 40;
constexpr int SYSCALL_VMO_MAP        = 41;
constexpr int SYSCALL_OPEN           = 50;
constexpr int SYSCALL_MOUNT          = 51;
constexpr int SYSCALL_BLKDEV_READ    = 52;
constexpr int SYSCALL_BLKDEV_WRITE   = 53;
constexpr int SYSCALL_SERIAL_READ    = 54;

struct SyscallArgs {
    uint64_t a1, a2, a3, a4;
};

using syscall_handler_t = auto (*)(SyscallArgs args) -> uint64_t;

auto syscall_init() -> void;
auto syscall_set_handler(syscall_handler_t h) -> void;

extern "C" auto syscall_entry() -> void;
extern "C" auto syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4) -> uint64_t;
```

- [ ] **Step 14: Commit**

```bash
git add kernel/arch/x86_64/*.hpp
git commit -m "style: trailing return types + noexcept on arch/x86_64 headers"
```

---

### Task 6: Code Style — `kernel/arch/x86_64/` Source Files

**Files to modify:**
- `kernel/arch/x86_64/boot.cpp`
- `kernel/arch/x86_64/gdt.cpp`
- `kernel/arch/x86_64/idt.cpp`
- `kernel/arch/x86_64/irq.cpp`
- `kernel/arch/x86_64/apic.cpp`
- `kernel/arch/x86_64/smp.cpp`
- `kernel/arch/x86_64/paging.cpp`
- `kernel/arch/x86_64/pci.cpp`
- `kernel/arch/x86_64/acpi.cpp`
- `kernel/arch/x86_64/timer.cpp`
- `kernel/arch/x86_64/syscall.cpp`

- [ ] **Step 1: `kernel/arch/x86_64/boot.cpp`** — trailing return on all functions

Apply Pattern A to every function definition. For `kernel_main` (called from assembly):
```cpp
extern "C" auto kernel_main() -> void { ... }
```

For all other functions, convert return types to trailing form.

- [ ] **Step 2: `kernel/arch/x86_64/gdt.cpp`** — trailing return

Convert all function signatures. Body code stays the same.

- [ ] **Step 3: `kernel/arch/x86_64/idt.cpp`** — trailing return

Convert all function signatures including `isr_common` and `irq_stub_*` if they appear as C++ functions (keep `extern "C"` for assembly entry points).

- [ ] **Step 4: `kernel/arch/x86_64/irq.cpp`** — trailing return

```cpp
auto irq_init() -> void { ... }
auto irq_register(uint8_t irq, irq_handler_t handler) -> int { ... }
extern "C" auto irq_dispatch(uint8_t vector) -> void { ... }
```

- [ ] **Step 5: `kernel/arch/x86_64/apic.cpp`** — trailing return

Apply Pattern A to all statics and global functions. Use `auto` for redundant casts (Pattern C).

- [ ] **Step 6: `kernel/arch/x86_64/smp.cpp`** — trailing return

Apply Pattern A to all functions. Replace `auto*` for redundant casts.

- [ ] **Step 7: `kernel/arch/x86_64/paging.cpp`** — trailing return

Apply Pattern A and C.

- [ ] **Step 8: `kernel/arch/x86_64/pci.cpp`** — trailing return

Apply Pattern A.

- [ ] **Step 9: `kernel/arch/x86_64/acpi.cpp`** — trailing return

Apply Pattern A.

- [ ] **Step 10: `kernel/arch/x86_64/timer.cpp`** — trailing return

Apply Pattern A.

- [ ] **Step 11: `kernel/arch/x86_64/syscall.cpp`** — trailing return on non-handler functions

Apply Pattern A to `syscall_init`, `init_syscall_table`, and the dispatch function (but see Task 11 for full refactor — for now just style changes). Keep `syscall_dispatcher` as `extern "C"`.

- [ ] **Step 12: Commit**

```bash
git add kernel/arch/x86_64/*.cpp
git commit -m "style: trailing return types + auto on arch/x86_64 source files"
```

---

### Task 7: Code Style — `kernel/core/` Headers

**Files to modify:**
- `kernel/core/object/object.hpp`
- `kernel/core/object/rights.hpp`
- `kernel/core/object/handle_table.hpp`
- `kernel/core/object/channel.hpp`
- `kernel/core/object/port.hpp`
- `kernel/core/object/process.hpp`
- `kernel/core/mm/pmm.hpp`
- `kernel/core/mm/bitmap_alloc.hpp`
- `kernel/core/mm/buddy.hpp`
- `kernel/core/mm/slab.hpp`
- `kernel/core/mm/vmo.hpp`
- `kernel/core/mm/vmm.hpp`
- `kernel/core/sched/sched.hpp`
- `kernel/core/blk/ahci.hpp`
- `kernel/core/blk/blkdev.hpp`
- `kernel/core/blk/bufcache.hpp`
- `kernel/core/elf_loader.hpp`

- [ ] **Step 1: `kernel/core/object/object.hpp`** — trailing return + noexcept

```cpp
#pragma once
#include <stdint.h>

class KernelObject {
public:
    enum class Type : uint8_t { Channel, Port, Process, Vmo };

    auto type() const noexcept -> Type { return type_; }
    auto refcount() const noexcept -> uint32_t { return ref_count_; }

    auto AddRef() noexcept -> void { ref_count_++; }
    auto Release() -> void;

protected:
    explicit KernelObject(Type t) noexcept : type_(t), ref_count_(1) {}
    virtual ~KernelObject() = default;

private:
    Type type_;
    uint32_t ref_count_;
};
```

- [ ] **Step 2: `kernel/core/object/rights.hpp`** — no style changes needed (already clean)

- [ ] **Step 3: `kernel/core/object/handle_table.hpp`** — trailing return (typed_lookup added in Task 10)

Apply Pattern A to all member functions:

```cpp
auto Init() -> void;
auto Alloc(KernelObject* obj, Rights rights) -> handle_t;
auto Free(handle_t h) -> void;
auto Lookup(handle_t h, Rights needed = Rights{}, Rights* out_rights = nullptr) -> KernelObject*;
auto ForEach(KernelObject** out_objs, handle_t* out_handles, int max) -> int;
auto Destroy() -> void;
```

Global helpers:
```cpp
auto handle_table_set_fallback(HandleTable* ht) -> void;
auto handle_alloc(KernelObject* obj, Rights rights) -> handle_t;
auto handle_free(handle_t h) -> void;
auto handle_lookup(handle_t h, Rights needed = Rights{}, Rights* out_rights = nullptr) -> KernelObject*;
```

- [ ] **Step 4: `kernel/core/object/channel.hpp`** — trailing return

Apply Pattern A. Add `static constexpr auto kType = KernelObject::Type::Channel;`:

```cpp
class Channel : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Channel;
    Channel() : KernelObject(Type::Channel) {}

    struct Message {
        uint8_t*  data;
        size_t    data_len;
        handle_t* handles;
        size_t    num_handles;
        Message*  next;
    };

    auto Write(const void* data, size_t len,
               const handle_t* handles, size_t num_handles,
               bool from_endpoint_b = false) -> int;
    auto Read(void* buf, size_t buf_size, size_t* out_len,
              handle_t* handle_buf, size_t buf_capacity,
              size_t* out_num_handles,
              bool from_endpoint_b = false) -> int;

private:
    SpinLock lock_;
    Message* head_a_ = nullptr;
    Message* tail_a_ = nullptr;
    Message* head_b_ = nullptr;
    Message* tail_b_ = nullptr;

    auto Enqueue(Message* msg, Message** head, Message** tail) -> void;
    auto Dequeue(Message** head, Message** tail) -> Message*;
};
```

- [ ] **Step 5: `kernel/core/object/port.hpp`** — trailing return + kType

```cpp
class Port : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Port;
    // ... trailing return on all methods
};
```

- [ ] **Step 6: `kernel/core/object/process.hpp`** — trailing return + kType

```cpp
class Process : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Process;
    static auto Create(const char* name, bool kernel_process = false) -> Process*;
    // ... trailing return on all methods
};
```

- [ ] **Step 7: `kernel/core/mm/pmm.hpp`** — trailing return + noexcept

Apply Pattern A to all function declarations.

- [ ] **Step 8: `kernel/core/mm/bitmap_alloc.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 9: `kernel/core/mm/buddy.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 10: `kernel/core/mm/slab.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 11: `kernel/core/mm/vmo.hpp`** — trailing return + kType

```cpp
class Vmo : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Vmo;
    // ... trailing return on all methods
};
```

- [ ] **Step 12: `kernel/core/mm/vmm.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 13: `kernel/core/sched/sched.hpp`** — trailing return

Apply Pattern A. Keep `[[noreturn]]` on `thread_exit()`.

- [ ] **Step 14: `kernel/core/blk/` headers** — trailing return

Apply Pattern A to `ahci.hpp`, `blkdev.hpp`, `bufcache.hpp`.

- [ ] **Step 15: `kernel/core/elf_loader.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 16: Commit**

```bash
git add kernel/core/
git commit -m "style: trailing return types + noexcept on core headers"
```

---

### Task 8: Code Style — `kernel/core/` + `kernel/fs/` Source Files

**Files to modify:**
- `kernel/core/object/object.cpp`
- `kernel/core/object/handle_table.cpp`
- `kernel/core/object/channel.cpp`
- `kernel/core/object/port.cpp`
- `kernel/core/object/process.cpp`
- `kernel/core/mm/pmm.cpp`
- `kernel/core/mm/bitmap_alloc.cpp`
- `kernel/core/mm/buddy.cpp`
- `kernel/core/mm/slab.cpp`
- `kernel/core/mm/vmo.cpp`
- `kernel/core/mm/vmm.cpp`
- `kernel/core/mm/new_delete.cpp`
- `kernel/core/sched/sched.cpp`
- `kernel/core/blk/ahci.cpp`
- `kernel/core/blk/blkdev.cpp`
- `kernel/core/blk/bufcache.cpp`
- `kernel/core/elf_loader.cpp`
- `kernel/fs/mount.cpp`
- `kernel/fs/mount.hpp`
- `kernel/fs/protocol.hpp`

- [ ] **Step 1: `kernel/core/object/object.cpp`** — trailing return

```cpp
auto KernelObject::Release() -> void {
    ref_count_--;
    if (ref_count_ == 0) {
        kfree(this);
    }
}
```

- [ ] **Step 2: `kernel/core/object/handle_table.cpp`** — trailing return

Apply Pattern A to all member function definitions.

- [ ] **Step 3: `kernel/core/object/channel.cpp`** — trailing return + use copy_bytes

```cpp
auto Channel::Write(...) -> int {
    auto* msg = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (!msg) return -1;

    // ... (use km::copy_bytes for data copy, see Task 10 for full Message refactor)
}
```

For now, just change function signatures and use `auto*` for redundant casts.

- [ ] **Step 4: `kernel/core/object/port.cpp`** — trailing return

Apply Pattern A. Use `auto*` for redundant casts.

- [ ] **Step 5: `kernel/core/object/process.cpp`** — trailing return

Apply Pattern A. Use `auto*` for redundant casts.

- [ ] **Step 6: `kernel/core/mm/` source files** — trailing return

Apply Pattern A to all function definitions in every `.cpp` file. Use `auto*` for redundant casts. Add `noexcept` to inline helpers already marked `noexcept` in headers.

- [ ] **Step 7: `kernel/core/sched/sched.cpp`** — trailing return

Apply Pattern A. Note: `switch_to` is in `switch.S`, not affected.

- [ ] **Step 8: `kernel/core/blk/` + `kernel/core/elf_loader.cpp`** — trailing return

Apply Pattern A.

- [ ] **Step 9: `kernel/fs/mount.hpp`** — trailing return

Apply Pattern A.

- [ ] **Step 10: `kernel/fs/protocol.hpp`** — no changes (POD structs, already clean)

- [ ] **Step 11: `kernel/fs/mount.cpp`** — trailing return

Apply Pattern A.

- [ ] **Step 12: Commit**

```bash
git add kernel/core/ kernel/fs/
git commit -m "style: trailing return types on core + fs source files"
```

---

### Task 9: Object Manager Modernization

**Files to modify:**
- `kernel/core/object/channel.hpp`
- `kernel/core/object/channel.cpp`
- `kernel/core/object/handle_table.hpp`
- `kernel/core/object/handle_table.cpp`

- [ ] **Step 1: Add `typed_lookup<T>` to `kernel/core/object/handle_table.hpp`**

After the `HandleTable` class, add:

```cpp
#include "kernel/lib/result.hpp"

template <typename T>
auto typed_lookup(HandleTable& table, handle_t h, Rights needed = {}) -> km::Result<T*> {
    auto* obj = table.Lookup(h, needed);
    if (!obj || obj->type() != T::kType) return km::Result<T*>::Err(-1);
    return km::Result<T*>::Ok(static_cast<T*>(obj));
}
```

- [ ] **Step 2: Add `ScopedHandle` to `kernel/core/object/handle_table.hpp`**

After `typed_lookup`, add:

```cpp
class ScopedHandle {
    HandleTable* table_ = nullptr;
    handle_t handle_ = INVALID_HANDLE;
public:
    ScopedHandle(HandleTable& t, handle_t h) noexcept : table_(&t), handle_(h) {}
    ~ScopedHandle() { if (table_ && handle_ != INVALID_HANDLE) table_->Free(handle_); }

    ScopedHandle(ScopedHandle&& other) noexcept
        : table_(other.table_), handle_(other.handle_) {
        other.table_ = nullptr;
        other.handle_ = INVALID_HANDLE;
    }

    auto operator=(ScopedHandle&& other) noexcept -> ScopedHandle& {
        if (this != &other) {
            if (table_ && handle_ != INVALID_HANDLE) table_->Free(handle_);
            table_ = other.table_;
            handle_ = other.handle_;
            other.table_ = nullptr;
            other.handle_ = INVALID_HANDLE;
        }
        return *this;
    }

    ScopedHandle(const ScopedHandle&) = delete;
    auto operator=(const ScopedHandle&) = delete;

    auto get() const noexcept -> handle_t { return handle_; }
    auto release() noexcept -> handle_t {
        auto h = handle_; handle_ = INVALID_HANDLE; table_ = nullptr; return h;
    }
};
```

- [ ] **Step 3: Refactor `Channel::Message` to use `ScopedMem` in `channel.hpp`**

```cpp
#include "kernel/lib/scoped_mem.hpp"

struct Message {
    km::ScopedMem data_mem{nullptr};
    km::ScopedMem handle_mem{nullptr};
    size_t    data_len = 0;
    handle_t* handles = nullptr;
    size_t    num_handles = 0;
    Message*  next = nullptr;

    Message() = default;
    Message(Message&& other) noexcept = default;
    auto operator=(Message&& other) noexcept -> Message& = default;
    Message(const Message&) = delete;
    auto operator=(const Message&) = delete;
};
```

- [ ] **Step 4: Rewrite `Channel::Write` in `channel.cpp`**

```cpp
auto Channel::Write(const void* data, size_t len,
                    const handle_t* handles, size_t num_handles,
                    bool from_endpoint_b) -> int {
    auto* raw = static_cast<Message*>(kmalloc(sizeof(Message)));
    if (!raw) return -1;
    auto msg = km::ScopedMem{raw};
    auto* m = static_cast<Message*>(msg.get());

    // Placement-new construct Message in allocated memory
    new (m) Message{};

    m->data_len = len;
    m->num_handles = num_handles;

    if (len > 0) {
        auto* buf = static_cast<uint8_t*>(kmalloc(len));
        if (!buf) return -1;
        m->data_mem = km::ScopedMem{buf};
        km::copy_bytes(buf, static_cast<const uint8_t*>(data), len);
    }

    if (num_handles > 0) {
        auto* hbuf = static_cast<handle_t*>(kmalloc(sizeof(handle_t) * num_handles));
        if (!hbuf) return -1;
        m->handle_mem = km::ScopedMem{hbuf};
        m->handles = hbuf;
        km::copy_bytes(hbuf, handles, num_handles);
    }

    lock_.lock();
    if (from_endpoint_b) Enqueue(m, &head_a_, &tail_a_);
    else                 Enqueue(m, &head_b_, &tail_b_);
    lock_.unlock();

    // Release ownership — Message is now on the queue
    msg.release();
    return 0;
}
```

- [ ] **Step 5: Rewrite `Channel::Read` in `channel.cpp`**

```cpp
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

    // ScopedMem destructors free data and handles
    kfree(msg);
    return 0;
}
```

- [ ] **Step 6: Commit**

```bash
git add kernel/core/object/
git commit -m "refactor: typed_lookup<T>, ScopedHandle, Message RAII in channel"
```

---

### Task 10: Syscall Dispatch Modernization

**Files to modify:**
- `kernel/arch/x86_64/syscall.hpp`
- `kernel/arch/x86_64/syscall.cpp`

- [ ] **Step 1: Update handler function type in `syscall.hpp`** (already done in Task 5 Step 13)

`SyscallArgs` struct and `using syscall_handler_t = auto (*)(SyscallArgs args) -> uint64_t;` already in place from the style pass.

- [ ] **Step 2: Update handler signatures in `syscall.cpp`**

Change ALL `uint64_t sys_xxx(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)` to:
```cpp
static auto sys_debug_print(SyscallArgs args) -> uint64_t {
    if (args.a1) klog(reinterpret_cast<const char*>(args.a1));
    return 0;
}
```

- [ ] **Step 3: Rewrite `sys_channel_write` using typed_lookup**

```cpp
static auto sys_channel_write(SyscallArgs args) -> uint64_t {
    auto* proc = current_process();
    if (!proc) return static_cast<uint64_t>(-1);

    auto result = typed_lookup<Channel>(proc->handles,
                                         static_cast<handle_t>(args.a1),
                                         Rights{.mask = Rights::Write});
    if (!result) return static_cast<uint64_t>(-1);
    auto* ch = result.value();

    Rights existing;
    proc->handles.Lookup(static_cast<handle_t>(args.a1), Rights{}, &existing);
    bool endpoint_b = (existing.mask & Rights::ChannelEndpointB) != 0;

    struct ChannelWriteArgs {
        const void* data; size_t data_len;
        const handle_t* handles; size_t num_handles;
    };
    auto* wa = reinterpret_cast<const ChannelWriteArgs*>(args.a2);
    if (reinterpret_cast<uint64_t>(wa) < 4096) return static_cast<uint64_t>(-1);

    return static_cast<uint64_t>(ch->Write(
        wa->data, wa->data_len, wa->handles, wa->num_handles, endpoint_b));
}
```

- [ ] **Step 4: Rewrite ALL other handlers (sys_channel_read, sys_port_*, sys_open, etc.)**

Apply the same pattern: `SyscallArgs` parameter + `typed_lookup<T>` where applicable. Use `auto*` for redundant casts.

- [ ] **Step 5: Update dispatch table to use the new signature**

```cpp
using syscall_fn_t = auto (*)(SyscallArgs) -> uint64_t;

auto init_syscall_table() -> void {
    g_syscall_table[SYSCALL_DEBUG_PRINT]    = sys_debug_print;
    g_syscall_table[SYSCALL_HANDLE_CLOSE]   = sys_handle_close;
    // ... all others
}
```

- [ ] **Step 6: Update `syscall_dispatcher`**

```cpp
extern "C" auto syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4) -> uint64_t {
    if (num >= MAX_SYSCALL || !g_syscall_table[num]) {
        klog("Syscall #"); klog_hex(num); klog(": invalid\n");
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    SyscallArgs args{a1, a2, a3, a4};
    return g_syscall_table[num](args);
}
```

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/x86_64/syscall.hpp kernel/arch/x86_64/syscall.cpp
git commit -m "refactor: SyscallArgs struct + typed_lookup in syscall dispatch"
```

---

### Task 11: Ring-3 Programs Modernization

**Files to modify:**
- `kernel/init/init.cpp`
- `kernel/init/shell/shell.cpp`
- `kernel/fs/devfs/devfs.cpp`
- `kernel/fs/tmpfs/tmpfs.cpp`
- `kernel/fs/fat32/fat32.cpp`

- [ ] **Step 1: `kernel/init/init.cpp`** — replace type aliases + style

Replace the top 9 lines:
```cpp
using int32_t = int; using uint8_t = unsigned char; ...
```

With:
```cpp
#include "kernel/lib/user_types.hpp"
```

Apply trailing return to all local static functions. Use `auto` for redundant types.

- [ ] **Step 2: `kernel/init/shell/shell.cpp`** — replace type aliases + style

Replace line 2-3 (`using uint64_t = ...; using uint32_t = ...;`) with:
```cpp
#include "kernel/lib/user_types.hpp"
```

Reformat the compressed declarations (lines 7-10) to one-per-line with trailing return:
```cpp
struct FileMsg { enum Op : uint32_t { Open=0,Read=1,Write=2,Seek=3,Stat=4,Close=5,Readdir=6 }; Op op; uint32_t flags; uint64_t offset, length; };
```

Becomes:
```cpp
struct FileMsg {
    enum Op : uint32_t { Open = 0, Read = 1, Write = 2, Seek = 3, Stat = 4, Close = 5, Readdir = 6 };
    Op op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};
```

Convert syscall helper functions to trailing return:
```cpp
static auto syscall6(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) -> uint64_t { ... }
static auto p(const char* m) -> void { ... }
static auto cw(uint32_t h, const void* d, size_t n) -> int { ... }
```

- [ ] **Step 3: `kernel/fs/devfs/devfs.cpp`** — replace type aliases + style

Replace the type alias block with `#include "kernel/lib/user_types.hpp"`. Apply trailing return to all functions.

- [ ] **Step 4: `kernel/fs/tmpfs/tmpfs.cpp`** — replace type aliases + style

Same as Step 3.

- [ ] **Step 5: `kernel/fs/fat32/fat32.cpp`** — replace type aliases + style

Same as Step 3.

- [ ] **Step 6: Update ring-3 BUILD files**

For each ring-3 `cc_binary` target (`init/`, `shell/`, `devfs/`, `tmpfs/`, `fat32/`), add:
```python
cc_binary(
    name = "...",
    srcs = ["..."],
    additional_linker_inputs = ["..."],
    copts = [
        "-mcmodel=small",
        "-O2",
    ],
    linkopts = ["-T", "$(location ...)"],
    deps = [
        "//kernel/lib:utils",
    ],
    visibility = ["//visibility:private"],
)
```

The `deps = ["//kernel/lib:utils"]` line is new — it brings in `user_types.hpp` and the C runtime stubs.

- [ ] **Step 7: Commit**

```bash
git add kernel/init/ kernel/fs/
git commit -m "style: modernize ring-3 programs — user_types.hpp, trailing return, reformat"
```

---

### Task 12: Build Integration & QEMU Verify

**Files to modify:**
- `kernel/BUILD.bazel` — add `//kernel/lib:utils` to kernel deps

- [ ] **Step 1: Update `kernel/BUILD.bazel` to depend on utils**

```python
cc_binary(
    name = "kernel",
    srcs = [
        "core/elf_loader.hpp",
        "core/elf_loader.cpp",
        "core/elf_trampoline.S",
        "//kernel/init:init_embed",
        "//kernel/fs/devfs:devfs_embed",
        "//kernel/fs/tmpfs:tmpfs_embed",
        "//kernel/fs/fat32:fat32_embed",
        "//kernel/init/shell:shell_embed",
    ],
    deps = [
        "//kernel/arch/x86_64:arch",
        "//kernel/lib:klib",
        "//kernel/lib:utils",
        "//kernel/core/mm:mm",
        "//kernel/core/mm:vmo",
        "//kernel/core/mm:vmm",
        "//kernel/core/sched:sched",
        "//kernel/core/blk:blk",
    ],
    additional_linker_inputs = ["//kernel/arch/x86_64:linker_script"],
    linkopts = ["-Wl,-T,$(location //kernel/arch/x86_64:linker_script)"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 2: Build kernel ELF**

```bash
bazel build //kernel:kernel
```

Expected: successful compilation with no errors.

If errors occur: fix style issues, missing includes, or function signature mismatches, then re-build.

- [ ] **Step 3: Boot in QEMU**

```bash
bash scripts/run.sh
```

Expected: kernel boots, init process runs, interactive shell works. Check serial output for any regressions.

- [ ] **Step 4: Run unit tests**

```bash
bazel test //test/mm:all //test/irq:all //test/sched:all //test/object:all
```

Expected: all tests pass. (Test targets use the host toolchain, not the cross-compilation toolchain, so libc++ config changes don't affect them.)

- [ ] **Step 5: Commit**

```bash
git add kernel/BUILD.bazel
git commit -m "build: add //kernel/lib:utils to kernel deps, verify QEMU boot"
```

---

## Completion Checklist

- [ ] Toolchain builds with Homebrew Clang 22.1.4
- [ ] libc++ headers (`<type_traits>`, `<utility>`, `<span>`, etc.) usable from kernel code
- [ ] C runtime stubs linked into kernel and ring-3
- [ ] `km::Result<T>` compiles and works
- [ ] `km::ScopedLock`, `km::ScopedMem`, `km::ScopedHandle` work
- [ ] All ~70 files use trailing return types (except `extern "C"` / asm)
- [ ] All pure functions annotated `noexcept`
- [ ] Redundant `static_cast` uses `auto*`
- [ ] `typed_lookup<T>` replaces Lookup+cast pattern in syscalls
- [ ] `Channel::Message` uses RAII via ScopedMem
- [ ] `SyscallArgs` struct used in dispatch
- [ ] Ring-3 programs share `user_types.hpp`
- [ ] `bazel build //kernel:kernel` succeeds
- [ ] `bash scripts/run.sh` boots successfully
- [ ] All unit tests pass
- [ ] `git log` shows clean commit history with meaningful messages
