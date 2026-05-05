# Fix Known Issues Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three deferred issues: paging_init CR3 reload triple-fault, buddy allocator wiring into kernel boot, and TSS I/O permission bitmap setup.

**Architecture:** Three independent fixes. Fix 1 copies Limine's full PML4 before CR3 reload. Fix 2 calls buddy_init and switches slab to buddy_alloc_pages. Fix 3 adds a static TSS with I/O bitmap and loads it via ltr. Each fix has its own commit.

**Tech Stack:** C++26 freestanding, x86-64, Limine boot protocol, Bazel 9

---

## File Structure

```
kernel/arch/x86_64/
├── paging.cpp          (MODIFIED) Fix 1: copy Limine PML4, safe CR3 reload
├── boot.cpp            (MODIFIED) Fix 2: call buddy_init + Fix 3: call tss_init
├── gdt.cpp             (MODIFIED) Fix 3: add tss_init()
├── gdt.hpp             (MODIFIED) Fix 3: declare tss_init()
kernel/core/mm/
├── slab.cpp            (MODIFIED) Fix 2: switch bitmap_alloc_page → buddy_alloc_pages
```

---

### Task 1: Fix paging_init CR3 reload triple-fault

**Files:**
- Modify: `kernel/arch/x86_64/paging.cpp`

- [ ] **Step 1: Read current paging.cpp**

Read the file to understand the current implementation. Note: `paging_init` builds a temporary PML4 with only kernel mapping, copies one entry back to Limine's PML4, but never reloads CR3. The triple-fault happens because the temporary PML4 is missing identity and HHDM mappings.

- [ ] **Step 2: Rewrite paging_init to copy Limine's full PML4, then CR3 reload**

Replace the existing `paging_init` function. The key changes:
1. Copy ALL Limine PML4 entries into the temporary PML4 (preserves identity + HHDM)
2. Build our kernel mapping into the temporary PML4 (overwriting just the kernel entry)
3. Reload CR3 with the temporary PML4 physical address
4. Remove the old in-place entry-swap approach

```cpp
void paging_init(
    uint64_t hhdm,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t kernel_size)
{
    g_hhdm = hhdm;

    // Get Limine's PML4
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    PageTable* pml4 = static_cast<PageTable*>(early_phys_to_virt(cr3));

    // Build new PML4: start by copying ALL Limine entries
    PageTable* tmp_pml4 = alloc_table();
    if (!tmp_pml4) {
        while (1) asm volatile("cli; hlt");
    }
    uint64_t* tmp_virt = static_cast<uint64_t*>(early_phys_to_virt(
        reinterpret_cast<uint64_t>(tmp_pml4)));
    for (int i = 0; i < 512; i++) {
        tmp_virt[i] = pml4->entries[i];
    }

    // Build kernel mapping, overwriting the kernel PML4 entry
    uint64_t kernel_pages_4k = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint16_t ki4 = pml4_index(kernel_virt_base);
    if (!map_4k_pages(tmp_pml4, kernel_virt_base, kernel_phys_base,
                      kernel_pages_4k, 0)) {
        while (1) asm volatile("cli; hlt");
    }

    // Reload CR3 — safe because identity and HHDM mappings are preserved
    uint64_t new_cr3 = reinterpret_cast<uint64_t>(tmp_pml4);
    asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");

    klog("Paging: CR3 reloaded, kernel at ");
    klog_hex(kernel_virt_base);
    klog(" mapped via own page tables\n");
}
```

Also add `#include "kernel/lib/klog.hpp"` at the top of paging.cpp if not already present.

- [ ] **Step 3: Update boot.cpp to call paging_init**

Read `kernel/arch/x86_64/boot.cpp`. Find the section after the bitmap demo (around line 188) that currently says:
```cpp
// 3. Higher-half paging takeover
// (Limine page tables are used directly — paging_init with CR3 reload
//  has issues that need further debugging)
klog("Using Limine page tables...\n");
```

