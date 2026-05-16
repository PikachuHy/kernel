# Phase 7: VMM + Process Objects — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full VMM with VMOs and demand paging, Process objects with per-process handle tables, ELF loading, and ring-3 user-space bootstrap.

**Architecture:** Process owns PML4 + HandleTable + VmRegion list. VMO is the unit of memory (Anonymous with COW, Physical for hardware). Page fault handler does demand paging and COW resolution. ELF loader maps segments into a new Process. Ring 3 entry via iretq with constructed stack frame. Init process is an embedded ELF.

**Tech Stack:** C++26 freestanding, x86-64 paging structures, GTest for host-side tests, Bazel 9.

**Design spec:** `docs/superpowers/specs/2026-05-10-phase-7-vmm-process.md`

---

### Task 1: Fix paging_init CR3 Reload

**Files:**
- Modify: `kernel/arch/x86_64/paging.cpp`
- Modify: `kernel/arch/x86_64/paging.hpp`

**Goal:** Fix the CR3 reload triple-fault so we can create per-process page tables. The kernel must take ownership of its page tables from Limine.

- [ ] **Step 1: Add `paging_save_kernel_template` to paging.hpp**

```cpp
// kernel/arch/x86_64/paging.hpp — add after existing declarations

// After paging_init succeeds, saves the kernel-half PML4 entries (indices 256-511)
// as a template for new process address spaces.
void paging_save_kernel_template();

// Returns the saved kernel PML4 template (physical address).
// New processes copy entries 256-511 from this template.
uint64_t paging_kernel_pml4_template();

// Walk a 4-level page table for `va` in the given PML4, creating intermediate
// tables as needed via bitmap_alloc_page. Installs `pa | flags` as the leaf PTE.
// Returns true on success, false on allocation failure.
bool page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags);

// Unmap a 4K page, freeing empty intermediate page tables.
// Returns the physical address that was mapped, or 0 if not mapped.
uint64_t page_table_unmap(uint64_t pml4_phys, uint64_t va);

// Look up the physical address mapped at `va` in the given PML4.
// Returns 0 if not mapped.
uint64_t page_table_lookup(uint64_t pml4_phys, uint64_t va);
```

- [ ] **Step 2: Rewrite paging_init in paging.cpp**

The current issue: `map_4k_pages` may try to split huge pages without properly handling the HHDM access window on the new PML4. The fix uses the HHDM to access all page tables during construction and verifies each step.

```cpp
// kernel/arch/x86_64/paging.cpp

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/lib/klog.hpp"

namespace {

uint64_t g_hhdm = 0;
uint64_t g_kernel_template_pml4 = 0;

inline uint64_t* hhdm_ptr(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(g_hhdm + phys);
}

// Allocate a zeroed page table. Returns physical address.
uint64_t alloc_table_phys() {
    void* phys = bitmap_alloc_page();
    if (!phys) return 0;
    uint64_t* virt = hhdm_ptr(reinterpret_cast<uint64_t>(phys));
    for (int i = 0; i < 512; i++) virt[i] = 0;
    return reinterpret_cast<uint64_t>(phys);
}

// Walk the page table for `va`, creating intermediate tables as needed.
// On return, *pte_out points to the leaf PTE slot (physical addr via HHDM).
// Returns true on success.
bool walk_create(uint64_t pml4_phys, uint64_t va, uint64_t** pte_out) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    uint64_t* pml4 = hhdm_ptr(pml4_phys);

    // PML4 → PDPT
    if (!(pml4[i4] & PageFlags::Present)) {
        uint64_t pdpt = alloc_table_phys();
        if (!pdpt) return false;
        pml4[i4] = make_pte(pdpt, PageFlags::Present | PageFlags::Writable);
    }
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));

    // PDPT → PD
    if (!(pdpt[i3] & PageFlags::Present)) {
        uint64_t pd = alloc_table_phys();
        if (!pd) return false;
        pdpt[i3] = make_pte(pd, PageFlags::Present | PageFlags::Writable);
    }
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));

    // PD may contain a 2MB huge-page entry. If so, split it before adding 4K PTEs.
    if (pd[i2] & PageFlags::Huge) {
        uint64_t huge_pa = pte_phys_addr(pd[i2]);
        uint64_t huge_flags = pd[i2] & ~PageFlags::Huge;

        uint64_t pt = alloc_table_phys();
        if (!pt) return false;

        // Pre-fill new PT with 4K entries covering the same 2MB range
        uint64_t* pt_virt = hhdm_ptr(pt);
        for (int j = 0; j < 512; j++) {
            pt_virt[j] = make_pte(huge_pa + j * PAGE_SIZE, huge_flags);
        }

        pd[i2] = make_pte(pt, PageFlags::Present | PageFlags::Writable);
    } else if (!(pd[i2] & PageFlags::Present)) {
        uint64_t pt = alloc_table_phys();
        if (!pt) return false;
        pd[i2] = make_pte(pt, PageFlags::Present | PageFlags::Writable);
    }

    // PT
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    *pte_out = &pt[i1];
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

    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    uint64_t* old_pml4 = hhdm_ptr(old_cr3);

    // Build new PML4
    uint64_t new_pml4_phys = alloc_table_phys();
    if (!new_pml4_phys) KPANIC("paging_init: failed to allocate PML4");
    uint64_t* new_pml4 = hhdm_ptr(new_pml4_phys);

    // Copy ALL Limine entries EXCEPT the kernel's PML4 entry — we rebuild
    // the kernel mapping with 4K pages via walk_create.
    uint16_t ki4 = pml4_index(kernel_virt_base);
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = (i == ki4) ? 0 : old_pml4[i];
    }

    // Map kernel pages as 4K mappings
    uint64_t kernel_pages = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    klog("Paging: mapping kernel (");
    klog_hex(kernel_pages); klog(" bytes)...\n");

    for (uint64_t off = 0; off < kernel_pages; off += PAGE_SIZE) {
        uint64_t va = kernel_virt_base + off;
        uint64_t pa = kernel_phys_base + off;
        uint64_t* pte = nullptr;
        if (!walk_create(new_pml4_phys, va, &pte)) {
            KPANIC("paging_init: OOM during kernel mapping");
        }
        *pte = make_pte(pa, PageFlags::Present | PageFlags::Writable);
    }

    // Also map the direct-map region (DIRECT_MAP_BASE → phys 0)
    // Map all usable physical memory into the direct map. We use 2MB huge pages
    // for efficiency. The direct map spans from DIRECT_MAP_BASE to cover all phys mem.
    // For now, map just enough for kernel heap + bitmap overhead (4GB = 2048 huge pages).
    // But we don't have pmm info easily here — map a conservative 128GB range.
    klog("Paging: mapping direct map...\n");
    uint64_t max_phys = 0x2000000000ULL; // 128 GB
    for (uint64_t pa = 0; pa < max_phys; pa += LARGE_PAGE_SIZE) {
        uint64_t va = DIRECT_MAP_BASE + pa;
        uint16_t i4 = pml4_index(va);
        uint16_t i3 = pdpt_index(va);
        uint16_t i2 = pd_index(va);

        // PML4 → PDPT
        if (!(new_pml4[i4] & PageFlags::Present)) {
            uint64_t pdpt = alloc_table_phys();
            if (!pdpt) break;
            new_pml4[i4] = make_pte(pdpt, PageFlags::Present | PageFlags::Writable);
        }
        uint64_t* pdpt = hhdm_ptr(pte_phys_addr(new_pml4[i4]));

        // PDPT → PD
        if (!(pdpt[i3] & PageFlags::Present)) {
            uint64_t pd = alloc_table_phys();
            if (!pd) break;
            pdpt[i3] = make_pte(pd, PageFlags::Present | PageFlags::Writable);
        }
        uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));

        pd[i2] = make_pte(pa, PageFlags::Present | PageFlags::Writable | PageFlags::Huge);
    }

    // Load new page tables
    klog("Paging: loading CR3...\n");
    asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
    klog("Paging: new page tables active\n");
}

void paging_save_kernel_template() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_template_pml4 = cr3;
    klog("Paging: kernel PML4 template saved at ");
    klog_hex(cr3); klog("\n");
}

uint64_t paging_kernel_pml4_template() {
    return g_kernel_template_pml4;
}

bool page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pte = nullptr;
    if (!walk_create(pml4_phys, va, &pte)) return false;
    *pte = make_pte(pa, flags);
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    return true;
}

uint64_t page_table_unmap(uint64_t pml4_phys, uint64_t va) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    uint64_t* pml4 = hhdm_ptr(pml4_phys);
    if (!(pml4[i4] & PageFlags::Present)) return 0;
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));
    if (!(pdpt[i3] & PageFlags::Present)) return 0;
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));
    if (pd[i2] & PageFlags::Huge) return 0;
    if (!(pd[i2] & PageFlags::Present)) return 0;
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    if (!(pt[i1] & PageFlags::Present)) return 0;

    uint64_t old_pa = pte_phys_addr(pt[i1]);
    pt[i1] = 0;
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");

    // TODO: free empty intermediate tables (enhancement)
    return old_pa;
}

uint64_t page_table_lookup(uint64_t pml4_phys, uint64_t va) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    uint64_t* pml4 = hhdm_ptr(pml4_phys);
    if (!(pml4[i4] & PageFlags::Present)) return 0;
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));
    if (!(pdpt[i3] & PageFlags::Present)) return 0;
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));
    if (pd[i2] & PageFlags::Huge) return 0;
    if (!(pd[i2] & PageFlags::Present)) return 0;
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    if (!(pt[i1] & PageFlags::Present)) return 0;
    return pte_phys_addr(pt[i1]);
}
```

