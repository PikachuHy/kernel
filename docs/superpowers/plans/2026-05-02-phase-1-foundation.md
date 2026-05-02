# Phase 1: Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Kernel boots via Limine on x86-64, initializes GDT/IDT, prints to framebuffer via terminal, and halts cleanly.

**Architecture:** Minimal Limine stivale2 entry point with kernel utility library (logging, panic). Runs identity-mapped; higher-half paging deferred to Phase 2. Freestanding C++26 compiled with custom Bazel 9 LLVM cross-compilation toolchain targeting `x86_64-unknown-elf`.

**Tech Stack:** Bazel 9 (MODULE.bazel), LLVM/Clang + LLD, Limine stivale2 boot protocol, QEMU for emulation, macOS dev host.

**Prerequisites:** LLVM/Clang toolchain installed (`brew install llvm` or Xcode CLT), QEMU (`brew install qemu`), Bazel 9 (`brew install bazel` or bazelisk).

---

## File Structure (this phase creates)

```
kernel/
├── MODULE.bazel
├── .bazelrc
├── .bazelversion
├── BUILD.bazel
├── toolchain/
│   ├── BUILD.bazel
│   └── x86_64_elf.BUILD
├── kernel/
│   ├── BUILD.bazel
│   ├── arch/
│   │   └── x86_64/
│       │   ├── BUILD.bazel
│       │   ├── link.ld
│       │   ├── boot.cpp
│       │   ├── gdt.hpp
│       │   ├── gdt.cpp
│       │   ├── idt.hpp
│       │   └── idt.cpp
│   └── lib/
│       ├── BUILD.bazel
│       ├── klog.hpp
│       ├── klog.cpp
│       ├── panic.hpp
│       └── panic.cpp
├── scripts/
│   ├── run.sh
│   └── debug.sh
└── third_party/
    └── limine/
        ├── BUILD.bazel
        └── stivale2.h
```

---

### Task 1: Project skeleton and Bazel workspace

**Files:**
- Create: `MODULE.bazel`
- Create: `.bazelrc`
- Create: `.bazelversion`
- Create: `BUILD.bazel`

- [ ] **Step 1: Create MODULE.bazel**

```python
module(
    name = "kernel",
    version = "0.1.0",
)
```

- [ ] **Step 2: Create .bazelrc**

```
build --incompatible_strict_action_env
build --verbose_failures
test --test_output=errors
```

- [ ] **Step 3: Create .bazelversion**

```
9.0.0
```

- [ ] **Step 4: Create top-level BUILD.bazel**

```python
filegroup(
    name = "limine_cfg",
    srcs = ["limine.cfg"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 5: Create limine.cfg (Limine boot config at repo root)**

```
TIMEOUT=5

:Kernel
    PROTOCOL=stivale2
    KERNEL_PATH=boot:///kernel.elf
```

- [ ] **Step 6: Verify Bazel version**

Run: `bazel --version`
Expected: `9.0.0` (or `bazel 9.x.x`)

- [ ] **Step 7: Commit**

```bash
git add MODULE.bazel .bazelrc .bazelversion BUILD.bazel limine.cfg
git commit -m "build: add Bazel 9 workspace skeleton with Limine config"
```

---

### Task 2: LLVM toolchain for x86-64-elf

**Files:**
- Create: `toolchain/BUILD.bazel`
- Create: `toolchain/x86_64_elf.BUILD`

- [ ] **Step 1: Create toolchain/x86_64_elf.BUILD**

```python
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "tool_path")

package(default_visibility = ["//visibility:public"])

filegroup(name = "empty")

cc_toolchain_config(
    name = "x86_64_elf_config",
    cpu = "x86_64",
    compiler = "clang",
    toolchain_identifier = "x86_64-elf-clang",
    host_system_name = "x86_64-darwin",
    target_system_name = "x86_64-unknown-elf",
    target_libc = "unknown",
    abi_version = "elf",
    abi_libc_version = "unknown",
    target_cpu = "x86_64",
    builtin_sysroot = "/dev/null",
    tool_paths = [
        tool_path(name = "gcc", path = "clang"),
        tool_path(name = "g++", path = "clang++"),
        tool_path(name = "cpp", path = "clang-cpp"),
        tool_path(name = "ar", path = "llvm-ar"),
        tool_path(name = "nm", path = "llvm-nm"),
        tool_path(name = "ld", path = "ld.lld"),
        tool_path(name = "objcopy", path = "llvm-objcopy"),
        tool_path(name = "objdump", path = "llvm-objdump"),
        tool_path(name = "strip", path = "llvm-strip"),
    ],
    cxx_builtin_include_directories = [],
    compile_flags = [
        "-target",
        "x86_64-unknown-elf",
        "-ffreestanding",
        "-nostdlib",
        "-nostdinc",
        "-std=c++26",
        "-fno-exceptions",
        "-fno-rtti",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-mno-red-zone",
        "-mno-mmx",
        "-mno-sse",
        "-mno-sse2",
        "-mgeneral-regs-only",
        "-mcmodel=kernel",
    ],
    dbg_compile_flags = [
        "-g",
        "-O0",
    ],
    opt_compile_flags = [
        "-O2",
        "-DNDEBUG",
    ],
    cxx_flags = [
        "-fno-use-cxa-atexit",
    ],
    link_flags = [
        "-target",
        "x86_64-unknown-elf",
        "-nostdlib",
        "-nostartfiles",
        "-ffreestanding",
        "-Wl,-z,max-page-size=0x1000",
        "-Wl,-build-id=none",
    ],
    dbg_link_flags = [],
    opt_link_flags = [],
)
```

- [ ] **Step 2: Create toolchain/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_toolchain")

cc_toolchain(
    name = "x86_64_elf_cc_toolchain",
    all_files = ":empty",
    compiler_files = ":empty",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 0,
    toolchain_config = ":x86_64_elf_config",
    toolchain_identifier = "x86_64-elf-clang",
)

toolchain(
    name = "x86_64_elf_toolchain",
    exec_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:osx",
    ],
    target_compatible_with = [
        "@platforms//cpu:x86_64",
        "@platforms//os:none",
    ],
    toolchain = ":x86_64_elf_cc_toolchain",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
```