Replace it with:
```cpp
// 3. Higher-half paging takeover
klog("Initializing paging...\n");
paging_init(hhdm, kernel_phys, kernel_virt, kernel_size);
```

- [ ] **Step 4: Build and verify**

```bash
cd /Users/pikachu/pr/kernel/.worktrees/fix-known-issues
bazel build //kernel:kernel
```

Expected: Build succeeds. If it fails, fix compilation errors (missing includes, etc.).

- [ ] **Step 5: Boot test**

```bash
bash scripts/run.sh > /tmp/qemu_paging.txt 2>&1 &
# wait 15s, kill QEMU, check output
grep "Paging" /tmp/qemu_paging.txt
```

Expected: "Paging: CR3 reloaded" appears in output. Kernel continues to boot through Phases 3-6 successfully.

NOTE: If QEMU is not available or Homebrew is locked, just verify the build succeeds. The key verification is `bazel build //kernel:kernel`.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/paging.cpp kernel/arch/x86_64/boot.cpp
git commit -m "fix: copy Limine PML4 mappings and enable safe CR3 reload"
```

---

### Task 2: Wire buddy allocator into kernel boot

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp`
- Modify: `kernel/core/mm/slab.cpp`

- [ ] **Step 1: Read current boot.cpp (paging/slab section) and slab.cpp**

Read both files to understand the current state. Note in boot.cpp: after paging_init, `slab_init(hhdm)` is called. Need to insert `buddy_init(hhdm, 0)` between them. Note in slab.cpp: `slab_create()` calls `bitmap_alloc_page()`.

- [ ] **Step 2: Add buddy_init call in boot.cpp**

In boot.cpp, after the paging_init call and before `slab_init`, add:

```cpp
// 4. Buddy allocator
klog("Initializing buddy allocator...\n");
buddy_init(hhdm, 0);
klog("  buddy ready (");
klog_hex(buddy_free_page_count());
klog(" free pages)\n\n");
```

Also add the include at the top of boot.cpp:
```cpp
#include "kernel/core/mm/buddy.hpp"
```

Update the comment before slab_init from:
```cpp
// 4. Slab allocator using bitmap directly for slab pages
// (buddy will be wired later when properly debugged)
```
to:
```cpp
// 5. Slab allocator backed by buddy
```

- [ ] **Step 3: Switch slab to use buddy in slab.cpp**

In `kernel/core/mm/slab.cpp`:

Replace the include:
```cpp
// Old:
#include "kernel/core/mm/bitmap_alloc.hpp"
// New:
#include "kernel/core/mm/buddy.hpp"
```

Replace `slab_create`'s page allocation:
```cpp
// Old:
void* page = bitmap_alloc_page();
// New:
void* page = buddy_alloc_pages(0);
```

- [ ] **Step 4: Build and verify**

```bash
bazel build //kernel:kernel
```

Expected: Build succeeds.

- [ ] **Step 5: Boot test**

```bash
bash scripts/run.sh > /tmp/qemu_buddy.txt 2>&1 &
# wait 15s, kill QEMU, check output
grep "buddy" /tmp/qemu_buddy.txt
```

Expected: "buddy ready" appears in output.

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp kernel/core/mm/slab.cpp
git commit -m "fix: wire buddy allocator into kernel boot sequence"
```

---

### Task 3: Add TSS with I/O permission bitmap

**Files:**
- Modify: `kernel/arch/x86_64/gdt.cpp`
- Modify: `kernel/arch/x86_64/gdt.hpp`
- Modify: `kernel/arch/x86_64/boot.cpp`

- [ ] **Step 1: Read current gdt.cpp, gdt.hpp, and boot.cpp**

Read all three files. Note: gdt.cpp has `set_entry()` as a static function. Need to either make it accessible or define tss_init in gdt.cpp (so it can call set_entry directly). The simpler approach: add `tss_init()` in gdt.cpp.

- [ ] **Step 2: Add TSS struct and tss_init to gdt.cpp**

Add at the top of `kernel/arch/x86_64/gdt.cpp` (after includes):

```cpp
#include <stddef.h>