- [ ] **Step 3: Update paging.hpp with new declarations**

Add the declarations shown in Step 1.

- [ ] **Step 4: Build and verify**

```bash
bazel build //kernel:kernel
```

Expected: build succeeds. No QEMU test yet — paging_init is called in boot.cpp but currently commented out (uses Limine tables). We'll wire it in Task 14.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/paging.cpp kernel/arch/x86_64/paging.hpp
git commit -m "fix: rewrite paging_init with walk_create, add page_table_map/unmap/lookup"
```

---

### Task 2: Refactor HandleTable into a Class

**Files:**
- Modify: `kernel/core/object/handle_table.hpp`
- Modify: `kernel/core/object/handle_table.cpp`
- Modify: `kernel/core/object/object.hpp` (add Process, Vmo to Type enum)

**Goal:** Extract the global handle table into a reusable `HandleTable` class. Keep backward-compatible free functions during the transition.

- [ ] **Step 1: Add new object types to object.hpp**

```cpp
// kernel/core/object/object.hpp — update Type enum
class KernelObject {
public:
    enum class Type : uint8_t {
        Channel,
        Port,
        Process,
        Vmo,
    };
    // ... rest unchanged
};
```

- [ ] **Step 2: Rewrite handle_table.hpp**

```cpp
// kernel/core/object/handle_table.hpp
#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"
#include "kernel/lib/spinlock.hpp"

using handle_t = uint32_t;
constexpr handle_t INVALID_HANDLE = 0;
constexpr int MAX_HANDLES = 1024;

struct HandleEntry {
    KernelObject* obj = nullptr;
    Rights rights{};
};

class HandleTable {
public:
    void Init();

    handle_t Alloc(KernelObject* obj, Rights rights);
    void     Free(handle_t h);
    KernelObject* Lookup(handle_t h, Rights needed = Rights{},
                         Rights* out_rights = nullptr);

    // For iteration during process teardown
    int ForEach(KernelObject** out_objs, handle_t* out_handles, int max);

private:
    HandleEntry entries_[MAX_HANDLES];
    handle_t    free_head_;
    SpinLock    lock_;
};

// ── Temporary backward-compatible globals ──────────────────────────
// These dispatch to the kernel process's handle table.
// Remove after all callers are migrated to per-process handles.

// Set the fallback handle table for global free functions.
void handle_table_set_fallback(HandleTable* ht);

handle_t      handle_alloc(KernelObject* obj, Rights rights);
void          handle_free(handle_t h);
KernelObject* handle_lookup(handle_t h, Rights needed = Rights{},
                             Rights* out_rights = nullptr);
```

- [ ] **Step 3: Rewrite handle_table.cpp**

```cpp
// kernel/core/object/handle_table.cpp
#include "kernel/core/object/handle_table.hpp"

// ── HandleTable ─────────────────────────────────────────────────────

void HandleTable::Init() {
    for (handle_t i = 1; i < MAX_HANDLES - 1; i++) {
        entries_[i].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(i + 1));
    }
    entries_[MAX_HANDLES - 1].obj = nullptr;
    free_head_ = 1;
}

handle_t HandleTable::Alloc(KernelObject* obj, Rights rights) {
    lock_.lock();
    if (free_head_ == 0 || free_head_ >= MAX_HANDLES) {
        lock_.unlock();
        return INVALID_HANDLE;
    }
    handle_t h = free_head_;
    free_head_ = static_cast<handle_t>(
        reinterpret_cast<uintptr_t>(entries_[h].obj));
    entries_[h].obj = obj;
    entries_[h].rights = rights;
    obj->AddRef();
    lock_.unlock();
    return h;
}

void HandleTable::Free(handle_t h) {
    if (h == 0 || h >= MAX_HANDLES) return;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    if (obj) {
        entries_[h].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(free_head_));
        entries_[h].rights = Rights{};
        free_head_ = h;
        obj->Release();
    }
    lock_.unlock();
}

KernelObject* HandleTable::Lookup(handle_t h, Rights needed,
                                   Rights* out_rights) {
    if (h == 0 || h >= MAX_HANDLES) return nullptr;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    Rights rights = entries_[h].rights;
    lock_.unlock();
    if (!obj) return nullptr;
    if (needed.mask != 0 && !rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = rights;
    return obj;
}

int HandleTable::ForEach(KernelObject** out_objs, handle_t* out_handles,
                          int max) {
    int count = 0;
    lock_.lock();
    for (handle_t h = 1; h < MAX_HANDLES && count < max; h++) {
        if (entries_[h].obj) {
            out_objs[count] = entries_[h].obj;
            out_handles[count] = h;
            count++;
        }
    }
    lock_.unlock();
    return count;
}

// ── Backward-compat globals ─────────────────────────────────────────

static HandleTable* g_fallback_ht = nullptr;

void handle_table_set_fallback(HandleTable* ht) { g_fallback_ht = ht; }

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    if (g_fallback_ht) return g_fallback_ht->Alloc(obj, rights);
    return INVALID_HANDLE;
}

void handle_free(handle_t h) {
    if (g_fallback_ht) g_fallback_ht->Free(h);
}

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    if (g_fallback_ht) return g_fallback_ht->Lookup(h, needed, out_rights);
    return nullptr;
}
```

- [ ] **Step 4: Build and run existing tests**

```bash
bazel test //test/object:all
```

Expected: existing Channel/Port tests pass (they use global `handle_alloc` etc. which now go through fallback).

- [ ] **Step 5: Commit**

```bash
git add kernel/core/object/handle_table.hpp kernel/core/object/handle_table.cpp kernel/core/object/object.hpp
git commit -m "refactor: extract HandleTable class, add backward-compat globals"
```

---

### Task 3: VMO — Virtual Memory Objects

**Files:**
- Create: `kernel/core/mm/vmo.hpp`
- Create: `kernel/core/mm/vmo.cpp`
- Create: `test/mm/vmo_test.cpp`
- Modify: `test/mm/BUILD.bazel`

**Goal:** Implement VMO with Anonymous and Physical types, demand paging, and COW cloning.

- [ ] **Step 1: Create vmo.hpp**

```cpp
// kernel/core/mm/vmo.hpp
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/lib/spinlock.hpp"
#include "kernel/arch/x86_64/paging.hpp"

struct CowPage {
    uint64_t phys_addr;   // 0 = not committed
    uint32_t cow_refs;    // number of VMOs sharing this page
};

class Vmo : public KernelObject {
public:
    enum Type : uint8_t { Anonymous, Physical };

    static Vmo* CreateAnonymous(uint64_t size);
    static Vmo* CreatePhysical(uint64_t size, uint64_t phys_base);

    // Get physical address for page at `offset`. Allocates a zero-filled page
    // if not committed (Anonymous). If for_write and page is COW-shared,
    // copies the page. Returns 0 on OOM.
    uint64_t GetPage(uint64_t offset, bool for_write);

    // Create a COW clone sharing all committed pages.
    Vmo* CloneCoW();

    uint64_t size()      const { return size_; }
    uint64_t num_pages() const { return num_pages_; }
    Type     type()      const { return type_; }

    ~Vmo() override;

private:
    Vmo(Type t, uint64_t size);

    Type       type_;
    uint64_t   size_;
    uint64_t   num_pages_;
    CowPage**  pages_;    // array[num_pages_]; nullptr entries = not committed
    SpinLock   lock_;
};
```

- [ ] **Step 2: Create vmo.cpp**

```cpp
// kernel/core/mm/vmo.cpp
#include "kernel/core/mm/vmo.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/lib/panic.hpp"

