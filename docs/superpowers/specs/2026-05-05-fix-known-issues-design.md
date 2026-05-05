# Fix Known Issues Design

## Overview

Fix three deferred issues from Phases 1-2: paging_init CR3 reload triple-fault, buddy allocator integration, and TSS I/O permission bitmap.

## Fix 1: paging_init CR3 reload triple-fault

### Problem

`paging_init` builds a temporary PML4 with only the kernel mapping (0xFFFFFFFF80000000→phys). When CR3 is loaded with this PML4, the CPU triple-faults because Limine's identity mapping (0x0→0x0) and HHDM mapping (0xFFFF800000000000→0x0) are missing. Any HHDM-based memory access (kmalloc, stack-relative to physical pages) immediately page-faults.

### Fix

Copy all entries from Limine's PML4 into the temporary PML4 before building the kernel mapping, then overwrite only the kernel PML4 entry. This preserves identity and HHDM mappings. CR3 reload is then safe.

```cpp
// In paging_init, after alloc_table():
PageTable* tmp_pml4 = alloc_table();
uint64_t* tmp_virt = static_cast<uint64_t*>(early_phys_to_virt(
    reinterpret_cast<uint64_t>(tmp_pml4)));

// Copy ALL Limine mappings (identity, HHDM, everything)
for (int i = 0; i < 512; i++) {
    tmp_virt[i] = pml4->entries[i];
}

// Overwrite kernel entry with our mapping
uint16_t ki4 = pml4_index(kernel_virt_base);
tmp_virt[ki4] = /* our kernel pml4 entry */;

// Safe CR3 reload
asm volatile("mov %0, %%cr3" :: "r"(reinterpret_cast<uint64_t>(tmp_pml4)) : "memory");
```

The previous in-place PML4 entry swap (without CR3 reload) is replaced by the full page table switch.

### Files
- Modify: `kernel/arch/x86_64/paging.cpp`

---

## Fix 2: Buddy allocator integration

### Problem

Buddy allocator is implemented and host-tested but never initialized in the kernel boot sequence. Slab allocator uses `bitmap_alloc_page()` directly for slab page allocation, bypassing buddy entirely.

### Fix

1. Call `buddy_init(hhdm, 0)` in boot.cpp after paging_init and before slab_init.
2. Replace `bitmap_alloc_page()` with `buddy_alloc_pages(0)` in slab.cpp's `slab_create()`.
3. Add a boot log for buddy initialization.

Bitmap allocator continues to be used during paging_init (before buddy is available) for temporary page table allocation. The two allocators form a natural handoff: bitmap→early boot, buddy→post-boot.

### Files
- Modify: `kernel/arch/x86_64/boot.cpp`
- Modify: `kernel/core/mm/slab.cpp`

---

## Fix 3: TSS I/O permission bitmap

### Problem

GDT entries 5 and 6 (TSS low/high) are placeholders. No TSS is initialized. Without TSS, there is no I/O permission bitmap, no IST (Interrupt Stack Table) for critical interrupts, and no ring 0 RSP for user→kernel transitions (needed for Phase 7 syscall support).

### Fix

Add a static TSS structure with I/O permission bitmap. The TSS is aligned to PAGE_SIZE. The I/O bitmap is 8192 bytes (65536 ports), all set to 0xFF (deny I/O from ring 3). A termination byte (0xFF) follows the bitmap per Intel spec.

```cpp
// TSS layout (x86-64)
struct TSS {
    uint32_t reserved0;
    uint64_t rsp0;   // Ring 0 stack pointer (for syscall/interrupt from ring 3)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7]; // Interrupt Stack Table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_bitmap_offset;
} __attribute__((packed));

// Static storage: TSS (104 bytes) + 8192-byte I/O bitmap + 1 termination byte
static uint8_t g_tss_data[sizeof(TSS) + 8192 + 1]
    __attribute__((aligned(PAGE_SIZE)));

void tss_init() {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    memset(tss, 0, sizeof(TSS));
    uint16_t bitmap_offset = sizeof(TSS);  // I/O bitmap starts right after TSS
    tss->io_bitmap_offset = bitmap_offset;
    uint8_t* bitmap = &g_tss_data[bitmap_offset];
    memset(bitmap, 0xFF, 8192);  // Deny all I/O from ring 3 by default
    g_tss_data[sizeof(g_tss_data) - 1] = 0xFF;  // Termination byte

    uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);

    set_gdt_entry(5, tss_addr & 0xFFFFFFFF,
                  sizeof(g_tss_data) - 1, 0x89, 0);   // TSS low
    set_gdt_entry(6, (tss_addr >> 32) & 0xFFFFFFFF,
                  0, 0, 0);                            // TSS high

    asm volatile("ltr %%ax" :: "a"(0x28));  // 0x28 = GDT selector for entry 5
}
```

Call `tss_init()` in boot.cpp after GDT init.

### Files
- Modify: `kernel/arch/x86_64/gdt.cpp` — add tss_init()
- Modify: `kernel/arch/x86_64/gdt.hpp` — declare tss_init() and export set_entry (or make it accessible)
- Modify: `kernel/arch/x86_64/boot.cpp` — call tss_init() after gdt_init()

---

## Build and Boot Verification

After all three fixes:
```bash
bazel build //kernel:kernel
bash scripts/run.sh
```

Expected: kernel boots without triple-fault, buddy and TSS init messages appear in log, Phase 3-6 demos continue to work.

---

## Dependencies

- **Incoming**: Phase 1 (GDT, IDT, paging), Phase 2 (bitmap, buddy, slab)
- **Outgoing**: None — these are standalone fixes