struct TSS {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_bitmap_offset;
} __attribute__((packed));

static uint8_t g_tss_data[sizeof(TSS) + 8192 + 1]
    __attribute__((aligned(PAGE_SIZE)));
```

Add the `tss_init` function after `gdt_init()`:

```cpp
void tss_init() {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    for (size_t i = 0; i < sizeof(TSS); i++) {
        reinterpret_cast<uint8_t*>(tss)[i] = 0;
    }
    tss->io_bitmap_offset = sizeof(TSS);  // bitmap starts right after TSS

    // Set I/O bitmap to deny all port access from ring 3
    uint8_t* bitmap = &g_tss_data[sizeof(TSS)];
    for (int i = 0; i < 8192; i++) bitmap[i] = 0xFF;
    g_tss_data[sizeof(g_tss_data) - 1] = 0xFF;  // termination byte

    uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);
    uint64_t limit = sizeof(g_tss_data) - 1;

    set_entry(5, tss_addr & 0xFFFFFFFF, limit & 0xFFFFF, 0x89, 0);
    set_entry(6, (tss_addr >> 32) & 0xFFFFFFFF, 0, 0, 0);

    asm volatile(
        "ltr %%ax\n"
        :
        : "a"(0x28)   // GDT selector for entry 5 (5 * 8 = 0x28)
        : "memory"
    );

    klog("TSS: loaded at ");
    klog_hex(tss_addr);
    klog(" (I/O bitmap: all ports denied from ring 3)\n");
}
```

Add the include for klog at the top of gdt.cpp:
```cpp
#include "kernel/lib/klog.hpp"
```

- [ ] **Step 3: Declare tss_init in gdt.hpp**

Add to `kernel/arch/x86_64/gdt.hpp`:

```cpp
void tss_init();
```

- [ ] **Step 4: Call tss_init in boot.cpp**

In boot.cpp, after `gdt_init()` and its log message, add:

```cpp
klog("Initializing TSS...\n");
tss_init();
```

- [ ] **Step 5: Build and verify**

```bash
bazel build //kernel:kernel
```

Expected: Build succeeds. Fix any compilation errors (e.g., `PAGE_SIZE` might need `#include "kernel/arch/x86_64/paging.hpp"` in gdt.cpp).

- [ ] **Step 6: Boot test**

```bash
bash scripts/run.sh > /tmp/qemu_tss.txt 2>&1 &
# wait 15s, kill QEMU, check output
grep "TSS" /tmp/qemu_tss.txt
```

Expected: "TSS: loaded at ..." appears in output. Kernel continues to boot normally.

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/x86_64/gdt.cpp kernel/arch/x86_64/gdt.hpp kernel/arch/x86_64/boot.cpp
git commit -m "fix: add TSS with I/O permission bitmap"
```

---

### Task 4: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Remove fixed issues from known issues**

Read `CLAUDE.md`. Remove the three fixed issues from the "Known Issues" section:
- "paging_init: CR3 reload causes triple-fault"
- "buddy allocator: Implemented and host-tested, but not yet wired into kernel boot"
- "I/O ports: No I/O permission bitmap in TSS"

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: remove fixed known issues from CLAUDE.md"
```

---

## Test Verification

After all tasks complete:

```bash
bazel build //kernel:kernel
bash scripts/run.sh
```

Expected: kernel boots fully through all phases. Log shows:
```
Paging: CR3 reloaded, kernel at 0xFFFFFFFF80000000 mapped via own page tables
  buddy ready (0x... free pages)
TSS: loaded at 0x... (I/O bitmap: all ports denied from ring 3)
```