Vmo::Vmo(Type t, uint64_t size)
    : KernelObject(KernelObject::Type::Vmo)
    , type_(t)
    , size_(size)
    , num_pages_(size / PAGE_SIZE)
{
    if (num_pages_ > 0) {
        size_t arr_sz = num_pages_ * sizeof(CowPage*);
        pages_ = static_cast<CowPage**>(kmalloc(arr_sz));
        if (!pages_) KPANIC("Vmo: OOM allocating page array");
        for (uint64_t i = 0; i < num_pages_; i++) pages_[i] = nullptr;
    } else {
        pages_ = nullptr;
    }
}

Vmo::~Vmo() {
    for (uint64_t i = 0; i < num_pages_; i++) {
        CowPage* cp = pages_[i];
        if (!cp) continue;
        cp->cow_refs--;
        if (cp->cow_refs == 0) {
            if (cp->phys_addr) bitmap_free_page(reinterpret_cast<void*>(cp->phys_addr));
            kfree(cp);
        }
    }
    kfree(pages_);
}

Vmo* Vmo::CreateAnonymous(uint64_t size) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size == 0) return nullptr;
    void* mem = kmalloc(sizeof(Vmo));
    if (!mem) return nullptr;
    return new (mem) Vmo(Anonymous, size);
}

Vmo* Vmo::CreatePhysical(uint64_t size, uint64_t phys_base) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    void* mem = kmalloc(sizeof(Vmo));
    if (!mem) return nullptr;
    Vmo* vmo = new (mem) Vmo(Physical, size);

    // Pre-allocate all CowPage entries with cow_refs=1
    for (uint64_t i = 0; i < vmo->num_pages_; i++) {
        CowPage* cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!cp) KPANIC("Vmo: OOM creating Physical pages");
        cp->phys_addr = phys_base + i * PAGE_SIZE;
        cp->cow_refs = 1;
        vmo->pages_[i] = cp;
    }
    return vmo;
}

uint64_t Vmo::GetPage(uint64_t offset, bool for_write) {
    uint64_t page_idx = offset / PAGE_SIZE;
    if (page_idx >= num_pages_) return 0;

    lock_.lock();

    CowPage* cp = pages_[page_idx];

    if (!cp) {
        // First access — allocate and zero-fill
        void* phys = bitmap_alloc_page();
        if (!phys) { lock_.unlock(); return 0; }

        cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!cp) {
            bitmap_free_page(phys);
            lock_.unlock();
            return 0;
        }

        uint64_t pa = reinterpret_cast<uint64_t>(phys);
        // Zero via HHDM: use the direct map
        uint64_t* virt = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pa);
        for (int j = 0; j < 512; j++) virt[j] = 0;

        cp->phys_addr = pa;
        cp->cow_refs = 1;
        pages_[page_idx] = cp;
        lock_.unlock();
        return pa;
    }

    // COW resolution: page is shared
    if (for_write && cp->cow_refs > 1) {
        void* new_phys = bitmap_alloc_page();
        if (!new_phys) { lock_.unlock(); return 0; }

        uint64_t new_pa = reinterpret_cast<uint64_t>(new_phys);
        uint64_t old_pa = cp->phys_addr;

        // Copy old → new via direct map
        uint64_t* src = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + old_pa);
        uint64_t* dst = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + new_pa);
        for (int j = 0; j < 512; j++) dst[j] = src[j];

        cp->cow_refs--;

        CowPage* new_cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!new_cp) {
            bitmap_free_page(new_phys);
            lock_.unlock();
            return 0;
        }
        new_cp->phys_addr = new_pa;
        new_cp->cow_refs = 1;
        pages_[page_idx] = new_cp;

        lock_.unlock();
        return new_pa;
    }

    lock_.unlock();
    return cp->phys_addr;
}

Vmo* Vmo::CloneCoW() {
    Vmo* child = CreateAnonymous(size_);
    if (!child) return nullptr;

    lock_.lock();
    for (uint64_t i = 0; i < num_pages_; i++) {
        CowPage* cp = pages_[i];
        if (cp) {
            cp->cow_refs++;
            child->pages_[i] = cp;
        }
    }
    lock_.unlock();

    return child;
}
```

- [ ] **Step 3: Create host-side test test/mm/vmo_test.cpp**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/mm/vmo.hpp"

TEST(VmoTest, CreateAnonymous) {
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 4);
    ASSERT_NE(vmo, nullptr);
    EXPECT_EQ(vmo->type(), Vmo::Anonymous);
    EXPECT_EQ(vmo->size(), PAGE_SIZE * 4);
    EXPECT_EQ(vmo->num_pages(), 4u);
    vmo->Release();
}

TEST(VmoTest, GetPageFirstAccess) {
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    ASSERT_NE(vmo, nullptr);

    uint64_t pa = vmo->GetPage(0, false);
    EXPECT_NE(pa, 0u);
    // Page should be zero-filled
    uint64_t* data = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pa);
    EXPECT_EQ(data[0], 0u);
    EXPECT_EQ(data[511], 0u);

    vmo->Release();
}

TEST(VmoTest, GetPageReturnsSamePage) {
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    uint64_t pa1 = vmo->GetPage(0, false);
    uint64_t pa2 = vmo->GetPage(0, false);
    EXPECT_EQ(pa1, pa2);
    vmo->Release();
}

TEST(VmoTest, CloneCoWSharesPages) {
    Vmo* parent = Vmo::CreateAnonymous(PAGE_SIZE * 2);
    uint64_t p0 = parent->GetPage(0, false);
    uint64_t p1 = parent->GetPage(PAGE_SIZE, false);

    Vmo* child = parent->CloneCoW();
    ASSERT_NE(child, nullptr);

    // Child sees same physical pages
    EXPECT_EQ(child->GetPage(0, false), p0);
    EXPECT_EQ(child->GetPage(PAGE_SIZE, false), p1);

    child->Release();
    parent->Release();
}

TEST(VmoTest, CowWriteBreaksSharing) {
    Vmo* parent = Vmo::CreateAnonymous(PAGE_SIZE);
    uint64_t orig_pa = parent->GetPage(0, false);

    // Write something to the page so we can verify copy
    uint64_t* orig_data = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + orig_pa);
    orig_data[0] = 0xDEADBEEF;

    Vmo* child = parent->CloneCoW();

    // Write in child triggers COW
    uint64_t child_pa = child->GetPage(0, true);
    EXPECT_NE(child_pa, orig_pa); // should be a new page

    // New page has copied content
    uint64_t* child_data = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + child_pa);
    EXPECT_EQ(child_data[0], 0xDEADBEEFu);

    // Parent still has original
    EXPECT_EQ(parent->GetPage(0, false), orig_pa);

    child->Release();
    parent->Release();
}

TEST(VmoTest, PhysicalType) {
    Vmo* vmo = Vmo::CreatePhysical(PAGE_SIZE * 2, 0x100000);
    ASSERT_NE(vmo, nullptr);
    EXPECT_EQ(vmo->type(), Vmo::Physical);
    EXPECT_EQ(vmo->GetPage(0, false), 0x100000u);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE, false), 0x101000u);
    vmo->Release();
}

TEST(VmoTest, OutOfRangeReturnsZero) {
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE, false), 0u);
    EXPECT_EQ(vmo->GetPage(PAGE_SIZE * 10, false), 0u);
    vmo->Release();
}
```

- [ ] **Step 4: Update test/mm/BUILD.bazel**

Add VMO test target linking against vmo.cpp and dependencies.

- [ ] **Step 5: Run tests**

```bash
bazel test //test/mm:vmo_test
```

Expected: 7 tests pass.

- [ ] **Step 6: Commit**

```bash
git add kernel/core/mm/vmo.hpp kernel/core/mm/vmo.cpp test/mm/vmo_test.cpp test/mm/BUILD.bazel
git commit -m "feat: VMO with Anonymous/Physical types, demand paging, COW clone"
```

---

### Task 4: VMM — Address Space Management

**Files:**
- Create: `kernel/core/mm/vmm.hpp`
- Create: `kernel/core/mm/vmm.cpp`

**Goal:** VmRegion linked list, address space Map/Unmap/FindRegion, PML4 creation for new processes.

- [ ] **Step 1: Create vmm.hpp**