- [ ] **Step 3: Register toolchain in MODULE.bazel**

Update `MODULE.bazel` to:

```python
module(
    name = "kernel",
    version = "0.1.0",
)

register_toolchains("//toolchain:x86_64_elf_toolchain")
```

- [ ] **Step 4: Verify toolchain loads**

Run: `bazel build //toolchain:... --platforms=//toolchain:x86_64_elf_toolchain 2>&1 || true`
Expected: Bazel loads and evaluates toolchain without syntax errors (may fail on missing source files — that's fine).

- [ ] **Step 5: Commit**

```bash
git add toolchain/ MODULE.bazel
git commit -m "build: add cross-compilation toolchain for x86-64-elf via LLVM/Clang"
```

---

### Task 3: Limine stivale2 header

**Files:**
- Create: `third_party/limine/stivale2.h`
- Create: `third_party/limine/BUILD.bazel`

- [ ] **Step 1: Create third_party/limine/stivale2.h**

```c
#ifndef STIVALE2_H
#define STIVALE2_H

#include <stdint.h>

struct stivale2_tag {
    uint64_t identifier;
    uint64_t next;
} __attribute__((packed));

struct stivale2_header {
    uint64_t entry_point;
    uint64_t stack;
    uint64_t flags;
    uint64_t tags;
} __attribute__((packed));

#define STIVALE2_HEADER_TAG_FRAMEBUFFER_ID 0x3ecc1bc43d0f7971
#define STIVALE2_HEADER_TAG_TERMINAL_ID    0xa85d499b1823be72
#define STIVALE2_HEADER_TAG_SMP_ID         0x1ab015085f3273df
#define STIVALE2_HEADER_TAG_5LV_PAGING_ID  0x932f477032007e8f

struct stivale2_header_tag_framebuffer {
    struct stivale2_tag tag;
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_bpp;
    uint16_t unused;
} __attribute__((packed));

struct stivale2_header_tag_terminal {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t callback;
} __attribute__((packed));

struct stivale2_header_tag_smp {
    struct stivale2_tag tag;
    uint64_t flags;
} __attribute__((packed));

struct stivale2_struct {
    char bootloader_brand[64];
    char bootloader_version[64];
    uint64_t tags;
} __attribute__((packed));

#define STIVALE2_STRUCT_TAG_CMDLINE_ID      0xe5e76a1b4597a781
#define STIVALE2_STRUCT_TAG_MEMMAP_ID       0x2187f79e8612de07
#define STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID  0x506461d2950408fa
#define STIVALE2_STRUCT_TAG_TERMINAL_ID     0xc2b3f4c3233b0974
#define STIVALE2_STRUCT_TAG_MODULES_ID      0x4b6fe466aade04ce
#define STIVALE2_STRUCT_TAG_RSDP_ID         0x9e1786930a375e78
#define STIVALE2_STRUCT_TAG_EPOCH_ID        0x566a7bed888e1407
#define STIVALE2_STRUCT_TAG_SMBIOS_ID       0x2792e5efe28c4de2
#define STIVALE2_STRUCT_TAG_SMP_ID          0x34d1d96339647025

struct stivale2_struct_tag_cmdline {
    struct stivale2_tag tag;
    uint64_t cmdline;
} __attribute__((packed));

struct stivale2_struct_tag_memmap {
    struct stivale2_tag tag;
    uint64_t entries;
    uint64_t memmap;
} __attribute__((packed));

enum {
    STIVALE2_MMAP_USABLE = 1,
    STIVALE2_MMAP_RESERVED = 2,
    STIVALE2_MMAP_ACPI_RECLAIMABLE = 3,
    STIVALE2_MMAP_ACPI_NVS = 4,
    STIVALE2_MMAP_BAD_MEMORY = 5,
    STIVALE2_MMAP_BOOTLOADER_RECLAIMABLE = 0x1000,
    STIVALE2_MMAP_KERNEL_AND_MODULES = 0x1001,
    STIVALE2_MMAP_FRAMEBUFFER = 0x1002,
};

struct stivale2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t unused;
} __attribute__((packed));

struct stivale2_struct_tag_framebuffer {
    struct stivale2_tag tag;
    uint64_t framebuffer_addr;
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_pitch;
    uint16_t framebuffer_bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused;
} __attribute__((packed));

struct stivale2_struct_tag_terminal {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t cols;
    uint64_t rows;
    uint64_t term_write;
    uint64_t max_length;
} __attribute__((packed));

struct stivale2_struct_tag_modules {
    struct stivale2_tag tag;
    uint64_t module_count;
    uint64_t modules;
} __attribute__((packed));

struct stivale2_module {
    uint64_t begin;
    uint64_t end;
    char string[128];
} __attribute__((packed));

struct stivale2_struct_tag_rsdp {
    struct stivale2_tag tag;
    uint64_t rsdp;
} __attribute__((packed));

struct stivale2_struct_tag_smbios {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t smbios_entry_32;
    uint64_t smbios_entry_64;
} __attribute__((packed));

struct stivale2_struct_tag_smp {
    struct stivale2_tag tag;
    uint64_t flags;
    uint32_t bsp_lapic_id;
    uint32_t unused;
    uint64_t cpu_count;
    uint64_t cpus;
} __attribute__((packed));

struct stivale2_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t target_stack;
    uint64_t goto_address;
    uint64_t extra_argument;
} __attribute__((packed));

#endif
```

- [ ] **Step 2: Create third_party/limine/BUILD.bazel**

```python
cc_library(
    name = "stivale2",
    hdrs = ["stivale2.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Commit**

```bash
git add third_party/
git commit -m "vendor: add Limine stivale2 protocol header"
```

---

### Task 4: Kernel logging (klog)

**Files:**
- Create: `kernel/lib/klog.hpp`
- Create: `kernel/lib/klog.cpp`
- Create: `kernel/lib/BUILD.bazel`

- [ ] **Step 1: Create kernel/lib/klog.hpp**

```cpp
#pragma once

#include <stddef.h>
#include <stdint.h>

void klog_init(struct stivale2_struct* info);
void klog_putc(char c);
void klog_write(const char* str, size_t len);
void klog(const char* str);
void klog_hex(uint64_t value);
```

- [ ] **Step 2: Create kernel/lib/klog.cpp**

```cpp
#include "kernel/lib/klog.hpp"
#include "stivale2.h"

namespace {

struct Framebuffer {
    uint8_t* addr;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t bpp;
} fb;

constexpr uint32_t GLYPH_WIDTH = 8;
constexpr uint32_t GLYPH_HEIGHT = 16;
uint32_t cursor_x = 0;
uint32_t cursor_y = 0;

constexpr uint8_t FONT[128][16] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x21] = {0x00,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x22] = {0x00,0x14,0x14,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x23] = {0x00,0x00,0x24,0x24,0x7E,0x24,0x24,0x7E,0x24,0x24,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x24] = {0x00,0x08,0x3E,0x49,0x48,0x3E,0x09,0x49,0x3E,0x08,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x25] = {0x00,0x61,0x92,0x64,0x08,0x10,0x26,0x49,0x86,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x26] = {0x00,0x1C,0x22,0x22,0x1C,0x29,0x46,0x42,0x3D,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x27] = {0x00,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x28] = {0x00,0x04,0x08,0x10,0x10,0x10,0x10,0x10,0x08,0x04,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x29] = {0x00,0x20,0x10,0x08,0x08,0x08,0x08,0x08,0x10,0x20,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x2A] = {0x00,0x00,0x08,0x49,0x2A,0x1C,0x2A,0x49,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x2B] = {0x00,0x00,0x08,0x08,0x08,0x7F,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x2C] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x08,0x08,0x10,0x00,0x00,0x00,0x00},
    [0x2D] = {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x2F] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x30] = {0x00,0x1C,0x22,0x26,0x2A,0x32,0x22,0x22,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x31] = {0x00,0x08,0x18,0x28,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x32] = {0x00,0x1C,0x22,0x02,0x04,0x08,0x10,0x20,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x33] = {0x00,0x3E,0x02,0x04,0x0C,0x02,0x02,0x22,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x34] = {0x00,0x04,0x0C,0x14,0x24,0x44,0x7E,0x04,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x35] = {0x00,0x7E,0x40,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x36] = {0x00,0x1C,0x20,0x40,0x5C,0x62,0x42,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x37] = {0x00,0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x38] = {0x00,0x3C,0x42,0x42,0x3C,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x39] = {0x00,0x3C,0x42,0x42,0x46,0x3A,0x02,0x04,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x3A] = {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x3B] = {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x08,0x08,0x10,0x00,0x00,0x00,0x00,0x00},
    [0x3C] = {0x00,0x02,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x02,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x3D] = {0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x3E] = {0x00,0x40,0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x40,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x3F] = {0x00,0x1C,0x22,0x02,0x04,0x08,0x08,0x00,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x40] = {0x00,0x1C,0x22,0x4A,0x56,0x52,0x4E,0x40,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x41] = {0x00,0x08,0x14,0x22,0x22,0x3E,0x22,0x22,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x42] = {0x00,0x7C,0x42,0x42,0x7C,0x42,0x42,0x42,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x43] = {0x00,0x1C,0x22,0x40,0x40,0x40,0x40,0x22,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x44] = {0x00,0x78,0x44,0x42,0x42,0x42,0x42,0x44,0x78,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x45] = {0x00,0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x46] = {0x00,0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x47] = {0x00,0x1C,0x22,0x40,0x40,0x4E,0x42,0x22,0x1E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x48] = {0x00,0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x49] = {0x00,0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4A] = {0x00,0x0F,0x04,0x04,0x04,0x04,0x04,0x44,0x38,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4B] = {0x00,0x42,0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4C] = {0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4D] = {0x00,0x82,0xC6,0xAA,0x92,0x82,0x82,0x82,0x82,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4E] = {0x00,0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x4F] = {0x00,0x1C,0x22,0x41,0x41,0x41,0x41,0x22,0x1C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x50] = {0x00,0x7C,0x42,0x42,0x42,0x7C,0x40,0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x51] = {0x00,0x1C,0x22,0x41,0x41,0x41,0x45,0x22,0x1D,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x52] = {0x00,0x7C,0x42,0x42,0x42,0x7C,0x48,0x44,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x53] = {0x00,0x3C,0x42,0x40,0x30,0x0C,0x02,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x54] = {0x00,0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x55] = {0x00,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x56] = {0x00,0x41,0x41,0x22,0x22,0x14,0x14,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x57] = {0x00,0x82,0x82,0x82,0x82,0x92,0xAA,0xC6,0x82,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x58] = {0x00,0x42,0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x59] = {0x00,0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5A] = {0x00,0x7E,0x02,0x04,0x08,0x10,0x20,0x40,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5B] = {0x00,0x3C,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x3C,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5C] = {0x00,0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5D] = {0x00,0x3C,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x3C,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5E] = {0x00,0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x60] = {0x00,0x10,0x08,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x61] = {0x00,0x00,0x00,0x3C,0x02,0x3E,0x42,0x46,0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x62] = {0x00,0x40,0x40,0x5C,0x62,0x42,0x42,0x62,0x5C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x63] = {0x00,0x00,0x00,0x3C,0x42,0x40,0x40,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x64] = {0x00,0x02,0x02,0x3A,0x46,0x42,0x42,0x46,0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x65] = {0x00,0x00,0x00,0x3C,0x42,0x7E,0x40,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x66] = {0x00,0x0C,0x12,0x10,0x7C,0x10,0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x67] = {0x00,0x00,0x00,0x3A,0x46,0x42,0x46,0x3A,0x02,0x42,0x3C,0x00,0x00,0x00,0x00,0x00},
    [0x68] = {0x00,0x40,0x40,0x5C,0x62,0x42,0x42,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x69] = {0x00,0x08,0x00,0x18,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6A] = {0x00,0x04,0x00,0x0C,0x04,0x04,0x04,0x04,0x44,0x38,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6B] = {0x00,0x40,0x40,0x44,0x48,0x50,0x60,0x50,0x48,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6C] = {0x00,0x18,0x08,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6D] = {0x00,0x00,0x00,0xEC,0x92,0x92,0x92,0x92,0x92,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6E] = {0x00,0x00,0x00,0x5C,0x62,0x42,0x42,0x42,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x6F] = {0x00,0x00,0x00,0x3C,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x70] = {0x00,0x00,0x00,0x5C,0x62,0x42,0x62,0x5C,0x40,0x40,0x40,0x00,0x00,0x00,0x00,0x00},
    [0x71] = {0x00,0x00,0x00,0x3A,0x46,0x42,0x46,0x3A,0x02,0x02,0x02,0x00,0x00,0x00,0x00,0x00},
    [0x72] = {0x00,0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x73] = {0x00,0x00,0x00,0x3C,0x42,0x30,0x0C,0x42,0x3C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x74] = {0x00,0x10,0x10,0x7C,0x10,0x10,0x10,0x12,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x75] = {0x00,0x00,0x00,0x42,0x42,0x42,0x42,0x46,0x3A,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x76] = {0x00,0x00,0x00,0x42,0x42,0x24,0x24,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x77] = {0x00,0x00,0x00,0x82,0x82,0x92,0xAA,0x44,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x78] = {0x00,0x00,0x00,0x42,0x24,0x18,0x18,0x24,0x42,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x79] = {0x00,0x00,0x00,0x42,0x42,0x24,0x24,0x18,0x18,0x10,0x60,0x00,0x00,0x00,0x00,0x00},
    [0x7A] = {0x00,0x00,0x00,0x7E,0x04,0x08,0x10,0x20,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7B] = {0x00,0x0C,0x10,0x10,0x10,0x60,0x10,0x10,0x10,0x0C,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7C] = {0x00,0x08,0x08,0x08,0x08,0x00,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7D] = {0x00,0x30,0x08,0x08,0x08,0x06,0x08,0x08,0x08,0x30,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7E] = {0x00,0x00,0x00,0x00,0x31,0x49,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [0x7F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

void draw_glyph(uint32_t cx, uint32_t cy, char c) {
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t* glyph = FONT[(unsigned char)c];
    for (uint32_t y = 0; y < GLYPH_HEIGHT; y++) {
        uint8_t row = glyph[y];
        for (uint32_t x = 0; x < GLYPH_WIDTH; x++) {
            uint32_t px = cx + x;
            uint32_t py = cy + y;
            if (px >= fb.width || py >= fb.height) continue;
            uint8_t* pixel = &fb.addr[py * fb.pitch + px * (fb.bpp / 8)];
            if (row & (1 << (7 - x))) {
                pixel[0] = 0xFF;
                pixel[1] = 0xFF;
                pixel[2] = 0xFF;
            } else {
                pixel[0] = 0x00;
                pixel[1] = 0x00;
                pixel[2] = 0x00;
            }
        }
    }
}

void scroll_screen() {
    uint32_t row_bytes = GLYPH_HEIGHT * fb.pitch;
    uint8_t* dst = fb.addr;
    uint8_t* src = fb.addr + row_bytes;
    uint32_t remaining = fb.height * fb.pitch - row_bytes;
    for (uint32_t i = 0; i < remaining; i++) {
        dst[i] = src[i];
    }
    for (uint32_t i = 0; i < row_bytes; i++) {
        dst[remaining + i] = 0;
    }
    cursor_y -= GLYPH_HEIGHT;
}

void newline() {
    cursor_x = 0;
    cursor_y += GLYPH_HEIGHT;
    if (cursor_y + GLYPH_HEIGHT > fb.height) {
        scroll_screen();
    }
}

} // namespace

void klog_init(stivale2_struct* info) {
    stivale2_struct_tag_framebuffer* fb_tag = nullptr;
    stivale2_tag* tag = (stivale2_tag*)info->tags;
    while (tag) {
        if (tag->identifier == STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID) {
            fb_tag = (stivale2_struct_tag_framebuffer*)tag;
            break;
        }
        tag = (stivale2_tag*)tag->next;
    }
    if (fb_tag) {
        fb.addr = (uint8_t*)fb_tag->framebuffer_addr;
        fb.width = fb_tag->framebuffer_width;
        fb.height = fb_tag->framebuffer_height;
        fb.pitch = fb_tag->framebuffer_pitch;
        fb.bpp = fb_tag->framebuffer_bpp;
    }
}

void klog_putc(char c) {
    if (fb.addr == nullptr) return;
    if (c == '\n') {
        newline();
    } else {
        draw_glyph(cursor_x, cursor_y, c);
        cursor_x += GLYPH_WIDTH;
        if (cursor_x + GLYPH_WIDTH > fb.width) {
            newline();
        }
    }
}

void klog_write(const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        klog_putc(str[i]);
    }
}

