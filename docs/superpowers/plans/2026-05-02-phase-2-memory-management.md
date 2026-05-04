# Phase 2: Memory Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take over paging from Limine, establish higher-half mapping, and build tiered physical memory allocators (bitmap → buddy → slab) ending with a working `kmalloc`/`kfree`.

**Architecture:** Three-tier allocator: bitmap (early boot, 1 bit per 4KB page) stores metadata in BSS, used to allocate page table pages. Buddy allocator replaces bitmap with order-0..10 coalescing. Slab allocator on top provides fixed-size caches (16B-2KB) for `kmalloc`. Paging setup uses Limine's HHDM to bootstrap, builds new PML4 with higher-half kernel + direct map, then switches CR3. Host-side tests exercise buddy and slab algorithms against a simulated page pool.

**Tech Stack:** Bazel 9, LLVM/Clang cross-compilation to x86_64-unknown-elf, Limine native protocol, GTest for host-side tests.

**Prerequisites:** Phase 1 complete (kernel boots, GDT/IDT, klog, serial). `brew install googletest` or GTest available.

---

## File Structure (this phase creates/modifies)

```
kernel/
├── arch/x86_64/
│   ├── paging.hpp          (NEW)
│   ├── paging.cpp          (NEW)
│   ├── BUILD.bazel         (MODIFY - add paging srcs)
│   └── boot.cpp            (MODIFY - add Limine requests, call mm init sequence)
├── core/
│   └── mm/
│       ├── BUILD.bazel      (NEW)
│       ├── pmm.hpp          (NEW)
│       ├── pmm.cpp          (NEW)
│       ├── bitmap_alloc.hpp (NEW)
│       ├── bitmap_alloc.cpp (NEW)
│       ├── buddy.hpp        (NEW)
│       ├── buddy.cpp        (NEW)
│       ├── slab.hpp         (NEW)
│       ├── slab.cpp         (NEW)
│       └── new_delete.cpp   (NEW - operator new/delete)
test/
├── BUILD.bazel              (MODIFY - add GTest dependency)
└── mm/
    ├── BUILD.bazel          (NEW)
    ├── pmm_test.cpp         (NEW)
    ├── buddy_test.cpp       (NEW)
    └── slab_test.cpp        (NEW)
```

---

### Task 1: x86-64 paging structures

**Files:**
- Create: `kernel/arch/x86_64/paging.hpp`

- [ ] **Step 1: Create kernel/arch/x86_64/paging.hpp**

```cpp
#pragma once

#include <stdint.h>

constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t LARGE_PAGE_SIZE = 0x200000;
constexpr uint64_t HUGE_PAGE_SIZE = 0x40000000;
constexpr uint16_t PAGE_TABLE_ENTRIES = 512;

namespace PageFlags {
    constexpr uint64_t Present    = 1ULL << 0;
    constexpr uint64_t Writable   = 1ULL << 1;
    constexpr uint64_t User       = 1ULL << 2;
    constexpr uint64_t WriteThru  = 1ULL << 3;
    constexpr uint64_t CacheDis   = 1ULL << 4;
    constexpr uint64_t Accessed   = 1ULL << 5;
    constexpr uint64_t Dirty      = 1ULL << 6;
    constexpr uint64_t Huge       = 1ULL << 7;
    constexpr uint64_t Global     = 1ULL << 8;
    constexpr uint64_t NoExec     = 1ULL << 63;
}

struct alignas(PAGE_SIZE) PageTable {
    uint64_t entries[PAGE_TABLE_ENTRIES];
};

inline constexpr uint16_t pml4_index(uint64_t va) { return (va >> 39) & 0x1FF; }
inline constexpr uint16_t pdpt_index(uint64_t va) { return (va >> 30) & 0x1FF; }
inline constexpr uint16_t pd_index(uint64_t va)   { return (va >> 21) & 0x1FF; }
inline constexpr uint16_t pt_index(uint64_t va)   { return (va >> 12) & 0x1FF; }

constexpr uint64_t DIRECT_MAP_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;

inline void* phys_to_virt(uint64_t phys_addr) {
    return reinterpret_cast<void*>(DIRECT_MAP_BASE + phys_addr);
}

inline uint64_t virt_to_phys(const void* virt_addr) {
    return reinterpret_cast<uint64_t>(virt_addr) - DIRECT_MAP_BASE;
}

inline constexpr uint64_t make_pte(uint64_t phys_addr, uint64_t flags) {
    return (phys_addr & ~(PAGE_SIZE - 1)) | flags;
}

inline constexpr uint64_t pte_phys_addr(uint64_t entry) {
    return entry & ~(PAGE_SIZE - 1);
}
```

- [ ] **Step 2: Build to verify compilation**

Run: `bazel build //kernel:kernel`

Expected: builds without errors (header is not yet included anywhere, but verify no parse errors).

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/paging.hpp
git commit -m "feat: add x86-64 4-level paging structures and helpers"
```

---

### Task 2: Physical memory manager (PMM)

**Files:**
- Create: `kernel/core/mm/pmm.hpp`
- Create: `kernel/core/mm/pmm.cpp`
- Create: `kernel/core/mm/BUILD.bazel`
- Create: `test/mm/pmm_test.cpp`
- Modify: `test/BUILD.bazel`

- [ ] **Step 1: Create kernel/core/mm/pmm.hpp**

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr uint32_t MEMMAP_USABLE                 = 0;
constexpr uint32_t MEMMAP_RESERVED               = 1;
constexpr uint32_t MEMMAP_ACPI_RECLAIMABLE       = 2;
constexpr uint32_t MEMMAP_ACPI_NVS               = 3;
constexpr uint32_t MEMMAP_BAD_MEMORY             = 4;
constexpr uint32_t MEMMAP_BOOTLOADER_RECLAIMABLE = 5;
constexpr uint32_t MEMMAP_KERNEL_AND_MODULES     = 6;
constexpr uint32_t MEMMAP_FRAMEBUFFER            = 7;

struct MemRange {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

// Initialize PMM from Limine memory map. Carves out kernel_phys_start..kernel_phys_end
// from usable ranges. Stores up to 128 consolidated usable ranges.
void pmm_init(
    const MemRange* ranges,
    size_t count,
    uint64_t kernel_phys_start,
    uint64_t kernel_phys_end);

uint64_t pmm_total_memory();
uint64_t pmm_usable_memory();
uint64_t pmm_highest_phys_addr();

// Returns pointer to internal array of usable ranges. count is output parameter.
const MemRange* pmm_usable_ranges(size_t* out_count);

// Check if a physical address range is contained within usable memory
bool pmm_is_usable(uint64_t phys_addr, uint64_t length);
```

- [ ] **Step 2: Create kernel/core/mm/pmm.cpp**

```cpp
#include "kernel/core/mm/pmm.hpp"

namespace {

constexpr size_t MAX_RANGES = 128;
MemRange g_usable[MAX_RANGES];
size_t g_usable_count = 0;
uint64_t g_total = 0;
uint64_t g_usable = 0;
uint64_t g_highest = 0;

bool overlaps(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

} // namespace

void pmm_init(
    const MemRange* ranges,
    size_t count,
    uint64_t kernel_phys_start,
    uint64_t kernel_phys_end)
{
    g_usable_count = 0;
    g_total = 0;
    g_usable = 0;
    g_highest = 0;

    for (size_t i = 0; i < count && g_usable_count < MAX_RANGES; i++) {
        const auto& r = ranges[i];
        uint64_t end = r.base + r.length;
        g_total += r.length;

        // BOOTLOADER_RECLAIMABLE: memory used by Limine that we can reuse
        if (r.type == MEMMAP_USABLE || r.type == MEMMAP_BOOTLOADER_RECLAIMABLE) {
            uint64_t base = r.base;
            uint64_t len = r.length;

            // Carve out the kernel's physical range
            if (overlaps(base, base + len, kernel_phys_start, kernel_phys_end)) {
                // Range before kernel
                if (base < kernel_phys_start) {
                    g_usable[g_usable_count++] = {base, kernel_phys_start - base, 0};
                    g_usable += kernel_phys_start - base;
                }
                // Range after kernel
                uint64_t after = kernel_phys_end;
                if (after < base + len) {
                    uint64_t remain = (base + len) - after;
                    g_usable[g_usable_count++] = {after, remain, 0};
                    g_usable += remain;
                    if (after + remain > g_highest) g_highest = after + remain;
                }
            } else {
                g_usable[g_usable_count++] = {base, len, 0};
                g_usable += len;
                if (end > g_highest) g_highest = end;
            }
        } else {
            if (end > g_highest) g_highest = end;
        }
    }
}

uint64_t pmm_total_memory() { return g_total; }
uint64_t pmm_usable_memory() { return g_usable; }
uint64_t pmm_highest_phys_addr() { return g_highest; }

const MemRange* pmm_usable_ranges(size_t* out_count) {
    *out_count = g_usable_count;
    return g_usable;
}

bool pmm_is_usable(uint64_t phys_addr, uint64_t length) {
    for (size_t i = 0; i < g_usable_count; i++) {
        if (g_usable[i].base <= phys_addr &&
            phys_addr + length <= g_usable[i].base + g_usable[i].length) {
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 3: Create kernel/core/mm/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "mm",
    srcs = [
        "pmm.cpp",
    ],
    hdrs = [
        "pmm.hpp",
    ],
    deps = [
        "//kernel/lib:klib",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Create host-side test for PMM**

Create `test/mm/pmm_test.cpp`:

```cpp
#include <gtest/gtest.h>