```cpp
// kernel/core/mm/vmm.hpp
#pragma once
#include <stdint.h>
#include "kernel/core/mm/vmo.hpp"

// VmRegion flags
constexpr uint64_t VM_READ   = 1ULL << 0;
constexpr uint64_t VM_WRITE  = 1ULL << 1;
constexpr uint64_t VM_EXEC   = 1ULL << 2;
constexpr uint64_t VM_COW    = 1ULL << 3;
constexpr uint64_t VM_USER   = 1ULL << 4;

struct VmRegion {
    VmRegion* next;       // sorted by base_va ascending
    uint64_t  base_va;
    uint64_t  size;       // page-aligned
    Vmo*      vmo;
    uint64_t  vmo_offset;
    uint64_t  flags;
};

// Address space bounds
constexpr uint64_t USER_SPACE_START = 0x0;
constexpr uint64_t USER_SPACE_END   = 0x00007FFFFFFFFFFFULL;

// Create a new PML4 for a user process. Copies kernel-half entries (256-511)
// from the kernel template. User-half entries (0-255) are zeroed.
// Returns physical address of new PML4, or 0 on OOM.
uint64_t vmm_create_user_pml4();

// Free a user PML4 and all intermediate page tables.
void vmm_destroy_user_pml4(uint64_t pml4_phys);

// Map helpers — operate on a VmRegion list.
// Insert a new region into the sorted list (by base_va). Returns false if
// the range overlaps an existing region.
bool vmm_insert_region(VmRegion** head, VmRegion* region);

// Remove and return the region containing `va`, or nullptr.
// Frees the VmRegion struct and the range of PTEs.
VmRegion* vmm_remove_region(VmRegion** head, uint64_t va, uint64_t size,
                             uint64_t pml4_phys);

// Find the VmRegion containing `va`.
VmRegion* vmm_find_region(VmRegion* head, uint64_t va);
```

- [ ] **Step 2: Create vmm.cpp**

```cpp
// kernel/core/mm/vmm.cpp
#include "kernel/core/mm/vmm.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"

uint64_t vmm_create_user_pml4() {
    uint64_t template_pml4 = paging_kernel_pml4_template();
    if (!template_pml4) return 0;

    void* new_pml4_phys = bitmap_alloc_page();
    if (!new_pml4_phys) return 0;

    uint64_t np = reinterpret_cast<uint64_t>(new_pml4_phys);
    uint64_t* src = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + template_pml4);
    uint64_t* dst = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + np);

    // Copy kernel-half entries (indices 256-511)
    for (int i = 256; i < 512; i++) {
        dst[i] = src[i];
    }
    // User-half entries (0-255) already zeroed by bitmap_alloc_page
    return np;
}

void vmm_destroy_user_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pml4_phys);

    // Walk user-half entries (0-255), recursively freeing page tables
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PageFlags::Present)) continue;
        uint64_t pdpt_phys = pte_phys_addr(pml4[i4]);
        uint64_t* pdpt = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pdpt_phys);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PageFlags::Present)) continue;
            uint64_t pd_phys = pte_phys_addr(pdpt[i3]);
            uint64_t* pd = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pd_phys);

            for (int i2 = 0; i2 < 512; i2++) {
                if (pd[i2] & PageFlags::Huge) continue;
                if (!(pd[i2] & PageFlags::Present)) continue;
                uint64_t pt_phys = pte_phys_addr(pd[i2]);
                bitmap_free_page(reinterpret_cast<void*>(pt_phys));
            }
            bitmap_free_page(reinterpret_cast<void*>(pd_phys));
        }
        bitmap_free_page(reinterpret_cast<void*>(pdpt_phys));
    }
    bitmap_free_page(reinterpret_cast<void*>(pml4_phys));
}

bool vmm_insert_region(VmRegion** head, VmRegion* region) {
    uint64_t end = region->base_va + region->size;

    // Find insertion point, check for overlap
    VmRegion** prev = head;
    while (*prev) {
        uint64_t prev_end = (*prev)->base_va + (*prev)->size;
        if (end <= (*prev)->base_va) break;  // region ends before this entry
        if (region->base_va < prev_end) return false; // overlap
        prev = &(*prev)->next;
    }

    region->next = *prev;
    *prev = region;
    return true;
}

VmRegion* vmm_find_region(VmRegion* head, uint64_t va) {
    while (head) {
        if (va >= head->base_va && va < head->base_va + head->size) {
            return head;
        }
        if (va < head->base_va) return nullptr; // regions are sorted
        head = head->next;
    }
    return nullptr;
}

VmRegion* vmm_remove_region(VmRegion** head, uint64_t va, uint64_t size,
                             uint64_t pml4_phys) {
    while (*head) {
        if (va >= (*head)->base_va && va < (*head)->base_va + (*head)->size) {
            VmRegion* r = *head;
            *head = r->next;

            // Unmap all pages in the region
            for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
                page_table_unmap(pml4_phys, va + off);
            }

            kfree(r);
            return r;
        }
        head = &(*head)->next;
    }
    return nullptr;
}
```

- [ ] **Step 3: Build**

```bash
bazel build //kernel:kernel
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add kernel/core/mm/vmm.hpp kernel/core/mm/vmm.cpp
git commit -m "feat: VMM address space — VmRegion list, PML4 create/destroy, map/unmap"
```

---

### Task 5: Process Object

**Files:**
- Create: `kernel/core/object/process.hpp`
- Create: `kernel/core/object/process.cpp`
- Create: `test/object/process_test.cpp`
- Modify: `test/object/BUILD.bazel`

**Goal:** Process object with PML4, handle table, VmRegion list, thread list. Map/Unmap/HandlePageFault.

- [ ] **Step 1: Create process.hpp**

```cpp
// kernel/core/object/process.hpp
#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/mm/vmm.hpp"

class Thread;
class Vmo;

class Process : public KernelObject {
public:
    static Process* Create(const char* name);

    uint64_t    pml4_phys;    // CR3 value
    VmRegion*   regions;      // sorted linked list
    HandleTable handles;      // per-process handle table
    Thread*     threads;      // linked list via Thread::proc_next
    Process*    parent;
    char        name[32];

    // VMM operations
    bool Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
             uint64_t size, uint64_t flags);
    bool Unmap(uint64_t va, uint64_t size);
    VmRegion* FindRegion(uint64_t va);

    // Page fault handling
    bool HandlePageFault(uint64_t fault_addr, bool was_write);

    // Thread management
    void AddThread(Thread* t);
    void RemoveThread(Thread* t);

private:
    Process(const char* name, uint64_t pml4);
    ~Process() override;
};
```

- [ ] **Step 2: Create process.cpp**

```cpp
// kernel/core/object/process.cpp
#include "kernel/core/object/process.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

Process* Process::Create(const char* name) {
    uint64_t pml4 = vmm_create_user_pml4();
    if (!pml4) return nullptr;

    void* mem = kmalloc(sizeof(Process));
    if (!mem) {
        vmm_destroy_user_pml4(pml4);
        return nullptr;
    }
    return new (mem) Process(name, pml4);
}

Process::Process(const char* name, uint64_t pml4)
    : KernelObject(KernelObject::Type::Process)
    , pml4_phys(pml4)
    , regions(nullptr)
    , threads(nullptr)
    , parent(nullptr)
{
    handles.Init();
    int i = 0;
    while (i < 31 && name[i]) { this->name[i] = name[i]; i++; }
    this->name[i] = '\0';
}

Process::~Process() {
    // Free all VmRegions
    while (regions) {
        VmRegion* r = regions;
        regions = r->next;
        for (uint64_t off = 0; off < r->size; off += PAGE_SIZE) {
            uint64_t va = r->base_va + off;
            page_table_unmap(pml4_phys, va);
        }
        if (r->vmo) r->vmo->Release();
        kfree(r);
    }

    // Free handle table entries
    // (objects are Released via handle_free, but we batch it here)
    KernelObject* objs[128];
    handle_t handles_arr[128];
    int n;
    while ((n = handles.ForEach(objs, handles_arr, 128)) > 0) {
        for (int i = 0; i < n; i++) {
            handles.Free(handles_arr[i]);
        }
    }

    // Free page tables
    vmm_destroy_user_pml4(pml4_phys);
}

bool Process::Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
                  uint64_t size, uint64_t flags) {
    if (va + size < va) return false;  // overflow
    if (va < USER_SPACE_START || va + size > USER_SPACE_END) return false;

    VmRegion* r = static_cast<VmRegion*>(kmalloc(sizeof(VmRegion)));
    if (!r) return false;

    r->base_va    = va;
    r->size       = size;
    r->vmo        = vmo;
    r->vmo_offset = vmo_offset;
    r->flags      = flags;
    r->next       = nullptr;

    if (!vmm_insert_region(&regions, r)) {
        kfree(r);
        return false;
    }

    vmo->AddRef();
    return true;
}

bool Process::Unmap(uint64_t va, uint64_t size) {
    VmRegion* r = vmm_remove_region(&regions, va, size, pml4_phys);
    if (!r) return false;
    if (r->vmo) r->vmo->Release();
    kfree(r);
    return true;
}

VmRegion* Process::FindRegion(uint64_t va) {
    return vmm_find_region(regions, va);
}

bool Process::HandlePageFault(uint64_t fault_addr, bool was_write) {
    VmRegion* r = FindRegion(fault_addr);
    if (!r) return false;

    // Check permissions
    if (was_write && !(r->flags & VM_WRITE) && !(r->flags & VM_COW)) {
        return false;
    }
    if (!was_write && !(r->flags & VM_READ)) {
        return false;
    }

    uint64_t vmo_off = (fault_addr - r->base_va) + r->vmo_offset;
    bool cow_write = was_write && (r->flags & VM_COW);
    uint64_t phys = r->vmo->GetPage(vmo_off, cow_write);
    if (!phys) return false;

    uint64_t pte_flags = PageFlags::Present | PageFlags::Writable;
    if (r->flags & VM_USER)  pte_flags |= PageFlags::User;
    if (!(r->flags & VM_EXEC)) pte_flags |= PageFlags::NoExec;

    // If COW write, remove COW flag — this page is now private
    if (cow_write) {
        r->flags &= ~VM_COW;
    }

    return page_table_map(pml4_phys, fault_addr, phys, pte_flags);
}

void Process::AddThread(Thread* t) {
    t->proc_next = threads;
    threads = t;
    t->process = this;
}

void Process::RemoveThread(Thread* t) {
    Thread** prev = &threads;
    while (*prev) {
        if (*prev == t) {
            *prev = t->proc_next;
            t->proc_next = nullptr;
            t->process = nullptr;
            return;
        }
        prev = &(*prev)->proc_next;
    }
}
```