void klog(const char* str) {
    while (*str) {
        klog_putc(*str++);
    }
}

void klog_hex(uint64_t value) {
    const char hex[] = "0123456789ABCDEF";
    klog("0x");
    for (int i = 60; i >= 0; i -= 4) {
        klog_putc(hex[(value >> i) & 0xF]);
    }
}
```

- [ ] **Step 3: Create kernel/lib/BUILD.bazel**

```python
cc_library(
    name = "klib",
    srcs = [
        "klog.cpp",
        "panic.cpp",
    ],
    hdrs = [
        "klog.hpp",
        "panic.hpp",
    ],
    deps = [
        "//third_party/limine:stivale2",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Commit**

```bash
git add kernel/lib/
git commit -m "feat: add kernel logging library with framebuffer font rendering"
```

---

### Task 5: Panic handler

**Files:**
- Create: `kernel/lib/panic.hpp`
- Create: `kernel/lib/panic.cpp`

- [ ] **Step 1: Create kernel/lib/panic.hpp**

```cpp
#pragma once

[[noreturn]] void kpanic(const char* msg);
[[noreturn]] void kpanic(const char* msg, const char* file, int line);
#define KPANIC(msg) kpanic(msg, __FILE__, __LINE__)
```

- [ ] **Step 2: Create kernel/lib/panic.cpp**

```cpp
#include "kernel/lib/panic.hpp"
#include "kernel/lib/klog.hpp"

[[noreturn]] void kpanic(const char* msg) {
    klog("\n\n=== KERNEL PANIC ===\n");
    klog(msg);
    klog("\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}

[[noreturn]] void kpanic(const char* msg, const char* file, int line) {
    klog("\n\n=== KERNEL PANIC ===\n");
    klog("File: ");
    klog(file);
    klog("\nLine: ");
    klog_hex(line);
    klog("\nMessage: ");
    klog(msg);
    klog("\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}
```

- [ ] **Step 6: Commit**

```bash
git add kernel/lib/panic.hpp kernel/lib/panic.cpp
git commit -m "feat: add kernel panic handler"
```

---

### Task 6: Linker script

**Files:**
- Create: `kernel/arch/x86_64/link.ld`

- [ ] **Step 1: Create kernel/arch/x86_64/link.ld**

```ld
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

PHDRS
{
    headers PT_LOAD FILEHDR PHDRS;
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    /* Phase 1: identity-mapped at 2MB. Higher-half (0xFFFF'8000'0000'0000)
       will be set up in Phase 2 when paging is enabled. */
    . = 0x200000 + SIZEOF_HEADERS;

    .stivale2hdr : {
        KEEP(*(.stivale2hdr))
    } :headers

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
        *(.comment.*)
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add kernel/arch/x86_64/link.ld
git commit -m "feat: add kernel linker script (higher-half, stivale2 header)"
```

---

### Task 7: GDT setup

**Files:**
- Create: `kernel/arch/x86_64/gdt.hpp`
- Create: `kernel/arch/x86_64/gdt.cpp`

- [ ] **Step 1: Create kernel/arch/x86_64/gdt.hpp**

```cpp
#pragma once

#include <stdint.h>

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high : 4;
    uint8_t  flags : 4;
    uint8_t  base_high;
} __attribute__((packed));

struct GDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init();
```

- [ ] **Step 2: Create kernel/arch/x86_64/gdt.cpp**

```cpp
#include "kernel/arch/x86_64/gdt.hpp"

namespace {

constexpr int GDT_ENTRIES = 7;
GDTEntry gdt[GDT_ENTRIES];
GDTR gdtr;

void set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    gdt[idx].base_low = base & 0xFFFF;
    gdt[idx].base_mid = (base >> 16) & 0xFF;
    gdt[idx].base_high = (base >> 24) & 0xFF;
    gdt[idx].limit_low = limit & 0xFFFF;
    gdt[idx].limit_high = (limit >> 16) & 0xF;
    gdt[idx].access = access;
    gdt[idx].flags = flags;
}

} // namespace

void gdt_init() {
    // 0: null
    set_entry(0, 0, 0, 0, 0);
    // 1: kernel code (64-bit, ring 0)
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA);
    // 2: kernel data (ring 0)
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);
    // 3: user code (64-bit, ring 3) — placeholder for Phase 5
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xA);
    // 4: user data (ring 3) — placeholder for Phase 5
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);
    // 5: TSS low
    // 6: TSS high
    // TSS entries are 16 bytes total; set later when we have a TSS struct

    gdtr.limit = sizeof(GDTEntry) * GDT_ENTRIES - 1;
    gdtr.base = (uint64_t)&gdt[0];

    asm volatile(
        "lgdt (%0)\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "r"(&gdtr)
        : "rax", "memory"
    );
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/gdt.hpp kernel/arch/x86_64/gdt.cpp
git commit -m "feat: add GDT setup for 64-bit long mode"
```

---

### Task 8: IDT and exception handlers

**Files:**
- Create: `kernel/arch/x86_64/idt.hpp`
- Create: `kernel/arch/x86_64/idt.cpp`

- [ ] **Step 1: Create kernel/arch/x86_64/idt.hpp**

```cpp
#pragma once

void idt_init();
```

- [ ] **Step 2: Create kernel/arch/x86_64/idt.cpp**

```cpp
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"
#include <stdint.h>

struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

namespace {

constexpr int IDT_ENTRIES = 256;
IDTEntry idt[IDT_ENTRIES];
IDTR idtr;

struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

const char* exception_names[] = {
    "#DE Division Error",
    "#DB Debug",
    "#NMI Non-maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 Floating-Point Exception",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point Exception",
    "#VE Virtualization Exception",
    "#CP Control Protection Exception",
};

void set_entry(int idx, uint64_t handler, uint8_t ist, uint8_t type_attr) {
    idt[idx].offset_low = handler & 0xFFFF;
    idt[idx].offset_mid = (handler >> 16) & 0xFFFF;
    idt[idx].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[idx].selector = 0x08;
    idt[idx].ist = ist;
    idt[idx].type_attr = type_attr;
    idt[idx].reserved = 0;
}

extern "C" void exception_handler(InterruptFrame* frame) {
    klog("\n=== EXCEPTION ===\n");
    if (frame->int_no < 22) {
        klog(exception_names[frame->int_no]);
    } else {
        klog("Unknown Exception #");
        klog_hex(frame->int_no);
    }
    klog("\nError code: ");
    klog_hex(frame->err_code);
    klog("\nRIP: ");
    klog_hex(frame->rip);
    klog("\nRSP: ");
    klog_hex(frame->rsp);
    klog("\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}

#define EXCEPTION(n, ist) \
    extern "C" void isr_exc##n(); \
    asm( \
        ".globl isr_exc" #n "\n" \
        "isr_exc" #n ":\n" \
        "pushq $0\n" \
        "pushq $" #n "\n" \
        "jmp isr_common\n" \
    );

// Exceptions with error codes push the code themselves
#define EXCEPTION_ERR(n, ist) \
    extern "C" void isr_exc##n(); \
    asm( \
        ".globl isr_exc" #n "\n" \
        "isr_exc" #n ":\n" \
        "pushq $" #n "\n" \
        "jmp isr_common\n" \
    );

asm(R"(
.globl isr_common
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    callq exception_handler

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp   // skip int_no and err_code
    iretq
)");

// Exception 0-7: no error code
EXCEPTION(0, 0)
EXCEPTION(1, 0)
EXCEPTION(2, 0)
EXCEPTION(3, 0)
EXCEPTION(4, 0)
EXCEPTION(5, 0)
EXCEPTION(6, 0)
EXCEPTION(7, 0)
// Exception 8: error code
EXCEPTION_ERR(8, 0)
// Exception 9: no error code (though reserved)
EXCEPTION(9, 0)
// Exception 10-14: error code
EXCEPTION_ERR(10, 0)
EXCEPTION_ERR(11, 0)
EXCEPTION_ERR(12, 0)
EXCEPTION_ERR(13, 0)
EXCEPTION_ERR(14, 0)
// Exception 15-16: no error code
EXCEPTION(15, 0)
EXCEPTION(16, 0)
// Exception 17: error code
EXCEPTION_ERR(17, 0)
// Exception 18-20: no error code
EXCEPTION(18, 0)
EXCEPTION(19, 0)
EXCEPTION(20, 0)
// Exception 21: error code
EXCEPTION_ERR(21, 0)
// 22-31: reserved, no error code
EXCEPTION(22, 0)
EXCEPTION(23, 0)
EXCEPTION(24, 0)
EXCEPTION(25, 0)
EXCEPTION(26, 0)
EXCEPTION(27, 0)
EXCEPTION(28, 0)
EXCEPTION(29, 0)
EXCEPTION(30, 0)
EXCEPTION(31, 0)

} // namespace

void idt_init() {
    set_entry(0, (uint64_t)&isr_exc0, 0, 0x8E);
    set_entry(1, (uint64_t)&isr_exc1, 0, 0x8E);
    set_entry(2, (uint64_t)&isr_exc2, 0, 0x8E);
    set_entry(3, (uint64_t)&isr_exc3, 0, 0x8E);
    set_entry(4, (uint64_t)&isr_exc4, 0, 0x8E);
    set_entry(5, (uint64_t)&isr_exc5, 0, 0x8E);
    set_entry(6, (uint64_t)&isr_exc6, 0, 0x8E);
    set_entry(7, (uint64_t)&isr_exc7, 0, 0x8E);
    set_entry(8, (uint64_t)&isr_exc8, 0, 0x8E);
    set_entry(9, (uint64_t)&isr_exc9, 0, 0x8E);
    set_entry(10, (uint64_t)&isr_exc10, 0, 0x8E);
    set_entry(11, (uint64_t)&isr_exc11, 0, 0x8E);
    set_entry(12, (uint64_t)&isr_exc12, 0, 0x8E);
    set_entry(13, (uint64_t)&isr_exc13, 0, 0x8E);
    set_entry(14, (uint64_t)&isr_exc14, 0, 0x8E);
    set_entry(15, (uint64_t)&isr_exc15, 0, 0x8E);
    set_entry(16, (uint64_t)&isr_exc16, 0, 0x8E);
    set_entry(17, (uint64_t)&isr_exc17, 0, 0x8E);
    set_entry(18, (uint64_t)&isr_exc18, 0, 0x8E);
    set_entry(19, (uint64_t)&isr_exc19, 0, 0x8E);
    set_entry(20, (uint64_t)&isr_exc20, 0, 0x8E);
    set_entry(21, (uint64_t)&isr_exc21, 0, 0x8E);
    set_entry(22, (uint64_t)&isr_exc22, 0, 0x8E);
    set_entry(23, (uint64_t)&isr_exc23, 0, 0x8E);
    set_entry(24, (uint64_t)&isr_exc24, 0, 0x8E);
    set_entry(25, (uint64_t)&isr_exc25, 0, 0x8E);
    set_entry(26, (uint64_t)&isr_exc26, 0, 0x8E);
    set_entry(27, (uint64_t)&isr_exc27, 0, 0x8E);
    set_entry(28, (uint64_t)&isr_exc28, 0, 0x8E);
    set_entry(29, (uint64_t)&isr_exc29, 0, 0x8E);
    set_entry(30, (uint64_t)&isr_exc30, 0, 0x8E);
    set_entry(31, (uint64_t)&isr_exc31, 0, 0x8E);
    // Entries 32-255 stay zeroed (will be set up for IRQs in later phases)

    idtr.limit = sizeof(IDTEntry) * IDT_ENTRIES - 1;
    idtr.base = (uint64_t)&idt[0];

    asm volatile("lidt (%0)" : : "r"(&idtr) : "memory");
}
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/idt.hpp kernel/arch/x86_64/idt.cpp
git commit -m "feat: add IDT with exception handlers and register dumps"
```

---

### Task 9: Boot entry point

**Files:**
- Create: `kernel/arch/x86_64/boot.cpp`
- Create: `kernel/arch/x86_64/BUILD.bazel`

- [ ] **Step 1: Create kernel/arch/x86_64/boot.cpp**

```cpp
#include <stdint.h>
#include <stddef.h>
#include "stivale2.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

static uint8_t boot_stack[65536];

static stivale2_header_tag_framebuffer fb_request = {
    .tag = {.identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID, .next = 0},
    .framebuffer_width = 0,
    .framebuffer_height = 0,
    .framebuffer_bpp = 0,
};

__attribute__((section(".stivale2hdr"), used))
static stivale2_header header = {
    .entry_point = 0,
    .stack = (uint64_t)boot_stack + sizeof(boot_stack),
    .flags = 0,
    .tags = (uint64_t)&fb_request,
};

static stivale2_struct_tag_memmap* get_memmap(stivale2_struct* info) {
    stivale2_tag* tag = (stivale2_tag*)info->tags;
    while (tag) {
        if (tag->identifier == STIVALE2_STRUCT_TAG_MEMMAP_ID) {
            return (stivale2_struct_tag_memmap*)tag;
        }
        tag = (stivale2_tag*)tag->next;
    }
    return nullptr;
}

extern "C" void kernel_entry(stivale2_struct* info) {
    klog_init(info);

    klog("\n=== C++26 Kernel ===\n");
    klog("Bootloader: ");
    klog(info->bootloader_brand);
    klog(" ");
    klog(info->bootloader_version);
    klog("\n\n");

    klog("Initializing GDT...\n");
    gdt_init();
    klog("GDT initialized.\n");

    klog("Initializing IDT...\n");
    idt_init();
    klog("IDT initialized.\n");

    auto* memmap = get_memmap(info);
    if (memmap) {
        klog("Memory map entries: ");
        klog_hex(memmap->entries);
        klog("\n");
    }

    klog("\nKernel booted successfully.\n");

    while (1) {
        asm volatile("hlt");
    }
}
```

- [ ] **Step 2: Create kernel/arch/x86_64/BUILD.bazel**

```python
cc_library(
    name = "arch",
    srcs = [
        "boot.cpp",
        "gdt.cpp",
        "idt.cpp",
    ],
    hdrs = [
        "gdt.hpp",
        "idt.hpp",
    ],
    deps = [
        "//kernel/lib:klib",
        "//third_party/limine:stivale2",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "linker_script",
    srcs = ["link.ld"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add kernel entry point with Limine stivale2 protocol"
```

---

### Task 10: Kernel binary target

**Files:**
- Create: `kernel/BUILD.bazel`

- [ ] **Step 1: Create kernel/BUILD.bazel**

```python
cc_binary(
    name = "kernel",
    deps = [
        "//kernel/arch/x86_64:arch",
        "//kernel/lib:klib",
    ],
    srcs = [],
    additional_linker_inputs = ["//kernel/arch/x86_64:linker_script"],
    linkopts = ["-Wl,-T,$(location //kernel/arch/x86_64:linker_script)"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 2: Commit**

```bash
git add kernel/BUILD.bazel
git commit -m "build: add kernel ELF binary target"
```

---

### Task 11: QEMU run and debug scripts

**Files:**
- Create: `scripts/run.sh`
- Create: `scripts/debug.sh`

- [ ] **Step 1: Create scripts/run.sh**

```bash
#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

echo "==> Building kernel..."
bazel build //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
LIMINE_DIR="$ROOT_DIR/third_party/limine"
ISO_DIR="$ROOT_DIR/build/iso"
ISO_IMAGE="$ROOT_DIR/build/kernel.iso"

# Check for Limine binaries
LIMINE_BIN=""
if command -v limine &>/dev/null; then
    LIMINE_BIN="limine"
elif [ -f "$LIMINE_DIR/limine" ]; then
    LIMINE_BIN="$LIMINE_DIR/limine"
elif [ -f "$LIMINE_DIR/limine-bios.sys" ]; then
    LIMINE_BIN=""  # Will use xorriso method below
fi

# Create ISO directory structure
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot"

cp "$KERNEL_ELF" "$ISO_DIR/boot/kernel.elf"
cp "$ROOT_DIR/limine.cfg" "$ISO_DIR/boot/limine.cfg"

# Download Limine if needed
if [ ! -f "$LIMINE_DIR/limine-bios.sys" ]; then
    echo "==> Downloading Limine..."
    LIMINE_VERSION="8.x"
    if ! command -v limine &>/dev/null && [ ! -f "$LIMINE_DIR/limine" ]; then
        # Use git clone if Limine source is available, or download release
        if [ ! -d "$LIMINE_DIR/.git" ]; then
            echo "Limine not found. Please install Limine or place binaries in third_party/limine/"
            echo "  brew install limine"
            echo "  or download from: https://github.com/limine-bootloader/limine/releases"
            exit 1
        fi
    fi
fi

echo "==> Creating bootable ISO..."

# Try using limine command first
if [ -n "$LIMINE_BIN" ]; then
    "$LIMINE_BIN" bios "$ISO_DIR/boot/limine.cfg" "$ISO_DIR" "$ISO_IMAGE"
else
    # Fallback: use xorriso
    mkdir -p "$ISO_DIR/EFI/BOOT"
    cp "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/limine-bios-cd.bin" \
       "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/boot/"
    cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_DIR/EFI/BOOT/"
    cp "$LIMINE_DIR/BOOTIA32.EFI" "$ISO_DIR/EFI/BOOT/"

    xorriso -as mkisofs -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "$ISO_DIR" -o "$ISO_IMAGE"

    "$LIMINE_DIR/limine" bios "$ISO_IMAGE"
fi

echo "==> Starting QEMU..."
qemu-system-x86_64 \
    -cdrom "$ISO_IMAGE" \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -d cpu_reset
```

- [ ] **Step 2: Create scripts/debug.sh**

```bash
#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

echo "==> Building kernel (debug)..."
bazel build -c dbg //kernel:kernel

KERNEL_ELF="$ROOT_DIR/bazel-bin/kernel/kernel"
ISO_DIR="$ROOT_DIR/build/iso"
ISO_IMAGE="$ROOT_DIR/build/kernel.iso"

rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR/boot"
cp "$KERNEL_ELF" "$ISO_DIR/boot/kernel.elf"
cp "$ROOT_DIR/limine.cfg" "$ISO_DIR/boot/limine.cfg"

# Create ISO (same as run.sh, simplified)
mkdir -p "$ISO_DIR/EFI/BOOT"
LIMINE_DIR="$ROOT_DIR/third_party/limine"
if [ -f "$LIMINE_DIR/limine-bios.sys" ]; then
    cp "$LIMINE_DIR/limine-bios.sys" "$LIMINE_DIR/limine-bios-cd.bin" \
       "$LIMINE_DIR/limine-uefi-cd.bin" "$ISO_DIR/boot/"
    cp "$LIMINE_DIR/BOOTX64.EFI" "$ISO_DIR/EFI/BOOT/" 2>/dev/null || true

    xorriso -as mkisofs -b boot/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        "$ISO_DIR" -o "$ISO_IMAGE" 2>/dev/null

    "$LIMINE_DIR/limine" bios "$ISO_IMAGE" 2>/dev/null
elif command -v limine &>/dev/null; then
    limine bios "$ISO_DIR/boot/limine.cfg" "$ISO_DIR" "$ISO_IMAGE"
fi

echo "==> Starting QEMU with GDB stub (port 1234)..."
echo "    Connect with: lldb -o 'gdb-remote 1234' bazel-bin/kernel/kernel"
echo "    Or: gdb -ex 'target remote :1234' bazel-bin/kernel/kernel"

qemu-system-x86_64 \
    -cdrom "$ISO_IMAGE" \
    -m 512M \
    -serial stdio \
    -no-reboot \
    -s -S
```

- [ ] **Step 3: Make scripts executable**

Run:
```bash
chmod +x scripts/run.sh scripts/debug.sh
```

- [ ] **Step 4: Commit**

```bash
git add scripts/
git commit -m "feat: add QEMU run and debug scripts"
```

---

### Task 12: Boot integration test

**Files:**
- Create: `test/BUILD.bazel`

- [ ] **Step 1: Create test/BUILD.bazel**

```python
# Host-side unit tests will be added in Phase 2 (memory allocators)
# Phase 1 verification is manual: boot kernel in QEMU and observe output
```

- [ ] **Step 2: Build and boot the kernel**

Run:
```bash
./scripts/run.sh
```

Expected output:
```
=== C++26 Kernel ===
Bootloader: Limine <version>
Initializing GDT...
GDT initialized.
Initializing IDT...
IDT initialized.
Memory map entries: 0x<count>
Kernel booted successfully.
```

- [ ] **Step 3: Verify exception handling**

Temporarily add a null pointer dereference after IDT init to trigger #PF:
```cpp
*(volatile int*)0 = 0;
```

Run again. Expected: exception dump with `#PF Page Fault`, error code, RIP, RSP.

Then remove the deliberate fault.

- [ ] **Step 4: Commit**

```bash
git add test/
git commit -m "test: add placeholder test directory (Phase 1 verified manually)"
```

---

### Task 13: Fix any build issues on macOS

- [ ] **Step 1: Verify LLVM toolchain availability**

Run:
```bash
clang --version
ld.lld --version 2>/dev/null || echo "ld.lld not found — install with: brew install llvm"
```

- [ ] **Step 2: Fix toolchain if ld.lld path differs**

If `ld.lld` is not on PATH (common with Homebrew), update `toolchain/x86_64_elf.BUILD` to use the Homebrew prefix:

```python
tool_path(name = "ld", path = "/opt/homebrew/opt/llvm/bin/ld.lld"),
```

Or add to PATH in `.bazelrc`:
```
build --action_env=PATH=/opt/homebrew/opt/llvm/bin:/usr/bin:/bin
```

- [ ] **Step 3: Bazel build**

Run:
```bash
bazel build //kernel:kernel
```

Expected: build succeeds, produces `bazel-bin/kernel/kernel` ELF binary.

- [ ] **Step 4: Verify output is a valid ELF**

Run:
```bash
file bazel-bin/kernel/kernel
```

Expected: `ELF 64-bit LSB executable, x86-64, version 1 (SYSV)`

- [ ] **Step 5: Commit any toolchain path fixes**

```bash
git add toolchain/ .bazelrc
git commit -m "fix: adjust LLVM toolchain paths for macOS Homebrew"
```

---

## Phase 1 completion checklist

- [ ] Kernel ELF builds with `bazel build //kernel:kernel`
- [ ] Boots in QEMU via Limine
- [ ] Prints kernel banner, bootloader info
- [ ] GDT loads without triple fault
- [ ] IDT loads without triple fault
- [ ] Exception handlers catch #PF and dump register state
- [ ] Memory map enumeration works
- [ ] Kernel halts cleanly in idle loop