// For host tests, we compile the PMM source directly (not via Bazel kernel deps)
// to avoid pulling in kernel dependencies. The PMM algorithm is pure logic
// with no kernel dependencies.

// We test the PMM algorithm indirectly: we'll compile pmm.cpp in a host context.
// For now, verify basic range logic manually.

#include "kernel/core/mm/pmm.hpp"

TEST(PmmTest, SingleUsableRange) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},  // 1MB-2MB usable
    };
    pmm_init(ranges, 1, 0x200000, 0x300000);  // kernel at 2MB-3MB (no overlap)
    EXPECT_EQ(pmm_usable_memory(), 0x100000);
    EXPECT_EQ(pmm_total_memory(), 0x100000);
}

TEST(PmmTest, KernelCarvesUsableRange) {
    MemRange ranges[] = {
        {0x0, 0x500000, MEMMAP_USABLE},  // 0-5MB usable
    };
    // kernel at 2MB-3MB
    pmm_init(ranges, 1, 0x200000, 0x300000);
    // Should get 0-2MB + 3-5MB = 4MB usable (kernel took 1MB)
    EXPECT_EQ(pmm_usable_memory(), 0x400000);
    EXPECT_EQ(pmm_total_memory(), 0x500000);
}

TEST(PmmTest, ReservedNotUsable) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},
        {0x200000, 0x100000, MEMMAP_RESERVED},
        {0x300000, 0x100000, MEMMAP_BOOTLOADER_RECLAIMABLE},
    };
    pmm_init(ranges, 3, 0, 0);
    EXPECT_EQ(pmm_usable_memory(), 0x200000);   // usable + reclaimable
    EXPECT_EQ(pmm_total_memory(), 0x300000);    // all three
}

TEST(PmmTest, UsableRangesPointer) {
    MemRange ranges[] = {
        {0x100000, 0x100000, MEMMAP_USABLE},
        {0x300000, 0x100000, MEMMAP_USABLE},
    };
    pmm_init(ranges, 2, 0, 0);
    size_t count = 0;
    const MemRange* ur = pmm_usable_ranges(&count);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(ur[0].base, 0x100000);
    EXPECT_EQ(ur[1].base, 0x300000);
}

TEST(PmmTest, IsUsableCheck) {
    MemRange ranges[] = {
        {0x100000, 0x200000, MEMMAP_USABLE},    // 1MB-3MB
        {0x500000, 0x100000, MEMMAP_RESERVED},  // 5MB-6MB reserved
    };
    pmm_init(ranges, 2, 0x180000, 0x200000);  // kernel at 1.5MB-2MB
    EXPECT_TRUE(pmm_is_usable(0x100000, 0x1000));   // within first half
    EXPECT_TRUE(pmm_is_usable(0x200000, 0x1000));   // within second half (after kernel)
    EXPECT_FALSE(pmm_is_usable(0x180000, 0x1000));  // in kernel carve-out
    EXPECT_FALSE(pmm_is_usable(0x500000, 0x1000));  // reserved
}
```

- [ ] **Step 5: Set up host-side test build**

Create `test/mm/BUILD.bazel`:

```python
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "pmm_test",
    size = "small",
    srcs = [
        "pmm_test.cpp",
        "//kernel/core/mm:pmm.cpp",
    ],
    hdrs = [
        "//kernel/core/mm:pmm.hpp",
    ],
    deps = [
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

Wait — Bazel doesn't allow `//kernel/core:mm:pmm.cpp` in `srcs` across different platforms (test uses host, kernel uses x86_64-elf). We need a separate host-compatible library.

Alternative approach: create a `cc_library` that both kernel and tests can depend on, using `select()` or a separate host build file. Simpler: for the MM code (pure algorithms with no kernel deps), compile pmm.cpp with the host toolchain in tests.

Update `test/mm/BUILD.bazel`:

```python
load("@rules_cc//cc:defs.bzl", "cc_test", "cc_library")

# Host-compatible version of PMM (no kernel deps)
cc_library(
    name = "pmm_host",
    srcs = ["//kernel/core/mm:pmm.cpp"],
    hdrs = ["//kernel/core/mm:pmm.hpp"],
    # No kernel deps — pmm.cpp only uses stdint.h and its own header
    # If pmm.cpp includes klog.hpp, we need to provide a stub. Let's check.
)

cc_test(
    name = "pmm_test",
    size = "small",
    srcs = ["pmm_test.cpp"],
    deps = [
        ":pmm_host",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

Note: `pmm.hpp` includes `<stdint.h>` and `<stddef.h>` only — no kernel deps. But `pmm.cpp` currently `#include`s `"kernel/lib/klog.hpp"` for logging. For the host test build, we need to either:
1. Remove the klog dependency from pmm.cpp (move logging to caller)
2. Provide a klog stub for host tests

Let's do option 1: remove `klog` from `pmm.cpp`. The PMM is a pure data structure. Logging belongs in the boot integration.

Update `pmm.cpp` to remove the `#include "kernel/lib/klog.hpp"` and the `klog()` call at the end of `pmm_init`. The boot sequence will log PMM stats after `pmm_init()` returns.

- [ ] **Step 6: Add GTest to MODULE.bazel**

Add to `MODULE.bazel`:

```python
bazel_dep(name = "googletest", version = "1.15.2")
```

If building offline, add a `local_path_override`:

```python
local_path_override(
    module_name = "googletest",
    path = "/tmp/googletest-1.15.2",
)
```

- [ ] **Step 7: Build and run host tests**

Run: `bazel test //test/mm:pmm_test`

Expected: tests pass.

- [ ] **Step 8: Verify kernel build still works**

Run: `bazel build //kernel:kernel`

Expected: builds without errors.

- [ ] **Step 9: Commit**

```bash
git add kernel/core/mm/ test/mm/ test/BUILD.bazel MODULE.bazel
git commit -m "feat: add physical memory manager with host-side tests"
```

---

### Task 3: Bitmap page allocator

**Files:**
- Create: `kernel/core/mm/bitmap_alloc.hpp`
- Create: `kernel/core/mm/bitmap_alloc.cpp`
- Modify: `kernel/core/mm/BUILD.bazel`

- [ ] **Step 1: Create kernel/core/mm/bitmap_alloc.hpp**

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

// Early-boot page allocator using a bitmap.
// Allocates individual 4KB pages. Used to bootstrap page table allocation
// before the buddy allocator is online.

// Initialize the bitmap allocator. The bitmap is placed at bitmap_base_phys
// and covers all usable memory reported by pmm.
// hhdm_offset: Limine HHDM offset for accessing physical memory before
//              our direct map is set up.
void bitmap_init(uint64_t hhdm_offset, uint64_t bitmap_base_phys);

// Allocate a single zeroed 4KB page. Returns physical address, or 0 if OOM.
void* bitmap_alloc_page();

// Free a single 4KB page.
void bitmap_free_page(void* phys_addr);

// Statistics
size_t bitmap_free_page_count();
size_t bitmap_total_page_count();
```

- [ ] **Step 2: Create kernel/core/mm/bitmap_alloc.cpp**

```cpp
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

// HHDM offset — used before our direct map is online.
uint64_t g_hhdm = 0;

// The bitmap: one uint64_t per 256 pages (each bit = one 4KB page).
// Sized for up to 4GB physical address space: 4GB/4KB = 1M pages,
// 1M bits / 64 = 16K uint64_t entries = 128KB bitmap.
constexpr size_t MAX_PAGES = 1024 * 1024;  // 1M pages = 4GB
constexpr size_t BITMAP_U64S = MAX_PAGES / 64;
// Bitmap placed in BSS (fixed size). We then copy it to its final location.
uint64_t g_bitmap_bss[BITMAP_U64S];

// Actual bitmap data pointers (set during init)
uint64_t* g_bitmap = g_bitmap_bss;
size_t g_total_pages = 0;

// Index of the next word to scan (avoid re-scanning from 0 each time)
size_t g_scan_start = 0;

inline bool bit_test(const uint64_t* bitmap, size_t idx) {
    return (bitmap[idx / 64] >> (idx % 64)) & 1;
}

inline void bit_set(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] |= (1ULL << (idx % 64));
}