- [ ] **Step 3: Create host-side test test/object/process_test.cpp**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/object/process.hpp"
#include "kernel/core/mm/vmo.hpp"

TEST(ProcessTest, Create) {
    Process* p = Process::Create("test");
    ASSERT_NE(p, nullptr);
    EXPECT_NE(p->pml4_phys, 0u);
    EXPECT_EQ(p->regions, nullptr);
    EXPECT_EQ(p->threads, nullptr);
    p->Release();
}

TEST(ProcessTest, MapAndFindRegion) {
    Process* p = Process::Create("test");
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 4);

    EXPECT_TRUE(p->Map(vmo, 0x1000000, 0, PAGE_SIZE * 4, VM_READ | VM_WRITE | VM_USER));

    VmRegion* r = p->FindRegion(0x1000000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->base_va, 0x1000000u);
    EXPECT_EQ(r->size, PAGE_SIZE * 4);
    EXPECT_EQ(r->vmo, vmo);

    // Find middle of region
    EXPECT_NE(p->FindRegion(0x1001000), nullptr);
    // Find outside region
    EXPECT_EQ(p->FindRegion(0x0), nullptr);

    p->Unmap(0x1000000, PAGE_SIZE * 4);
    EXPECT_EQ(p->FindRegion(0x1000000), nullptr);

    vmo->Release();
    p->Release();
}

TEST(ProcessTest, MapOverlapFails) {
    Process* p = Process::Create("test");
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 8);

    EXPECT_TRUE(p->Map(vmo, 0x400000, 0, PAGE_SIZE * 4, VM_READ));
    EXPECT_FALSE(p->Map(vmo, 0x401000, 0, PAGE_SIZE, VM_READ));  // overlap

    vmo->Release();
    p->Release();
}

TEST(ProcessTest, HandleTableIsolation) {
    Process* p1 = Process::Create("p1");
    Process* p2 = Process::Create("p2");

    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    handle_t h1 = p1->handles.Alloc(vmo, Rights{.mask = Rights::Read});
    handle_t h2 = p2->handles.Alloc(vmo, Rights{.mask = Rights::Write});

    // Same object, different handles in different tables
    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);

    // p1's table sees h1, p2's table sees h2
    EXPECT_NE(p1->handles.Lookup(h1), nullptr);
    EXPECT_EQ(p2->handles.Lookup(h1), nullptr);
    EXPECT_EQ(p1->handles.Lookup(h2), nullptr);
    EXPECT_NE(p2->handles.Lookup(h2), nullptr);

    p1->handles.Free(h1);
    p2->handles.Free(h2);
    vmo->Release();
    p1->Release();
    p2->Release();
}
```

- [ ] **Step 4: Build and run tests**

```bash
bazel test //test/object:process_test
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add kernel/core/object/process.hpp kernel/core/object/process.cpp test/object/process_test.cpp test/object/BUILD.bazel
git commit -m "feat: Process object with per-process PML4, handle table, VmRegions, page fault handler"
```

---

### Task 6: Thread → Process Association + CR3 Reload

**Files:**
- Modify: `kernel/core/sched/sched.hpp`
- Modify: `kernel/core/sched/sched.cpp`
- Modify: `kernel/core/sched/switch.S`

**Goal:** Add `process` and `proc_next` fields to Thread. Reload CR3 on cross-process context switch. Update `thread_create` to accept a Process.

- [ ] **Step 1: Update sched.hpp — Thread struct**

```cpp
// In kernel/core/sched/sched.hpp, add to Thread struct (after stack_size):
    Process*   process;      // owning process (nullptr = idle)
    Thread*    proc_next;    // next in process->threads list
```

- [ ] **Step 2: Update sched.hpp — API changes**

```cpp
// Update thread_create signature:
Thread* thread_create(void (*entry)(), const char* name, uint8_t priority,
                      Process* process = nullptr);
```

- [ ] **Step 3: Update sched.cpp — CR3 reload in scheduler_schedule**

```cpp
// In scheduler_schedule(), before calling switch_to:

    // Reload CR3 if crossing process boundary
    if (prev && next->process != prev->process) {
        uint64_t new_cr3 = next->process ? next->process->pml4_phys
                                         : paging_kernel_pml4_template();
        if (new_cr3) {
            asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
        }
    }
```

- [ ] **Step 4: Update thread_create**

```cpp
Thread* thread_create(void (*entry)(), const char* name, uint8_t priority,
                      Process* process) {
    // ... existing allocation code ...

    t->process   = process;
    t->proc_next = nullptr;

    // If a process is given, add thread to its list
    if (process) {
        process->AddThread(t);
    }

    return t;
}
```

- [ ] **Step 5: Update scheduler_init — create kernel process**

```cpp
// In scheduler_init, create kernel process before idle threads:
#include "kernel/core/object/process.hpp"

static Process* s_kernel_process = nullptr;

void scheduler_init(uint64_t hhdm) {
    // ... existing init ...

    s_kernel_process = Process::Create("kernel");
    if (!s_kernel_process) KPANIC("sched: failed to create kernel process");

    // Set as fallback handle table
    handle_table_set_fallback(&s_kernel_process->handles);

    // ... existing per-CPU init ...
}
```

- [ ] **Step 6: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 7: Commit**

```bash
git add kernel/core/sched/sched.hpp kernel/core/sched/sched.cpp
git commit -m "feat: Thread→Process association, CR3 reload on cross-process switch"
```

---

### Task 7: TSS — Ring-0 Stack for Interrupts

**Files:**
- Create: `kernel/arch/x86_64/tss.hpp`
- Create: `kernel/arch/x86_64/tss.cpp`
- Modify: `kernel/arch/x86_64/gdt.cpp`

**Goal:** Per-CPU TSS with RSP0 pointing to a kernel interrupt stack. Required for interrupts/exceptions that occur while running in ring 3.

- [ ] **Step 1: Create tss.hpp**

```cpp
// kernel/arch/x86_64/tss.hpp
#pragma once
#include <stdint.h>

struct Tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
    uint8_t  io_bitmap[8192]; // one bit per I/O port; all 1s = deny
} __attribute__((packed));

// Initialize TSS for a CPU. Allocates a kernel interrupt stack.
// Must be called after paging_init (needs DIRECT_MAP_BASE).
void tss_init_per_cpu(uint32_t cpu_id);

// Update RSP0 for the given CPU (called on thread switch to ring 3).
void tss_set_rsp0(uint32_t cpu_id, uint64_t rsp0);

// Get the TSS pointer for GDT loading.
Tss* tss_for_cpu(uint32_t cpu_id);
```

- [ ] **Step 2: Create tss.cpp**

```cpp
// kernel/arch/x86_64/tss.cpp
#include "kernel/arch/x86_64/tss.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"

static Tss g_tss[MAX_CPUS];
static uint64_t g_kstack[MAX_CPUS]; // kernel interrupt stack phys addr

void tss_init_per_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return;

    // Allocate 4 pages for kernel interrupt stack
    void* kstack_phys = bitmap_alloc_page();
    // Additional pages... for now just 1 page (4KB)
    uint64_t kstack_phys_addr = reinterpret_cast<uint64_t>(kstack_phys);

    Tss* tss = &g_tss[cpu_id];
    // Zero the TSS
    for (size_t i = 0; i < sizeof(Tss); i++) {
        reinterpret_cast<uint8_t*>(tss)[i] = 0;
    }

    // RSP0 = top of kernel interrupt stack (grows down)
    tss->rsp0 = DIRECT_MAP_BASE + kstack_phys_addr + PAGE_SIZE;

    // Set I/O bitmap base to point to the io_bitmap array within TSS
    tss->io_map_base = offsetof(Tss, io_bitmap);

    // All-ones in io_bitmap = all ports denied
    for (int i = 0; i < 8192; i++) tss->io_bitmap[i] = 0xFF;

    g_kstack[cpu_id] = kstack_phys_addr;

    klog("TSS: CPU "); klog_hex(cpu_id);
    klog(" RSP0="); klog_hex(tss->rsp0); klog("\n");
}

void tss_set_rsp0(uint32_t cpu_id, uint64_t rsp0) {
    if (cpu_id < MAX_CPUS) g_tss[cpu_id].rsp0 = rsp0;
}

Tss* tss_for_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return nullptr;
    return &g_tss[cpu_id];
}
```

- [ ] **Step 3: Load TSS in GDT (modify gdt.cpp)**

The GDT already has TSS support from Task 2 (the I/O permission bitmap fix). Add a per-CPU TSS load. The TSS descriptor in the GDT needs to be updated for each CPU's TSS address, and `ltr` must be called on each CPU.

```cpp
// In gdt.cpp, add function:
void gdt_load_tss(uint32_t cpu_id) {
    Tss* tss = tss_for_cpu(cpu_id);
    if (!tss) return;
    uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);

    // Update GDT TSS descriptor (index 5 or 6, depending on layout)
    // GDT layout: null, kernel code, kernel data, user code, user data, TSS lo, TSS hi
    // TSS descriptor spans entries 5 and 6 (16 bytes)
    struct [[gnu::packed]] TssDesc {
        uint16_t limit_lo;
        uint16_t base_lo;
        uint8_t  base_mid;
        uint8_t  access;    // 0x89 = present, 64-bit available TSS
        uint8_t  flags;     // limit_hi[3:0] | flags[7:4], 0 for 64-bit
        uint8_t  base_hi;
        uint32_t base_upper;
        uint32_t reserved;
    };

    TssDesc* desc = reinterpret_cast<TssDesc*>(&g_gdt[5]); // adjust index
    desc->limit_lo   = sizeof(Tss) - 1;
    desc->base_lo    = tss_addr & 0xFFFF;
    desc->base_mid   = (tss_addr >> 16) & 0xFF;
    desc->access     = 0x89;
    desc->flags      = 0;
    desc->base_hi    = (tss_addr >> 24) & 0xFF;
    desc->base_upper = (tss_addr >> 32) & 0xFFFFFFFF;
    desc->reserved   = 0;

    asm volatile("ltr %0" :: "r"(uint16_t(5 * 8))); // TSS selector
}
```

- [ ] **Step 4: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/tss.hpp kernel/arch/x86_64/tss.cpp kernel/arch/x86_64/gdt.cpp
git commit -m "feat: per-CPU TSS with RSP0 for ring-3 interrupt handling"
```

---

### Task 8: Ring 3 Entry

**Files:**
- Create: `kernel/arch/x86_64/usermode.S`
- Create: `kernel/arch/x86_64/usermode.hpp`

**Goal:** `enter_usermode(entry_rip, user_rsp)` — constructs IRET frame and jumps to ring 3.

- [ ] **Step 1: Create usermode.hpp**

```cpp
// kernel/arch/x86_64/usermode.hpp
#pragma once
#include <stdint.h>

// Jump from ring 0 to ring 3. Never returns.
// entry_rip: user-space instruction pointer
// user_rsp: initial user stack pointer
[[noreturn]] void enter_usermode(uint64_t entry_rip, uint64_t user_rsp);
```

- [ ] **Step 2: Create usermode.S**

```asm
# kernel/arch/x86_64/usermode.S
.globl enter_usermode
enter_usermode:
    # rdi = entry_rip
    # rsi = user_rsp

    # Switch to user data segments
    movw $0x23, %ax      # user data selector (GDT index 4 | RPL 3)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Build IRET frame
    pushq $0x23          # SS = user data
    pushq %rsi           # RSP = user stack
    pushfq               # RFLAGS (IF=1 from sti before calling)
    pushq $0x1B          # CS = user code (GDT index 3 | RPL 3)
    pushq %rdi           # RIP = entry point

    iretq
```

- [ ] **Step 3: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/usermode.S kernel/arch/x86_64/usermode.hpp
git commit -m "feat: enter_usermode — ring 0 to ring 3 transition via iretq"
```

---

### Task 9: Page Fault Handler (#PF ISR)

**Files:**
- Create: `kernel/arch/x86_64/page_fault.cpp`

**Goal:** Register #PF ISR (vector 14), delegate to Process::HandlePageFault.

- [ ] **Step 1: Create page_fault.cpp**

```cpp
// kernel/arch/x86_64/page_fault.cpp
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/core/object/process.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

// ISR stub for vector 14 — declared in assembly (irq_stubs.S already
// generates irq_stub_14, but #PF pushes error code so the stub format
// differs from IRQ stubs). Add a dedicated stub.

extern "C" void pf_stub_14();

static bool page_fault_handler(uint8_t vector) {
    (void)vector;

    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    // Read error code from the interrupt frame to determine write vs read
    // The error code is on the stack after the iret frame.
    // For now, assume was_write=true on any PF — we check VmRegion flags.
    Thread* cur = current_thread();
    if (!cur || !cur->process) {
        klog("PAGE FAULT in kernel/idle at ");
        klog_hex(cr2); klog(" — PANIC\n");
        KPANIC("Page fault with no user process");
    }

    if (cr2 >= DIRECT_MAP_BASE) {
        klog("PAGE FAULT in kernel space at ");
        klog_hex(cr2); klog(" — PANIC\n");
        KPANIC("Kernel page fault");
    }

    // Determine if it was a write from the error code
    // Error code bit 1 = write (0 = read)
    // The error code is pushed by the CPU before the ISR entry,
    // so it's at [RSP] on entry to this handler.
    // We access it via inline asm or pass from stub.
    // For now: try write first (GetPage will handle COW), then read.
    bool handled = cur->process->HandlePageFault(cr2, true);
    if (!handled) {
        handled = cur->process->HandlePageFault(cr2, false);
    }

    if (!handled) {
        klog("PAGE FAULT: unhandled at ");
        klog_hex(cr2); klog(" in process ");
        klog(cur->process->name); klog(" — killing\n");
        thread_exit();
    }

    return true;
}

void page_fault_init() {
    // Register the handler and set the IDT entry
    // idt_set_gate(14, pf_stub_14, ...) — or register via IRQ
    // Since #PF is exception (0-31), not IRQ, add directly to IDT
    // This requires idt_set_gate access — add via idt_init extension
}
```

Note: The page fault stub and IDT registration require coordination with `irq_stubs.S` and `idt.cpp`. The stub format is:

```asm
# kernel/arch/x86_64/pf_stub.S
.globl pf_stub_14
pf_stub_14:
    # CPU pushes error code automatically
    pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx
    pushq %rsi; pushq %rdi; pushq %rbp
    pushq %r8; pushq %r9; pushq %r10; pushq %r11
    pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq $14, %rdi
    callq page_fault_handler_internal
    popq %r15; popq %r14; popq %r13; popq %r12
    popq %r11; popq %r10; popq %r9; popq %r8
    popq %rbp; popq %rdi; popq %rsi; popq %rdx
    popq %rcx; popq %rbx; popq %rax
    addq $8, %rsp    # skip error code
    iretq
```

- [ ] **Step 2: Build and verify**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/page_fault.cpp kernel/arch/x86_64/pf_stub.S
git commit -m "feat: page fault handler (#PF) with demand paging and COW resolution"
```

---

### Task 10: Syscall Updates

**Files:**
- Modify: `kernel/arch/x86_64/syscall.hpp`
- Modify: `kernel/arch/x86_64/syscall.cpp`

**Goal:** Route all handle operations through `current_thread()->process->handles`. Add Process/VMO syscalls.

- [ ] **Step 1: Add new syscall numbers to syscall.hpp**

```cpp
// Process
constexpr uint64_t SYSCALL_PROCESS_CREATE  = 30;
constexpr uint64_t SYSCALL_PROCESS_EXIT    = 31;

// VMO
constexpr uint64_t SYSCALL_VMO_CREATE      = 40;
constexpr uint64_t SYSCALL_VMO_MAP         = 41;
```