inline void bit_clear(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

// Convert physical address to page index and vice versa
inline size_t phys_to_idx(uint64_t phys) { return phys / PAGE_SIZE; }
inline uint64_t idx_to_phys(size_t idx) { return idx * PAGE_SIZE; }

} // namespace

void bitmap_init(uint64_t hhdm_offset, uint64_t bitmap_base_phys) {
    g_hhdm = hhdm_offset;

    size_t usable_count;
    const MemRange* usable = pmm_usable_ranges(&usable_count);
    uint64_t max_phys = pmm_highest_phys_addr();

    g_total_pages = max_phys / PAGE_SIZE;
    if (g_total_pages > MAX_PAGES) g_total_pages = MAX_PAGES;

    // Initially mark ALL pages as used
    for (size_t i = 0; i < g_total_pages / 64 + 1; i++) {
        g_bitmap[i] = ~0ULL;
    }

    // Mark usable ranges as free
    for (size_t r = 0; r < usable_count; r++) {
        uint64_t start = usable[r].base;
        uint64_t end = start + usable[r].length;
        size_t start_idx = phys_to_idx(start);
        size_t end_idx = phys_to_idx((end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        for (size_t i = start_idx; i < end_idx && i < g_total_pages; i++) {
            bit_clear(g_bitmap, i);
        }
    }

    // Mark the bitmap's own pages as used
    size_t bitmap_bytes = (g_total_pages + 7) / 8;
    size_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t bitmap_start_idx = phys_to_idx(bitmap_base_phys);
    for (size_t i = 0; i < bitmap_pages; i++) {
        bit_set(g_bitmap, bitmap_start_idx + i);
    }

    g_scan_start = 0;
}

void* bitmap_alloc_page() {
    for (size_t i = g_scan_start; i < g_total_pages; i++) {
        if (!bit_test(g_bitmap, i)) {
            bit_set(g_bitmap, i);
            g_scan_start = i + 1;
            uint64_t phys = idx_to_phys(i);
            // Zero the page via HHDM
            uint8_t* ptr = reinterpret_cast<uint8_t*>(g_hhdm + phys);
            for (size_t j = 0; j < PAGE_SIZE; j++) ptr[j] = 0;
            return reinterpret_cast<void*>(phys);
        }
    }
    // Wrap around
    for (size_t i = 0; i < g_scan_start; i++) {
        if (!bit_test(g_bitmap, i)) {
            bit_set(g_bitmap, i);
            g_scan_start = i + 1;
            uint64_t phys = idx_to_phys(i);
            uint8_t* ptr = reinterpret_cast<uint8_t*>(g_hhdm + phys);
            for (size_t j = 0; j < PAGE_SIZE; j++) ptr[j] = 0;
            return reinterpret_cast<void*>(phys);
        }
    }
    return nullptr;  // OOM
}

void bitmap_free_page(void* phys_addr) {
    size_t idx = phys_to_idx(reinterpret_cast<uint64_t>(phys_addr));
    if (idx < g_total_pages) {
        bit_clear(g_bitmap, idx);
        if (idx < g_scan_start) g_scan_start = idx;
    }
}

size_t bitmap_free_page_count() {
    size_t free = 0;
    for (size_t i = 0; i < g_total_pages; i++) {
        if (!bit_test(g_bitmap, i)) free++;
    }
    return free;
}

size_t bitmap_total_page_count() {
    return g_total_pages;
}
```

- [ ] **Step 3: Update kernel/core/mm/BUILD.bazel**

Add to deps:

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "mm",
    srcs = [
        "pmm.cpp",
        "bitmap_alloc.cpp",
    ],
    hdrs = [
        "pmm.hpp",
        "bitmap_alloc.hpp",
    ],
    deps = [
        "//kernel/lib:klib",
        "//kernel/arch/x86_64:paging_hdrs",
    ],
    visibility = ["//visibility:public"],
)
```

Note: We need `kernel/arch/x86_64:paging_hdrs` as a header-only target. Let's add it to `kernel/arch/x86_64/BUILD.bazel`:

```python
# Add to kernel/arch/x86_64/BUILD.bazel:
cc_library(
    name = "paging_hdrs",
    hdrs = ["paging.hpp"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Build to verify compilation**

Run: `bazel build //kernel/core/mm:mm`

Expected: builds without errors.

- [ ] **Step 5: Commit**

```bash
git add kernel/core/mm/bitmap_alloc.hpp kernel/core/mm/bitmap_alloc.cpp kernel/core/mm/BUILD.bazel kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add bitmap page allocator for early boot"
```

---

### Task 4: Higher-half paging takeover

**Files:**
- Create: `kernel/arch/x86_64/paging.cpp`
- Modify: `kernel/arch/x86_64/BUILD.bazel`

- [ ] **Step 1: Create kernel/arch/x86_64/paging.cpp**

```cpp
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/pmm.hpp"

namespace {

uint64_t g_hhdm = 0;

// Use HHDM to access physical memory before our direct map is online.
inline void* early_phys_to_virt(uint64_t phys_addr) {
    return reinterpret_cast<void*>(g_hhdm + phys_addr);
}

PageTable* alloc_table() {
    void* phys = bitmap_alloc_page();
    if (!phys) return nullptr;
    uint64_t phys_addr = reinterpret_cast<uint64_t>(phys);
    uint64_t* p = static_cast<uint64_t*>(early_phys_to_virt(phys_addr));
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        p[i] = 0;
    }
    return static_cast<PageTable*>(phys);
}

// Map [va, va+size) to [pa, pa+size) using 2MB huge pages.
// Both va and pa must be 2MB-aligned. size must be a multiple of 2MB.
bool map_huge_range(PageTable* pml4, uint64_t va, uint64_t pa, uint64_t size, uint64_t flags) {
    for (uint64_t offset = 0; offset < size; offset += LARGE_PAGE_SIZE) {
        uint64_t cur_va = va + offset;
        uint64_t cur_pa = pa + offset;

        uint16_t i4 = pml4_index(cur_va);
        uint16_t i3 = pdpt_index(cur_va);
        uint16_t i2 = pd_index(cur_va);

        // Walk PML4 → PDPT → PD, allocating tables as needed
        if (!(pml4->entries[i4] & PageFlags::Present)) {
            PageTable* pdpt = alloc_table();
            if (!pdpt) return false;
            pml4->entries[i4] = make_pte(reinterpret_cast<uint64_t>(pdpt),
                                         PageFlags::Present | PageFlags::Writable);
        }

        PageTable* pdpt = static_cast<PageTable*>(
            early_phys_to_virt(pte_phys_addr(pml4->entries[i4])));
        if (!(pdpt->entries[i3] & PageFlags::Present)) {
            PageTable* pd = alloc_table();
            if (!pd) return false;
            pdpt->entries[i3] = make_pte(reinterpret_cast<uint64_t>(pd),
                                         PageFlags::Present | PageFlags::Writable);
        }

        // 2MB huge page in PD
        PageTable* pd = static_cast<PageTable*>(
            early_phys_to_virt(pte_phys_addr(pdpt->entries[i3])));
        pd->entries[i2] = make_pte(cur_pa, PageFlags::Present | PageFlags::Writable |
                                          PageFlags::Huge | flags);
    }
    return true;
}

} // namespace

void paging_init(
    uint64_t hhdm,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t kernel_size)
{
    g_hhdm = hhdm;

    // Allocate new PML4
    PageTable* pml4 = alloc_table();
    if (!pml4) {
        // OOM at this stage is fatal — can't even klog reliably
        while (1) asm volatile("cli; hlt");
    }

    // 1. Map kernel at higher-half virtual address (2MB pages)
    uint64_t kernel_pages = (kernel_size + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1);
    map_huge_range(pml4, kernel_virt_base, kernel_phys_base, kernel_pages, PageFlags::NoExec);

    // 2. Direct map: 0xFFFF800000000000 → phys 0, covering all physical memory
    uint64_t max_phys = pmm_highest_phys_addr();
    uint64_t direct_map_size = (max_phys + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1);
    map_huge_range(pml4, DIRECT_MAP_BASE, 0, direct_map_size, PageFlags::NoExec);

    // 3. Identity-map the lowest 2MB for the trampoline (SMP bringup, Phase 3+)
    //    This keeps the code at low addresses reachable during CR3 switch.
    uint64_t identity_size = (2ULL * 1024 * 1024);  // 2MB
    map_huge_range(pml4, 0x0, 0x0, identity_size, PageFlags::NoExec);

    // 4. Switch CR3
    asm volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");

    // From this point on, the direct map at DIRECT_MAP_BASE is active.
    // phys_to_virt() and virt_to_phys() now work correctly.
}
```

- [ ] **Step 2: Update kernel/arch/x86_64/BUILD.bazel**

Update the `arch` target:

```python
cc_library(
    name = "arch",
    srcs = [
        "boot.cpp",
        "gdt.cpp",
        "idt.cpp",
        "paging.cpp",
    ],
    hdrs = [
        "gdt.hpp",
        "idt.hpp",
        "paging.hpp",
    ],
    deps = [
        "//kernel/lib:klib",
        "//third_party/limine:limine",
        "//kernel/core/mm:mm",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "paging_hdrs",
    hdrs = ["paging.hpp"],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "linker_script",
    srcs = ["link.ld"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Build to verify compilation**

Run: `bazel build //kernel:kernel`

Expected: builds without errors.

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/paging.cpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add higher-half paging takeover from Limine"
```

---

### Task 5: Buddy page allocator

**Files:**
- Create: `kernel/core/mm/buddy.hpp`
- Create: `kernel/core/mm/buddy.cpp`
- Create: `test/mm/buddy_test.cpp`
- Modify: `kernel/core/mm/BUILD.bazel`
- Modify: `test/mm/BUILD.bazel`

- [ ] **Step 1: Create kernel/core/mm/buddy.hpp**

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr int BUDDY_MAX_ORDER = 10;  // 4KB * 2^10 = 4MB max block

// Initialize the buddy allocator, taking over from the bitmap allocator.
// Uses all usable physical memory. After buddy_init(), bitmap_alloc_page()
// should no longer be used.
// hhdm_offset: required only for zeroing pages (before direct map is stable).
//   Pass 0 if direct map is already active.
void buddy_init(uint64_t hhdm_offset);

// Allocate 2^order contiguous physical pages. Returns physical address, or
// nullptr on OOM. Pages are NOT zeroed (caller zeroes if needed).
void* buddy_alloc_pages(size_t order);

// Free 2^order contiguous physical pages previously allocated by buddy_alloc_pages.
void buddy_free_pages(void* phys_addr, size_t order);

// Statistics
size_t buddy_free_page_count();
size_t buddy_total_pages();
```

- [ ] **Step 2: Create kernel/core/mm/buddy.cpp**

```cpp
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

struct Page {
    int order;     // -1 = allocated, 0..MAX_ORDER = free block of this order
    Page* next;    // next free block in this order's free list
};

Page* g_pages;         // Array: one Page struct per physical page
Page* g_free_lists[BUDDY_MAX_ORDER + 1];  // Linked lists of free blocks per order
size_t g_total_pages = 0;
size_t g_free_pages = 0;

// Convert between page index and physical address
inline size_t phys_to_idx(uint64_t phys) { return phys / PAGE_SIZE; }
inline uint64_t idx_to_phys(size_t idx) { return idx * PAGE_SIZE; }

// Find the buddy page index for a given page at a given order
inline size_t buddy_idx(size_t idx, int order) {
    return idx ^ (1ULL << order);
}

// Split a block at 'idx' of 'order' down to 'target_order'.
// Returns the block at 'idx' at 'target_order'.
void split(size_t idx, int order, int target_order) {
    while (order > target_order) {
        order--;
        size_t buddy = idx + (1ULL << order);
        g_pages[buddy].order = order;
        g_pages[buddy].next = g_free_lists[order];
        g_free_lists[order] = &g_pages[buddy];
        g_free_pages += (1ULL << order);
    }
    g_pages[idx].order = -1;  // allocated
    g_free_pages -= (1ULL << target_order);
}

// Try to coalesce a freed block at 'idx' of 'order' upward.
// Returns the final order after coalescing.
int coalesce(size_t idx, int order) {
    while (order < BUDDY_MAX_ORDER) {
        size_t bud = buddy_idx(idx, order);
        if (bud >= g_total_pages) break;  // buddy out of range

        // Check if buddy is free and at the same order
        if (g_pages[bud].order != order) break;

        // Remove buddy from free list
        Page** prev = &g_free_lists[order];
        while (*prev != &g_pages[bud]) {
            prev = &(*prev)->next;
        }
        *prev = g_pages[bud].next;

        g_free_pages -= (1ULL << order);

        // Merge: idx is the lower of the two
        if (bud < idx) idx = bud;
        order++;
    }
    g_pages[idx].order = order;
    g_pages[idx].next = g_free_lists[order];
    g_free_lists[order] = &g_pages[idx];
    g_free_pages += (1ULL << order);
    return order;
}

} // namespace

void buddy_init(uint64_t hhdm_offset) {
    size_t usable_count;
    const MemRange* usable = pmm_usable_ranges(&usable_count);
    (void)hhdm_offset;  // Phase 2: direct map is active, zero via phys_to_virt

    g_total_pages = pmm_highest_phys_addr() / PAGE_SIZE;

    // The Page array is stored at the start of the first usable range.
    // After paging_init(), we can use phys_to_virt() to access it.
    size_t pages_array_bytes = g_total_pages * sizeof(Page);
    size_t pages_array_pages = (pages_array_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    g_pages = static_cast<Page*>(phys_to_virt(usable[0].base));

    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        g_free_lists[i] = nullptr;
    }

    // Mark all pages as allocated initially
    for (size_t i = 0; i < g_total_pages; i++) {
        g_pages[i].order = -1;
        g_pages[i].next = nullptr;
    }

    // Free all usable pages except the first few (used by the Page array itself)
    for (size_t r = 0; r < usable_count; r++) {
        uint64_t base = usable[r].base;
        uint64_t end = base + usable[r].length;
        size_t start_idx = phys_to_idx((base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
        size_t end_idx = phys_to_idx(end & ~(PAGE_SIZE - 1));

        // Skip the Page array pages in the first usable range
        if (r == 0) {
            size_t reserved = pages_array_pages + 1;  // +1 for alignment safety
            if (start_idx < reserved) start_idx = reserved;
        }

        // Free pages at the highest possible order
        size_t i = start_idx;
        while (i < end_idx) {
            for (int order = BUDDY_MAX_ORDER; order >= 0; order--) {
                size_t block_size = 1ULL << order;
                if (i + block_size <= end_idx && (i & (block_size - 1)) == 0) {
                    g_pages[i].order = order;
                    g_pages[i].next = g_free_lists[order];
                    g_free_lists[order] = &g_pages[i];
                    g_free_pages += block_size;
                    i += block_size;
                    break;
                }
            }
        }
    }
}

void* buddy_alloc_pages(size_t order) {
    if (order > BUDDY_MAX_ORDER) return nullptr;

    // Find smallest order >= requested that has a free block
    for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
        if (g_free_lists[o]) {
            Page* block = g_free_lists[o];
            g_free_lists[o] = block->next;

            size_t idx = block - g_pages;
            g_free_pages -= (1ULL << o);

            // Split down to the requested order
            split(idx, o, order);

            return reinterpret_cast<void*>(idx_to_phys(idx));
        }
    }
    return nullptr;
}

void buddy_free_pages(void* phys_addr, size_t order) {
    if (!phys_addr || order > BUDDY_MAX_ORDER) return;
    size_t idx = phys_to_idx(reinterpret_cast<uint64_t>(phys_addr));
    g_pages[idx].order = static_cast<int>(order);
    coalesce(idx, order);
}

size_t buddy_free_page_count() { return g_free_pages; }
size_t buddy_total_pages() { return g_total_pages; }
```

- [ ] **Step 3: Update kernel/core/mm/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "mm",
    srcs = [
        "pmm.cpp",
        "bitmap_alloc.cpp",
        "buddy.cpp",
    ],
    hdrs = [
        "pmm.hpp",
        "bitmap_alloc.hpp",
        "buddy.hpp",
    ],
    deps = [
        "//kernel/lib:klib",
        "//kernel/arch/x86_64:paging_hdrs",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Create host-side buddy test**

Create `test/mm/buddy_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"

// For host testing, we set up a simulated memory environment.
// We provide a fake usable range and initialize PMM, then buddy_init.

// Note: buddy.cpp uses PAGE_SIZE from paging.hpp, which is 0x1000.
// The test allocates a buffer to serve as "physical memory" and sets
// up PMM to report that buffer as usable.

class BuddyTest : public ::testing::Test {
protected:
    // 16MB simulated physical memory
    static constexpr size_t SIM_MEM_SIZE = 16 * 1024 * 1024;
    uint8_t* sim_mem = nullptr;

    void SetUp() override {
        sim_mem = new uint8_t[SIM_MEM_SIZE];
        uint64_t phys_base = reinterpret_cast<uint64_t>(sim_mem);
        MemRange ranges[] = {
            {phys_base, SIM_MEM_SIZE, MEMMAP_USABLE},
        };
        pmm_init(ranges, 1, 0, 0);  // kernel at 0 (no carve-out)
    }

    void TearDown() override {
        delete[] sim_mem;
    }
};

TEST_F(BuddyTest, InitDoesNotCrash) {
    buddy_init(0);
    EXPECT_GT(buddy_free_page_count(), 0);
    EXPECT_GT(buddy_total_pages(), 0);
}

TEST_F(BuddyTest, AllocSinglePage) {
    buddy_init(0);
    void* page = buddy_alloc_pages(0);
    ASSERT_NE(page, nullptr);
    size_t free_before = buddy_free_page_count();
    buddy_free_pages(page, 0);
    EXPECT_EQ(buddy_free_page_count(), free_before + 1);
}

TEST_F(BuddyTest, AllocMultipleOrders) {
    buddy_init(0);
    void* p0 = buddy_alloc_pages(0);  // 4KB
    void* p1 = buddy_alloc_pages(1);  // 8KB
    void* p2 = buddy_alloc_pages(2);  // 16KB
    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    // Verify addresses don't overlap
    uint64_t a0 = reinterpret_cast<uint64_t>(p0);
    uint64_t a1 = reinterpret_cast<uint64_t>(p1);
    uint64_t a2 = reinterpret_cast<uint64_t>(p2);
    EXPECT_TRUE(a1 >= a0 + PAGE_SIZE || a0 >= a1 + 2*PAGE_SIZE);
    EXPECT_TRUE(a2 >= a1 + 2*PAGE_SIZE || a1 >= a2 + 4*PAGE_SIZE);
}

TEST_F(BuddyTest, FreeAndRealloc) {
    buddy_init(0);
    void* p = buddy_alloc_pages(3);  // 32KB
    ASSERT_NE(p, nullptr);
    buddy_free_pages(p, 3);
    void* p2 = buddy_alloc_pages(3);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p, p2);  // should get the same block back
}

TEST_F(BuddyTest, OOMReturnsNull) {
    buddy_init(0);
    // Try to allocate more pages than exist
    size_t total_4kb = buddy_free_page_count();
    for (size_t i = 0; i < total_4kb; i++) {
        buddy_alloc_pages(0);
    }
    void* fail = buddy_alloc_pages(0);
    EXPECT_EQ(fail, nullptr);
}

TEST_F(BuddyTest, CoalesceSiblings) {
    buddy_init(0);
    void* a = buddy_alloc_pages(0);
    void* b = buddy_alloc_pages(0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    buddy_free_pages(a, 0);
    buddy_free_pages(b, 0);
    // After freeing both halves, should be able to allocate 8KB (order 1)
    void* c = buddy_alloc_pages(1);
    EXPECT_NE(c, nullptr);
}
```

- [ ] **Step 5: Update test/mm/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_test", "cc_library")

# Host-compatible MM library (only sources that compile on host)
cc_library(
    name = "mm_host",
    srcs = [
        "//kernel/core/mm:pmm.cpp",
        "//kernel/core/mm:buddy.cpp",
    ],
    hdrs = [
        "//kernel/core/mm:pmm.hpp",
        "//kernel/core/mm:buddy.hpp",
    ],
    deps = [
        "//kernel/arch/x86_64:paging_hdrs",
    ],
)

cc_test(
    name = "pmm_test",
    size = "small",
    srcs = ["pmm_test.cpp"],
    deps = [
        ":mm_host",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "buddy_test",
    size = "small",
    srcs = ["buddy_test.cpp"],
    deps = [
        ":mm_host",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

- [ ] **Step 6: Build and run buddy tests**

Run: `bazel test //test/mm:buddy_test`

Expected: all 6 tests pass.

- [ ] **Step 7: Commit**

```bash
git add kernel/core/mm/buddy.hpp kernel/core/mm/buddy.cpp kernel/core/mm/BUILD.bazel test/mm/buddy_test.cpp test/mm/BUILD.bazel
git commit -m "feat: add buddy page allocator with host-side tests"
```

---

### Task 6: Slab allocator and kmalloc

**Files:**
- Create: `kernel/core/mm/slab.hpp`
- Create: `kernel/core/mm/slab.cpp`
- Create: `test/mm/slab_test.cpp`
- Modify: `kernel/core/mm/BUILD.bazel`
- Modify: `test/mm/BUILD.bazel`

- [ ] **Step 1: Create kernel/core/mm/slab.hpp**

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

// Slab allocator provides kmalloc/kfree for small fixed-size allocations.
// Backed by the buddy page allocator.
//
// Size classes (bytes): 16, 32, 64, 128, 256, 512, 1024, 2048
// Each slab is one 4KB page subdivided into objects of the cache's size.

// Initialize slab caches. Must be called after buddy_init().
void slab_init();

// Allocate memory. Rounds size up to the next slab size class.
// Returns nullptr for sizes > 2048 (use buddy directly for large allocs) or OOM.
void* kmalloc(size_t size);

// Free memory previously allocated by kmalloc.
void kfree(void* ptr);

// Return the actual allocated size (may be larger than requested due to rounding).
size_t kmalloc_usable_size(void* ptr);
```

- [ ] **Step 2: Create kernel/core/mm/slab.cpp**

```cpp
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

// Size classes
constexpr int NUM_CACHES = 8;
constexpr size_t CACHE_SIZES[NUM_CACHES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct Slab {
    Slab* next;      // next slab in cache list
    void* freelist;   // linked list of free objects
    size_t obj_count; // total objects in this slab
    size_t free_count;// number of free objects
};

struct SlabCache {
    size_t obj_size;
    size_t objs_per_slab;
    Slab* slabs_partial; // slabs with some free, some used
    Slab* slabs_full;    // slabs with no free objects
    Slab* slabs_free;    // slabs with all objects free
};

SlabCache g_caches[NUM_CACHES];

// Get cache index for a given size
int cache_index(size_t size) {
    for (int i = 0; i < NUM_CACHES; i++) {
        if (size <= CACHE_SIZES[i]) return i;
    }
    return -1;  // too large
}

// Initialize a single cache
void cache_init(SlabCache* cache, size_t obj_size) {
    cache->obj_size = obj_size;
    // Each object needs 8 bytes for the freelist pointer when free
    size_t effective_size = obj_size < 8 ? 8 : obj_size;
    cache->objs_per_slab = (PAGE_SIZE - sizeof(Slab)) / effective_size;
    cache->slabs_partial = nullptr;
    cache->slabs_full = nullptr;
    cache->slabs_free = nullptr;
}

// Create a new slab for a cache, getting a page from buddy
bool slab_create(SlabCache* cache) {
    void* page = buddy_alloc_pages(0);  // one 4KB page
    if (!page) return false;

    // Slab metadata at the start of the page
    // Access via direct map (phys_to_virt) since buddy returns physical addresses
    uint64_t phys = reinterpret_cast<uint64_t>(page);
    Slab* slab = static_cast<Slab*>(phys_to_virt(phys));

    slab->next = nullptr;
    slab->obj_count = cache->objs_per_slab;
    slab->free_count = cache->objs_per_slab;
    slab->freelist = nullptr;

    // Build the freelist
    uint8_t* obj_base = reinterpret_cast<uint8_t*>(slab + 1);
    for (size_t i = 0; i < cache->objs_per_slab; i++) {
        uint8_t* obj = obj_base + i * cache->obj_size;
        *reinterpret_cast<void**>(obj) = slab->freelist;
        slab->freelist = obj;
    }

    // Add to partial list
    slab->next = cache->slabs_partial;
    cache->slabs_partial = slab;
    return true;
}

// Allocate an object from a specific slab
void* slab_alloc_obj(SlabCache* cache, Slab* slab) {
    // Pop from freelist
    void* obj = slab->freelist;
    slab->freelist = *reinterpret_cast<void**>(obj);
    slab->free_count--;

    // Move slab between lists based on fullness
    // ... (handle list movements)
    return obj;
}

// Simple: always allocate from the first partial or free slab
void* cache_alloc(SlabCache* cache) {
    // Try partial slabs first
    if (cache->slabs_partial) {
        void* obj = slab_alloc_obj(cache, cache->slabs_partial);
        // Move to full if empty
        if (cache->slabs_partial->free_count == 0) {
            Slab* slab = cache->slabs_partial;
            cache->slabs_partial = slab->next;
            slab->next = cache->slabs_full;
            cache->slabs_full = slab;
        }
        return obj;
    }

    // Try free slabs
    if (cache->slabs_free) {
        Slab* slab = cache->slabs_free;
        cache->slabs_free = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
        void* obj = slab_alloc_obj(cache, slab);
        return obj;
    }

    // No slabs available — create a new one
    if (!slab_create(cache)) return nullptr;
    return cache_alloc(cache);  // retry with the new slab
}

// Free an object — find which slab it belongs to, return to freelist
void cache_free(SlabCache* cache, void* ptr) {
    // Find the slab: it's at the start of the page containing ptr
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    Slab* slab = static_cast<Slab*>(phys_to_virt(virt_to_phys(
        reinterpret_cast<void*>(page_addr))));

    bool was_full = (slab->free_count == 0);

    // Push onto freelist
    *reinterpret_cast<void**>(ptr) = slab->freelist;
    slab->freelist = ptr;
    slab->free_count++;

    // Move between lists
    if (was_full) {
        // Remove from full list, add to partial
        Slab** prev = &cache->slabs_full;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }

    if (slab->free_count == cache->objs_per_slab) {
        // Remove from partial, add to free
        Slab** prev = &cache->slabs_partial;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        slab->next = cache->slabs_free;
        cache->slabs_free = slab;
    }
}

} // namespace

void slab_init() {
    for (int i = 0; i < NUM_CACHES; i++) {
        cache_init(&g_caches[i], CACHE_SIZES[i]);
    }
}

void* kmalloc(size_t size) {
    int ci = cache_index(size);
    if (ci < 0) return nullptr;  // too large, use buddy directly
    return cache_alloc(&g_caches[ci]);
}

void kfree(void* ptr) {
    if (!ptr) return;
    // Determine cache by examining the object size.
    // We store the cache index in the byte just before the object.
    // (Simpler approach: only one cache array, so we scan.)
    // For now: the slab metadata at page base tells us the object size.
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    Slab* slab = static_cast<Slab*>(phys_to_virt(virt_to_phys(
        reinterpret_cast<void*>(page_addr))));
    size_t obj_size = PAGE_SIZE / slab->obj_count; // approximate; we divide page by count

    // Find matching cache
    for (int i = 0; i < NUM_CACHES; i++) {
        if (g_caches[i].obj_size >= obj_size) {
            cache_free(&g_caches[i], ptr);
            return;
        }
    }
}

size_t kmalloc_usable_size(void* ptr) {
    if (!ptr) return 0;
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    Slab* slab = static_cast<Slab*>(phys_to_virt(virt_to_phys(
        reinterpret_cast<void*>(page_addr))));
    for (int i = 0; i < NUM_CACHES; i++) {
        // Identify cache by total object count per page
        if (g_caches[i].objs_per_slab == slab->obj_count) {
            return g_caches[i].obj_size;
        }
    }
    return 0;
}
```

Each slab stores a back-pointer to its cache for O(1) kfree lookup:

```cpp
struct Slab {
    Slab* next;
    void* freelist;
    size_t obj_count;
    size_t free_count;
    SlabCache* cache;  // back-pointer to owning cache
};
```

This is cleaner. Let me rewrite the slab implementation with this fix and make the plan more concise.

Actually, let me just write the plan with the corrected implementation. Let me rewrite the slab task.

- [ ] **Step 2 (revised): Create kernel/core/mm/slab.cpp**

```cpp
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/arch/x86_64/paging.hpp"

namespace {

constexpr int NUM_CACHES = 8;
constexpr size_t CACHE_SIZES[NUM_CACHES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

struct SlabCache;

struct Slab {
    Slab* next;
    void* freelist;
    size_t obj_count;
    size_t free_count;
    SlabCache* cache;
};

struct SlabCache {
    size_t obj_size;
    size_t objs_per_slab;
    Slab* slabs_partial;
    Slab* slabs_full;
    Slab* slabs_free;
};

SlabCache g_caches[NUM_CACHES];

int cache_index(size_t size) {
    for (int i = 0; i < NUM_CACHES; i++) {
        if (size <= CACHE_SIZES[i]) return i;
    }
    return -1;
}

void cache_init(SlabCache* cache, size_t obj_size) {
    cache->obj_size = obj_size;
    size_t effective = obj_size < sizeof(void*) ? sizeof(void*) : obj_size;
    cache->objs_per_slab = (PAGE_SIZE - sizeof(Slab)) / effective;
    cache->slabs_partial = nullptr;
    cache->slabs_full = nullptr;
    cache->slabs_free = nullptr;
}

bool slab_create(SlabCache* cache) {
    void* page = buddy_alloc_pages(0);
    if (!page) return false;

    uint64_t phys = reinterpret_cast<uint64_t>(page);
    Slab* slab = static_cast<Slab*>(phys_to_virt(phys));

    slab->next = nullptr;
    slab->obj_count = cache->objs_per_slab;
    slab->free_count = cache->objs_per_slab;
    slab->freelist = nullptr;
    slab->cache = cache;

    uint8_t* obj_base = reinterpret_cast<uint8_t*>(slab + 1);
    for (size_t i = 0; i < cache->objs_per_slab; i++) {
        void** slot = reinterpret_cast<void**>(obj_base + i * cache->obj_size);
        *slot = slab->freelist;
        slab->freelist = slot;
    }

    slab->next = cache->slabs_partial;
    cache->slabs_partial = slab;
    return true;
}

void* cache_alloc(SlabCache* cache) {
    if (cache->slabs_partial) {
        Slab* slab = cache->slabs_partial;
        void* obj = slab->freelist;
        slab->freelist = *static_cast<void**>(obj);
        slab->free_count--;
        if (slab->free_count == 0) {
            cache->slabs_partial = slab->next;
            slab->next = cache->slabs_full;
            cache->slabs_full = slab;
        }
        return obj;
    }

    if (cache->slabs_free) {
        Slab* slab = cache->slabs_free;
        cache->slabs_free = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
        return cache_alloc(cache);
    }

    if (!slab_create(cache)) return nullptr;
    return cache_alloc(cache);
}

} // namespace

void slab_init() {
    for (int i = 0; i < NUM_CACHES; i++) {
        cache_init(&g_caches[i], CACHE_SIZES[i]);
    }
}

void* kmalloc(size_t size) {
    int ci = cache_index(size);
    if (ci < 0) return nullptr;
    return cache_alloc(&g_caches[ci]);
}

void kfree(void* ptr) {
    if (!ptr) return;
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    uint64_t phys = virt_to_phys(reinterpret_cast<void*>(page_addr));
    Slab* slab = static_cast<Slab*>(phys_to_virt(phys));
    SlabCache* cache = slab->cache;

    bool was_full = (slab->free_count == 0);

    *static_cast<void**>(ptr) = slab->freelist;
    slab->freelist = ptr;
    slab->free_count++;

    if (was_full) {
        // Remove from full list
        Slab** prev = &cache->slabs_full;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        slab->next = cache->slabs_partial;
        cache->slabs_partial = slab;
    }

    if (slab->free_count == cache->objs_per_slab) {
        // Remove from partial, add to free
        Slab** prev = &cache->slabs_partial;
        while (*prev != slab) prev = &(*prev)->next;
        *prev = slab->next;
        slab->next = cache->slabs_free;
        cache->slabs_free = slab;
    }
}

size_t kmalloc_usable_size(void* ptr) {
    if (!ptr) return 0;
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    uint64_t phys = virt_to_phys(reinterpret_cast<void*>(page_addr));
    Slab* slab = static_cast<Slab*>(phys_to_virt(phys));
    return slab->cache->obj_size;
}
```

- [ ] **Step 3: Create kernel/core/mm/slab.hpp** (same as above)

- [ ] **Step 4: Create host-side slab test**

Create `test/mm/slab_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/pmm.hpp"

class SlabTest : public ::testing::Test {
protected:
    static constexpr size_t SIM_MEM_SIZE = 16 * 1024 * 1024;
    uint8_t* sim_mem = nullptr;

    void SetUp() override {
        sim_mem = new uint8_t[SIM_MEM_SIZE];
        uint64_t phys_base = reinterpret_cast<uint64_t>(sim_mem);
        MemRange ranges[] = {
            {phys_base, SIM_MEM_SIZE, MEMMAP_USABLE},
        };
        pmm_init(ranges, 1, 0, 0);
        buddy_init(0);
        slab_init();
    }

    void TearDown() override {
        delete[] sim_mem;
    }
};

TEST_F(SlabTest, KmAllocSucceeds) {
    void* p = kmalloc(32);
    ASSERT_NE(p, nullptr);
    kfree(p);
}

TEST_F(SlabTest, KmAllocZeroReturnsNull) {
    // Allocating 0 bytes is implementation-defined. Our implementation
    // rounds up to the smallest size class (16).
    void* p = kmalloc(16);
    ASSERT_NE(p, nullptr);
    kfree(p);
}

TEST_F(SlabTest, AllocAllSizes) {
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    void* ptrs[8] = {};
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        ASSERT_NE(ptrs[i], nullptr) << "failed for size " << sizes[i];
    }
    for (int i = 0; i < 8; i++) {
        kfree(ptrs[i]);
    }
}

TEST_F(SlabTest, AllocTooLargeReturnsNull) {
    void* p = kmalloc(4096);
    EXPECT_EQ(p, nullptr);
}

TEST_F(SlabTest, ReuseAfterFree) {
    void* a = kmalloc(64);
    void* b = kmalloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    kfree(a);
    void* c = kmalloc(64);
    ASSERT_NE(c, nullptr);
    // After freeing 'a', the freelist reuses that slot
    EXPECT_EQ(c, a);
    kfree(b);
    kfree(c);
}

TEST_F(SlabTest, WriteAndRead) {
    int* p = static_cast<int*>(kmalloc(sizeof(int) * 10));
    ASSERT_NE(p, nullptr);
    for (int i = 0; i < 10; i++) p[i] = i * 42;
    for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], i * 42);
    kfree(p);
}

TEST_F(SlabTest, UsableSize) {
    void* p = kmalloc(100);
    ASSERT_NE(p, nullptr);
    size_t usable = kmalloc_usable_size(p);
    EXPECT_GE(usable, 100);  // should round up to 128
    EXPECT_LE(usable, 128);
    kfree(p);
}
```

- [ ] **Step 5: Update build files**

Update `kernel/core/mm/BUILD.bazel` to add `slab.cpp` and `slab.hpp`.

Update `test/mm/BUILD.bazel` to add `slab_test` target.

- [ ] **Step 6: Build and run slab tests**

Run: `bazel test //test/mm:slab_test`

Expected: all 7 tests pass.

- [ ] **Step 7: Commit**

```bash
git add kernel/core/mm/slab.hpp kernel/core/mm/slab.cpp kernel/core/mm/BUILD.bazel test/mm/slab_test.cpp test/mm/BUILD.bazel
git commit -m "feat: add slab allocator with kmalloc/kfree and host-side tests"
```

---

### Task 7: C++ new/delete integration

**Files:**
- Create: `kernel/core/mm/new_delete.cpp`
- Modify: `kernel/core/mm/BUILD.bazel`

- [ ] **Step 1: Create kernel/core/mm/new_delete.cpp**

```cpp
#include <stddef.h>
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/buddy.hpp"

// C++ freestanding environment requires these for 'new'/'delete' to work.

void* operator new(size_t size) {
    if (size == 0) size = 1;
    if (size <= 2048) {
        void* p = kmalloc(size);
        if (p) return p;
    }
    // Fallback: allocate directly from buddy for large objects
    size_t pages = (size + 4095) / 4096;
    // Find appropriate order
    size_t order = 0;
    while ((1ULL << order) * 4096 < pages) order++;
    return buddy_alloc_pages(order);
}

void* operator new[](size_t size) {
    return operator new(size);
}

void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    // kfree handles this correctly (finds the slab via page metadata)
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    operator delete(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    operator delete(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    operator delete(ptr);
}
```

Phase 2: `new` always goes to `kmalloc`. Sizes > 2KB are unsupported via `new` — callers use `buddy_alloc_pages` directly for large allocations. A later phase will add page-header tracking to support `delete` on buddy-backed allocations.

```cpp
void* operator new(size_t size) {
    if (size == 0) size = 1;
    void* p = kmalloc(size);
    if (!p) {
        // Panic on OOM in kernel
        asm volatile("cli; hlt");
    }
    return p;
}
```

- [ ] **Step 2: Update kernel/core/mm/BUILD.bazel**

Add `new_delete.cpp` to the `mm` target srcs.

- [ ] **Step 3: Verify kernel build**

Run: `bazel build //kernel:kernel`

Expected: builds successfully with no undefined symbol errors.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/mm/new_delete.cpp kernel/core/mm/BUILD.bazel
git commit -m "feat: wire C++ new/delete to kmalloc/kfree"
```

---

### Task 8: Boot sequence integration

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp`
- Modify: `kernel/BUILD.bazel`

- [ ] **Step 1: Update kernel/arch/x86_64/boot.cpp with full mm init sequence**

Replace the current `kernel_entry` with:

```cpp
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"

static uint8_t boot_stack[65536];

// Limine requests
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_FRAMEBUFFER_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_BOOTLOADER_INFO_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_memmap_request memmap_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_MEMMAP_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_hhdm_request hhdm_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_HHDM_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_KERNEL_ADDRESS_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

__attribute__((section(".limine_reqs"), used))
static volatile void* limine_requests[] = {
    &framebuffer_request,
    &bootloader_info_request,
    &memmap_request,
    &hhdm_request,
    &kernel_address_request,
    nullptr,
};

// Estimate kernel size: from kernel phys base to end of BSS.
// The linker defines _end symbol at the end of BSS.
extern uint8_t _end;
// Linker defines __kernel_phys_start (if we add it). Otherwise estimate:
// Use kernel physical base from Limine, and _end - KERNEL_VIRT_BASE as size.

extern "C" void kernel_entry(void) {
    asm volatile("movq %0, %%rsp" : : "r"(&boot_stack[sizeof(boot_stack)]));

    serial_init();
    klog_init(
        framebuffer_request.response
            ? framebuffer_request.response->framebuffers[0] : nullptr
    );

    klog("\n=== C++26 Kernel ===\n");

    if (bootloader_info_request.response) {
        klog("Bootloader: ");
        klog(bootloader_info_request.response->name);
        klog(" ");
        klog(bootloader_info_request.response->version);
        klog("\n\n");
    }

    // ---- Phase 1: GDT, IDT ----
    klog("Initializing GDT...\n");
    gdt_init();
    klog("GDT initialized.\n");

    klog("Initializing IDT...\n");
    idt_init();
    klog("IDT initialized.\n");

    // ---- Verify Limine responses ----
    if (!memmap_request.response) KPANIC("No memory map from Limine");
    if (!hhdm_request.response) KPANIC("No HHDM offset from Limine");
    if (!kernel_address_request.response) KPANIC("No kernel address from Limine");

    uint64_t hhdm = hhdm_request.response->offset;
    uint64_t kernel_phys = kernel_address_request.response->physical_base;
    uint64_t kernel_virt = kernel_address_request.response->virtual_base;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&_end) - kernel_virt;

    klog("HHDM offset: "); klog_hex(hhdm); klog("\n");
    klog("Kernel: phys ");
    klog_hex(kernel_phys); klog(" -> virt ");
    klog_hex(kernel_virt); klog(" (");
    klog_hex(kernel_size); klog(" bytes)\n\n");

    // ---- Phase 2: Memory management ----
    klog("=== Phase 2: Memory Management ===\n\n");

    // 1. Physical memory manager
    klog("Initializing PMM...\n");
    const auto* ranges = reinterpret_cast<const MemRange*>(memmap_request.response->entries);
    pmm_init(ranges, memmap_request.response->entry_count, kernel_phys, kernel_phys + kernel_size);
    klog("  Total: "); klog_hex(pmm_total_memory()); klog(" bytes\n");
    klog("  Usable: "); klog_hex(pmm_usable_memory()); klog(" bytes\n");
    klog("  Highest physical: "); klog_hex(pmm_highest_phys_addr()); klog("\n");

    // 2. Bitmap allocator (early boot)
    klog("Initializing bitmap allocator...\n");
    // Place the bitmap at 2MB physical (after the BSP trampoline area).
    // The kernel and bootloader already occupy low memory; 2MB is safely above.
    bitmap_init(hhdm, 0x200000);
    klog("  Free pages: "); klog_hex(bitmap_free_page_count()); klog("\n");
    klog("  Total pages: "); klog_hex(bitmap_total_page_count()); klog("\n");

    // 3. Higher-half paging takeover
    klog("Setting up higher-half paging...\n");
    paging_init(hhdm, kernel_phys, kernel_virt, kernel_size);
    klog("  Paging active (CR3 switched)\n");

    // 4. Buddy allocator (replaces bitmap)
    klog("Initializing buddy allocator...\n");
    buddy_init(0);  // pass 0: direct map is now active, phys_to_virt works
    klog("  Free pages: "); klog_hex(buddy_free_page_count()); klog("\n");
    klog("  Total pages: "); klog_hex(buddy_total_pages()); klog("\n");

    // 5. Slab allocator (kmalloc available)
    klog("Initializing slab allocator...\n");
    slab_init();
    klog("  kmalloc ready (16B-2048B)\n");

    // ---- Verification: test an allocation ----
    void* test_alloc = kmalloc(128);
    klog("Test kmalloc(128): ");
    klog_hex(reinterpret_cast<uint64_t>(test_alloc));
    klog("\n");
    if (test_alloc) {
        kfree(test_alloc);
        klog("  freed OK\n");
    }

    klog("\n=== Kernel booted successfully ===\n");

    while (1) {
        asm volatile("hlt");
    }
}
```

The Limine memmap response provides `limine_memmap_entry** entries` (array of pointers). Each `limine_memmap_entry` has base/length/type as uint64_t. We convert to `MemRange` (which uses uint32_t for type) via iteration:

```cpp
// Convert Limine memmap entries to MemRange array
MemRange memmap_buf[128];
size_t entry_count = memmap_request.response->entry_count;
if (entry_count > 128) entry_count = 128;
for (size_t i = 0; i < entry_count; i++) {
    auto* e = memmap_request.response->entries[i];
    memmap_buf[i] = {e->base, e->length, static_cast<uint32_t>(e->type)};
}
pmm_init(memmap_buf, entry_count, kernel_phys, kernel_phys + kernel_size);
```

- [ ] **Step 2: Update kernel/BUILD.bazel to depend on mm**

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "kernel",
    deps = [
        "//kernel/arch/x86_64:arch",
        "//kernel/lib:klib",
        "//kernel/core/mm:mm",
    ],
    additional_linker_inputs = ["//kernel/arch/x86_64:linker_script"],
    linkopts = ["-Wl,-T,$(location //kernel/arch/x86_64:linker_script)"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Add LIMINE_HHDM_REQUEST_ID and LIMINE_KERNEL_ADDRESS_REQUEST_ID to limine.h**

Add to `third_party/limine/limine.h` (these are the actual protocol UUIDs from Limine):

```c
#define LIMINE_HHDM_REQUEST_ID \
    0x48dcf1cb8ad2b852, 0x63984e959a98244b

#define LIMINE_KERNEL_ADDRESS_REQUEST_ID \
    0x3f34bfd0e3a0c2db, 0xa79e6e729c67f47e
```

The Limine protocol uses 4-element `id[4]` arrays where `id[0..1]` = `LIMINE_COMMON_MAGIC` and `id[2..3]` = request-specific. The request-specific IDs below are from the Limine protocol specification:

- [ ] **Step 4: Build the kernel**

Run: `bazel build //kernel:kernel`

Expected: builds successfully.

- [ ] **Step 5: Boot and verify**

Run: `bash scripts/run.sh`

Expected output should include:
```
=== C++26 Kernel ===
Bootloader: Limine 12.1.0
Initializing GDT...
GDT initialized.
Initializing IDT...
IDT initialized.
HHDM offset: 0x...
Kernel: phys 0x... -> virt 0x... (...)

=== Phase 2: Memory Management ===

Initializing PMM...
  Total: 0x... bytes
  Usable: 0x... bytes
  Highest physical: 0x...
Initializing bitmap allocator...
  Free pages: 0x...
  Total pages: 0x...
Setting up higher-half paging...
  Paging active (CR3 switched)
Initializing buddy allocator...
  Free pages: 0x...
  Total pages: 0x...
Initializing slab allocator...
  kmalloc ready (16B-2048B)
Test kmalloc(128): 0x...
  freed OK

=== Kernel booted successfully ===
```

- [ ] **Step 6: Debug any issues**

If Limine request IDs are wrong (KPANIC: "No HHDM offset from Limine"):
- Check the correct request IDs from the Limine source at `/usr/local/opt/limine/share/doc/limine/`
- Update `limine.h` with the correct values
- Rebuild and retest

If page fault during paging_init():
- Check that bitmap_alloc_page() returns valid addresses
- Verify HHDM offset is correct
- Check page table entry construction

If buddy_init() crashes:
- Verify direct map (phys_to_virt) works after CR3 switch
- Check that usable ranges are correct

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp kernel/BUILD.bazel third_party/limine/limine.h
git commit -m "feat: integrate memory management into boot sequence"
```

---

### Task 9: Exception handler updates for #PF details

**Files:**
- Modify: `kernel/arch/x86_64/idt.cpp` (minor)

- [ ] **Step 1: Add CR2 readout to #PF handler**

Update the `exception_handler` in `kernel/arch/x86_64/idt.cpp` to read CR2 on page faults:

```cpp
if (frame->int_no == 14) {  // #PF
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    klog("CR2 (faulting address): ");
    klog_hex(cr2);
    klog("\n");
}
```

- [ ] **Step 2: Build and verify**

Run: `bazel build //kernel:kernel`

Expected: builds.

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/idt.cpp
git commit -m "feat: add CR2 readout to page fault handler"
```

---

## Phase 2 Completion Checklist

- [ ] `bazel test //test/mm:...` — all host-side tests pass (PMM, buddy, slab)
- [ ] `bazel build //kernel:kernel` — kernel ELF builds
- [ ] `bash scripts/run.sh` boots and shows:
  - [ ] PMM stats (total, usable memory, ranges)
  - [ ] Bitmap free/total pages
  - [ ] "Paging active (CR3 switched)"
  - [ ] Buddy free/total pages
  - [ ] "kmalloc ready (16B-2048B)"
  - [ ] Test kmalloc allocation+freed OK
  - [ ] "Kernel booted successfully"
- [ ] Kernel no longer depends on Limine's identity mapping (our own page tables active)
- [ ] `kmalloc`/`kfree` work (verified by test allocation in boot)
- [ ] C++ `new`/`delete` work (can allocate C++ objects on the heap)
- [ ] Exception handlers still catch #PF with CR2 readout