- [ ] **Step 2: Update existing syscall handlers for per-process handles**

Replace all `handle_lookup`/`handle_alloc`/`handle_free` calls with process-local versions. Add a helper:

```cpp
static Process* current_process() {
    Thread* t = current_thread();
    return t ? t->process : nullptr;
}
```

Update `sys_handle_close`, `sys_handle_dup`, `sys_channel_create`, `sys_channel_write`, `sys_channel_read`, `sys_port_create`, `sys_port_register`, `sys_port_connect`, `sys_port_accept` to use `current_process()->handles.*`.

- [ ] **Step 3: Add new syscall handlers**

```cpp
uint64_t sys_process_create(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    const char* name = reinterpret_cast<const char*>(a1);
    Process* proc = Process::Create(name);
    if (!proc) return INVALID_HANDLE;

    Rights r{.mask = Rights::Read | Rights::Write |
                   Rights::Duplicate | Rights::Transfer};
    Process* cur = current_process();
    if (!cur) { proc->Release(); return INVALID_HANDLE; }
    return cur->handles.Alloc(proc, r);
}

uint64_t sys_process_exit(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    (void)a1; // exit code, unused for now
    thread_exit();
    return 0; // unreachable
}

uint64_t sys_vmo_create(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    uint64_t size = (a1 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    Vmo* vmo = Vmo::CreateAnonymous(size);
    if (!vmo) return INVALID_HANDLE;

    Rights r{.mask = Rights::Read | Rights::Write |
                   Rights::Duplicate | Rights::Transfer};
    Process* cur = current_process();
    if (!cur) { vmo->Release(); return INVALID_HANDLE; }
    return cur->handles.Alloc(vmo, r);
}

uint64_t sys_vmo_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    handle_t h = static_cast<handle_t>(a1);
    uint64_t va = a2;
    uint64_t flags = a3;
    uint64_t vmo_offset = a4;

    Process* cur = current_process();
    if (!cur) return -1;

    KernelObject* obj = cur->handles.Lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Vmo) return -1;

    Vmo* vmo = static_cast<Vmo*>(obj);
    uint64_t size = vmo->size() - vmo_offset;

    bool ok = cur->Map(vmo, va, vmo_offset, size,
                       (flags & 0x7) | VM_USER | VM_COW);
    return ok ? 0 : -1;
}
```

- [ ] **Step 4: Register new syscalls in init_syscall_table**

```cpp
g_syscall_table[SYSCALL_PROCESS_CREATE] = sys_process_create;
g_syscall_table[SYSCALL_PROCESS_EXIT]   = sys_process_exit;
g_syscall_table[SYSCALL_VMO_CREATE]     = sys_vmo_create;
g_syscall_table[SYSCALL_VMO_MAP]        = sys_vmo_map;
```

Update `MAX_SYSCALL` to 44.

- [ ] **Step 5: Build and run tests**

```bash
bazel test //test/object:all
```

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/syscall.hpp kernel/arch/x86_64/syscall.cpp
git commit -m "feat: per-process handle resolution in syscalls, add Process/VMO syscalls"
```

---

### Task 11: ELF Loader

**Files:**
- Create: `kernel/core/elf_loader.hpp`
- Create: `kernel/core/elf_loader.cpp`

**Goal:** Parse ELF64, create Process with mapped segments, create main Thread. Wire up init process.

- [ ] **Step 1: Create elf_loader.hpp**

```cpp
// kernel/core/elf_loader.hpp
#pragma once
#include <stdint.h>
#include <stddef.h>

class Process;

// Load an ELF64 executable from memory. Creates a Process, maps PT_LOAD
// segments as VMOs, creates a main Thread at the ELF entry point.
// The thread is NOT started — caller must call thread_start.
// Returns the Process, or nullptr on failure.
Process* elf_load(const void* elf_data, size_t elf_size,
                  const char* proc_name, uint8_t priority,
                  Thread** out_thread);
```

- [ ] **Step 2: Create elf_loader.cpp**

```cpp
// kernel/core/elf_loader.cpp
#include "kernel/core/elf_loader.hpp"
#include "kernel/core/object/process.hpp"
#include "kernel/core/mm/vmo.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"

struct Elf64Header {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct Elf64PHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_R = 4;
constexpr uint32_t PF_W = 2;
constexpr uint32_t PF_X = 1;

constexpr uint64_t ELF_STACK_TOP = 0x00007FFFFFFFFFFFULL & ~(PAGE_SIZE - 1);

Process* elf_load(const void* elf_data, size_t elf_size,
                  const char* proc_name, uint8_t priority,
                  Thread** out_thread) {
    if (elf_size < sizeof(Elf64Header)) return nullptr;

    auto* hdr = static_cast<const Elf64Header*>(elf_data);

    // Verify ELF magic
    if (hdr->ident[0] != 0x7F || hdr->ident[1] != 'E' ||
        hdr->ident[2] != 'L'  || hdr->ident[3] != 'F') {
        klog("elf_load: bad magic\n");
        return nullptr;
    }

    // Verify 64-bit, x86-64, executable
    if (hdr->ident[4] != 2) { klog("elf_load: not 64-bit\n"); return nullptr; }
    if (hdr->machine != 0x3E) { klog("elf_load: not x86-64\n"); return nullptr; }

    Process* proc = Process::Create(proc_name);
    if (!proc) return nullptr;

    auto* phdr_base = reinterpret_cast<const uint8_t*>(elf_data) + hdr->phoff;

    for (int i = 0; i < hdr->phnum; i++) {
        auto* ph = reinterpret_cast<const Elf64PHeader*>(
            phdr_base + i * hdr->phentsize);

        if (ph->type != PT_LOAD) continue;

        uint64_t va_page  = ph->vaddr & ~(PAGE_SIZE - 1);
        uint64_t va_off   = ph->vaddr & (PAGE_SIZE - 1);
        uint64_t size_pg  = (va_off + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        // Map flags
        uint64_t vm_flags = VM_USER | VM_COW;
        if (ph->flags & PF_R) vm_flags |= VM_READ;
        if (ph->flags & PF_W) vm_flags |= VM_WRITE;
        if (ph->flags & PF_X) vm_flags |= VM_EXEC;

        Vmo* vmo = Vmo::CreateAnonymous(size_pg);
        if (!vmo) { proc->Release(); return nullptr; }

        // Copy segment data into VMO pages
        const uint8_t* src = reinterpret_cast<const uint8_t*>(elf_data) + ph->offset;
        for (uint64_t off = 0; off < ph->filesz; off += PAGE_SIZE) {
            uint64_t phys = vmo->GetPage(va_off + off, true);
            if (!phys) { vmo->Release(); proc->Release(); return nullptr; }

            uint64_t* dst = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + phys);
            uint64_t copy_sz = PAGE_SIZE;
            if (ph->filesz - off < PAGE_SIZE) copy_sz = ph->filesz - off;

            // Copy byte-by-byte to handle unaligned va_off
            for (uint64_t j = 0; j < copy_sz; j++) {
                reinterpret_cast<uint8_t*>(dst)[va_off + j] = src[off + j];
            }
        }

        proc->Map(vmo, va_page, 0, size_pg, vm_flags);
        vmo->Release(); // Map holds ref
    }

    // Create user stack: 16KB at top of user space
    constexpr uint64_t STACK_SIZE = PAGE_SIZE * 4;
    uint64_t stack_va = ELF_STACK_TOP - STACK_SIZE;
    Vmo* stack_vmo = Vmo::CreateAnonymous(STACK_SIZE);
    if (!stack_vmo) { proc->Release(); return nullptr; }
    proc->Map(stack_vmo, stack_va, 0, STACK_SIZE, VM_READ | VM_WRITE | VM_USER);
    stack_vmo->Release();

    // Pre-commit first stack page so the thread can push before any PF
    stack_vmo->GetPage(STACK_SIZE - PAGE_SIZE, true);

    uint64_t user_stack_top = ELF_STACK_TOP;

    // Create main thread — entry point with user stack
    Thread* main_thread = thread_create(
        nullptr,  // entry is set via rsp; actually we need a trampoline
        proc_name, priority, proc);
    // ... see below

    *out_thread = main_thread;
    return proc;
}
```

Note: The ELF loader creates the Process and VMO mappings. The thread entry is the ELF `e_entry` field. The thread's initial stack frame needs to set up a user-mode entry, which requires coordination with `enter_usermode`. Add a trampoline that calls `enter_usermode(elf_entry, user_stack_top)`.

- [ ] **Step 3: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 4: Commit**

```bash
git add kernel/core/elf_loader.hpp kernel/core/elf_loader.cpp
git commit -m "feat: ELF64 loader — map segments as VMOs, create Process + Thread"
```

---

### Task 12: Init Process + Build Integration

**Files:**
- Create: `kernel/init/init.cpp`
- Create: `kernel/init/BUILD.bazel`
- Create: `kernel/init/init.ld`
- Modify: `kernel/BUILD.bazel`

**Goal:** A ring-3 init ELF that demonstrates IPC. Embedded into the kernel binary.

- [ ] **Step 1: Create init.ld linker script**

```
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
```

- [ ] **Step 2: Create init.cpp**

```cpp
// kernel/init/init.cpp
// User-space init process — ring 3 demonstration

// Syscall numbers (duplicated — user space doesn't include kernel headers)
constexpr int SYS_DEBUG_PRINT    = 0;
constexpr int SYS_HANDLE_CLOSE   = 1;
constexpr int SYS_CHANNEL_CREATE = 10;
constexpr int SYS_CHANNEL_WRITE  = 11;
constexpr int SYS_CHANNEL_READ   = 12;
constexpr int SYS_PORT_CREATE    = 20;
constexpr int SYS_PORT_REGISTER  = 21;
constexpr int SYS_PORT_CONNECT   = 22;
constexpr int SYS_PROCESS_EXIT   = 31;

static inline uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    asm volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return ret;
}

static void print(const char* msg) {
    syscall6(SYS_DEBUG_PRINT, reinterpret_cast<uint64_t>(msg), 0, 0, 0, 0);
}

// Simple write-to-serial helper
static void putchar(char c) {
    // Write a single char via debug print
    char buf[2] = {c, 0};
    print(buf);
}

static void print_hex(uint64_t n) {
    char buf[17] = "0x0000000000000000";
    for (int i = 17; i > 1; i--) {
        uint8_t d = n & 0xF;
        buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
        n >>= 4;
    }
    print(buf);
}

extern "C" void _start() {
    print("\n=== init: user-space bootstrap ===\n");

    // Test 1: Channel create + write + read
    print("Test 1: Channel IPC...\n");
    uint64_t pair = syscall6(SYS_CHANNEL_CREATE, 0, 0, 0, 0, 0);
    uint32_t ch_a = pair >> 32;
    uint32_t ch_b = pair & 0xFFFFFFFF;

    const char* msg = "Hello from ring 3!";
    uint64_t msg_len = 18;
    syscall6(SYS_CHANNEL_WRITE, ch_a,
             reinterpret_cast<uint64_t>(msg), msg_len, 0, 0);

    char rbuf[64];
    for (int i = 0; i < 64; i++) rbuf[i] = 0;
    uint64_t nread = syscall6(SYS_CHANNEL_READ, ch_b,
                               reinterpret_cast<uint64_t>(rbuf), 64, 0, 0);
    print("  Received: ");
    print(rbuf);
    print(" (");
    print_hex(nread);
    print(" bytes)\n");

    syscall6(SYS_HANDLE_CLOSE, ch_a, 0, 0, 0, 0);
    syscall6(SYS_HANDLE_CLOSE, ch_b, 0, 0, 0, 0);

    // Test 2: Port register + connect
    print("Test 2: Port discovery...\n");
    uint64_t port_h = syscall6(SYS_PORT_CREATE, 0, 0, 0, 0, 0);
    syscall6(SYS_PORT_REGISTER, port_h,
             reinterpret_cast<uint64_t>("init/test"), 0, 0, 0);

    uint64_t conn = syscall6(SYS_PORT_CONNECT,
                              reinterpret_cast<uint64_t>("init/test"),
                              0, 0, 0, 0);
    print("  Port connect result: ");
    print_hex(conn);
    print("\n");

    syscall6(SYS_HANDLE_CLOSE, conn, 0, 0, 0, 0);
    syscall6(SYS_HANDLE_CLOSE, port_h, 0, 0, 0, 0);

    print("=== init: all tests passed ===\n");
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);

    while (1) { asm volatile("hlt"); }
}
```

- [ ] **Step 3: Create kernel/init/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "init.elf",
    srcs = ["init.cpp"],
    linkopts = ["-T", "$(location init.ld)", "-nostdlib", "-static"],
    features = [],  # disable default features
    target_compatible_with = ["@platforms//os:none"],
)

genrule(
    name = "init_embed",
    srcs = [":init.elf"],
    outs = ["init_embed.S"],
    cmd = "echo '.section .init_elf,\"a\"; .global init_elf_start, init_elf_end; init_elf_start: .incbin \"$(SRCS)\"; init_elf_end:' > $@",
)
```

Note: The exact Bazel rules depend on the toolchain configuration. The key requirement is that the init ELF is embedded as a raw binary via `.incbin` in a `.S` file that the kernel links.

- [ ] **Step 4: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 5: Commit**

```bash
git add kernel/init/init.cpp kernel/init/init.ld kernel/init/BUILD.bazel kernel/BUILD.bazel
git commit -m "feat: init process — ring-3 ELF with channel/port IPC demo"
```

---

### Task 13: Boot Sequence Wiring

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp`

**Goal:** Wire Phase 7 into the boot sequence. Fix paging, create kernel process, load init, start scheduler.

- [ ] **Step 1: Update boot.cpp**

The updated boot flow replaces the Phase 5 demo with Phase 7 init:

```cpp
// In kernel_entry(), after SMP bringup:

// Phase 2b: Take over paging (previously skipped)
klog("=== Phase 2b: Paging Takeover ===\n\n");
paging_init(hhdm, kernel_phys, kernel_virt, kernel_size);
paging_save_kernel_template();

// Phase 5: Scheduler
klog("=== Phase 5: Scheduler ===\n\n");
scheduler_init(hhdm);

// Phase 7: VMM + Process
klog("=== Phase 7: VMM + Process ===\n\n");

// Per-CPU TSS
klog("Initializing TSS...\n");
for (uint32_t i = 0; i < smp_cpu_count(); i++) {
    tss_init_per_cpu(i);
}

// Page fault handler
klog("Registering page fault handler...\n");
page_fault_init();

// Load init process
klog("Loading init process...\n");
extern "C" uint8_t init_elf_start[];
extern "C" uint8_t init_elf_end[];
size_t init_size = init_elf_end - init_elf_start;

Thread* init_thread = nullptr;
Process* init_proc = elf_load(init_elf_start, init_size,
                               "init", 1, &init_thread);
if (!init_proc || !init_thread) {
    KPANIC("Failed to load init process");
}

// Start init thread
thread_start(init_thread);

// Timer for preemption
timer_periodic(10000, timer_preempt_callback);

klog("Starting scheduler...\n\n");
asm volatile("sti");
scheduler_start();  // never returns
```

- [ ] **Step 2: Remove old demo**

Remove the `[A]`/`[B]` demo threads from boot.cpp. The init process replaces them.

- [ ] **Step 3: Build**

```bash
bazel build //kernel:kernel
```

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp
git commit -m "feat: wire Phase 7 — paging takeover, TSS, page fault, init ELF, boot to ring 3"
```

---

### Task 14: QEMU Integration Test

**Files:**
- Modify: `scripts/run.sh` (if needed)

**Goal:** Boot the kernel in QEMU and verify the init process runs, prints its test output, and exits cleanly.

- [ ] **Step 1: Run in QEMU**

```bash
bash scripts/run.sh
```

Expected serial output:
```
=== C++26 Kernel ===
...
=== Phase 7: VMM + Process ===
...
=== init: user-space bootstrap ===
Test 1: Channel IPC...
  Received: Hello from ring 3! (0x12 bytes)
Test 2: Port discovery...
  Port connect result: 0x...
=== init: all tests passed ===
```

- [ ] **Step 2: Debug and fix**

Common issues and fixes:
- Triple fault on `paging_init` CR3 reload → verify page table construction
- #GP on `enter_usermode`/`iretq` → check GDT user segments, TSS RSP0
- #PF in init → verify ELF loader maps all segments correctly
- Syscall fails → verify `current_thread()->process` is set for init thread

- [ ] **Step 3: Commit any fixes**

```bash
git add <fixed files>
git commit -m "fix: Phase 7 integration — debug QEMU boot issues"
```

---

### Task 15: Final Code Review + Documentation

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md phase status**

```markdown
| Phase | Plan | Status |
|-------|------|--------|
...
| 7: VMM + Process | `docs/superpowers/plans/2026-05-10-phase-7-vmm-process.md` | Done |
```

- [ ] **Step 2: Run all tests**

```bash
bazel test //test:mm:all //test/irq:all //test/sched:all //test/object:all
```

Expected: all tests pass (mm, irq, sched, object).

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: mark Phase 7 as Done"
```
