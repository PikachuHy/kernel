# Phase 7: VMM + Process Objects — Design Spec

## Overview

Full Virtual Memory Manager with VMOs and demand paging, Process objects with per-process handle tables, ELF loading, and ring-3 user-space bootstrap. This phase bridges the kernel from pure ring-0 execution to hosting user processes.

## Current State (post-Phase 6)

- Page tables: using Limine's PML4 directly; `paging_init` exists but has CR3-reload bugs
- Handle table: global 1024-entry, spinlock-protected, free-list allocation
- Thread: standalone execution unit, no process parent
- Syscall: `syscall`/`sysret` wired (LSTAR, STAR, SFMASK), but everything runs in ring 0
- No user mode, no per-process address spaces, no VMOs

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Phase 7: VMM + Process Objects                               │
│                                                              │
│  ┌──────────┐    ┌──────────────┐    ┌──────────────────┐   │
│  │ Process  │───▶│ HandleTable  │    │ VmRegion list    │   │
│  │ (object) │    │ (per-proc)   │    │ sorted by VA     │   │
│  │          │───▶│ PML4*        │    │ → VMO + offset   │   │
│  │          │───▶│ Thread list  │    │ → flags (RWX)    │   │
│  └──────────┘    └──────────────┘    └────────┬─────────┘   │
│                                               │             │
│  ┌──────────┐    ┌──────────────┐    ┌────────▼─────────┐   │
│  │ Thread   │───▶│ Process*     │    │ Vmo (object)     │   │
│  │ (has     │    │ (parent)     │    │ type: Anon/Phys  │   │
│  │  proc*)  │    └──────────────┘    │ CowPage array    │   │
│  └──────────┘                        │ size, cow state  │   │
│                                      └──────────────────┘   │
│                                                              │
│  ┌──────────────┐    ┌──────────────┐                       │
│  │ ELF Loader   │    │ Page Fault   │                       │
│  │ parse ELF64  │    │ #PF handler  │                       │
│  │ map segments │    │ demand page  │                       │
│  │ create proc  │    │ COW resolve  │                       │
│  └──────────────┘    └──────────────┘                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 1. Process Object

**File:** `kernel/core/object/process.hpp`, `kernel/core/object/process.cpp`

`Process` inherits `KernelObject`. It is the unit of resource ownership — every Thread belongs to a Process, every Process has its own address space and handle table.

### Data Layout

```cpp
class Process : public KernelObject {
public:
    static Process* Create(const char* name);

    // Address space
    uint64_t       pml4_phys;      // CR3 value for this process
    VmRegion*      regions;        // sorted intrusive linked list (by base_va)

    // Handle table (inline, 1024 entries → ~16KB per process)
    HandleTable    handles;

    // Threads belonging to this process
    Thread*        threads;        // intrusive linked list via Thread::proc_next

    // Hierarchy (for init→child relationship; full process tree deferred)
    Process*       parent;

    char           name[32];

    // Page fault handler — looks up region, allocates page, installs PTE
    bool HandlePageFault(uint64_t fault_addr, bool was_write);

    // VMM operations
    bool Map(Vmo* vmo, uint64_t va, uint64_t offset, uint64_t size, uint64_t flags);
    bool Unmap(uint64_t va, uint64_t size);
    VmRegion* FindRegion(uint64_t va);

    // Thread management
    void AddThread(Thread* t);
    void RemoveThread(Thread* t);
};
```

### Kernel Process

A singleton `Process::Create("kernel")` is created during boot. The BSP boot thread and all AP idle threads belong to it. The kernel process has no user-space mappings; its PML4 is the shared kernel-half template.

### Lifecycle

- **Create:** allocate `Process` via `kmalloc`, init handle table, allocate fresh PML4 from bitmap, copy kernel-half entries from template
- **Destroy:** on last `Release()`: free all VmRegions, free handle table entries, free PML4 pages recursively, free Thread stacks
- Reference counting via `KernelObject::AddRef/Release`

---

## 2. Handle Table (Per-Process)

**File:** `kernel/core/object/handle_table.hpp`, `kernel/core/object/handle_table.cpp`

The current global `g_table`, `g_free_head`, `g_lock` are extracted into a `HandleTable` class. Each Process owns one inline.

### Interface

```cpp
class HandleTable {
public:
    void      Init();
    handle_t  Alloc(KernelObject* obj, Rights rights);
    void      Free(handle_t h);
    KernelObject* Lookup(handle_t h, Rights needed = Rights{}, Rights* out_rights = nullptr);

private:
    HandleEntry entries[MAX_HANDLES];
    handle_t    free_head;
    SpinLock    lock;
};
```

### Migration from Global

| Before (Phase 6) | After (Phase 7) |
|---|---|
| `handle_alloc(obj, rights)` | `proc->handles.Alloc(obj, rights)` |
| `handle_free(h)` | `proc->handles.Free(h)` |
| `handle_lookup(h, needed, &r)` | `proc->handles.Lookup(h, needed, &r)` |
| `handle_table_init()` in `syscall_init` | `Process::Create` calls `handles.Init()` |

### Syscall Impact

All syscall handlers resolve handles from `current_thread()->process->handles`. The `handle_lookup` free function becomes a thin wrapper for backward compatibility during the transition, then removed.

### Kernel Process Handles

The kernel process can hold handles to any object. Syscalls from user processes can only use handles in their own table. Handles can be transferred between processes via Channel messages (existing handle-passing in `Channel::Write/Read`).

---

## 3. Virtual Memory Object (VMO)

**File:** `kernel/core/mm/vmo.hpp`, `kernel/core/mm/vmo.cpp`

A VMO represents a logical memory region — the unit of memory that can be mapped into address spaces. VMOs are ref-counted `KernelObject` instances and can be shared between processes.

### Types

| Type | Purpose |
|------|---------|
| `Anonymous` | Zero-filled pages allocated on first access. Supports COW cloning |
| `Physical` | Contiguous pre-allocated physical memory (framebuffer, DMA) |

File-backed VMOs are deferred to the VFS phase.

### Data Layout

```cpp
struct CowPage {
    uint64_t phys_addr;      // 0 = not yet committed
    uint32_t cow_refs;       // # of VMOs sharing this page (1 = private)
};

class Vmo : public KernelObject {
public:
    enum Type : uint8_t { Anonymous, Physical };

    static Vmo* CreateAnonymous(uint64_t size);   // size in bytes, page-aligned
    static Vmo* CreatePhysical(uint64_t size, uint64_t phys_base);

    // Demand-page: returns phys_addr for offset. May allocate (Anonymous) or
    // COW-resolve. for_write=true triggers COW copy if page is shared.
    uint64_t GetPage(uint64_t offset, bool for_write);

    // COW clone: creates child sharing all committed pages. cow_refs incremented.
    Vmo* CloneCoW();

    uint64_t size()     const { return size_; }
    uint64_t num_pages() const { return num_pages_; }
    Type     type()     const { return type_; }

private:
    Type         type_;
    uint64_t     size_;
    uint64_t     num_pages_;
    CowPage**    pages_;          // array[num_pages_], nullptr entries = not committed
    SpinLock     lock_;
};
```

### Anonymous VMO Lifecycle

1. **Create:** allocate `pages_` array (all nullptr). No physical pages allocated.
2. **GetPage(offset, read):** if `pages_[i]` is null, allocate zero-filled 4KB page via `bitmap_alloc_page`, set `cow_refs=1`.
3. **GetPage(offset, write):** same as read, but if `cow_refs > 1`, allocate new page, memcpy old→new, decrement old cow_refs, set new cow_refs=1.
4. **CloneCoW:** create child VMO with same `num_pages_`. For each committed page in parent: `child->pages_[i] = parent->pages_[i]`, increment `cow_refs`.
5. **Destroy:** for each committed page: decrement `cow_refs`. If zero, free phys page + `CowPage`.

### Physical VMO

For hardware-backed memory (framebuffer, ACPI tables, MMIO). Pre-allocated; `GetPage` just returns the pre-computed phys_addr. No COW support.

---

## 4. Virtual Memory Manager (Address Space)

**File:** `kernel/core/mm/vmm.hpp`, `kernel/core/mm/vmm.cpp`

Per-process virtual memory region tracking and page table management.

### VmRegion

```cpp
struct VmRegion {
    VmRegion*  next;         // intrusive linked list, sorted by base_va ascending
    uint64_t   base_va;
    uint64_t   size;         // page-aligned
    Vmo*       vmo;
    uint64_t   vmo_offset;   // offset within VMO where this mapping starts
    uint64_t   flags;        // VM_READ, VM_WRITE, VM_EXEC, VM_COW, VM_USER
};

constexpr uint64_t VM_READ   = 1 << 0;
constexpr uint64_t VM_WRITE  = 1 << 1;
constexpr uint64_t VM_EXEC   = 1 << 2;
constexpr uint64_t VM_COW    = 1 << 3;  // write-fault triggers COW copy
constexpr uint64_t VM_USER   = 1 << 4;  // accessible from ring 3
```

### Operations

```cpp
// Map a portion of a VMO into the address space at `va`.
// Returns false if the range overlaps an existing region.
bool Process::Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
                  uint64_t size, uint64_t flags);

// Unmap a range. Frees page table pages if a whole table becomes empty.
bool Process::Unmap(uint64_t va, uint64_t size);

// Find the VmRegion containing `va`, or nullptr.
VmRegion* Process::FindRegion(uint64_t va);
```

### Address Space Layout

```
0x0000000000000000 ─────────────────── user space (ring 3)
                    │ .text, .data   │
                    │ stack          │
                    │ mmap regions   │
0x00007FFFFFFFFFFF ─────────────────── user space end
                    /// gap ///
0xFFFF800000000000 ─────────────────── DIRECT_MAP_BASE (direct map of all phys mem)
0xFFFFFFFF80000000 ─────────────────── KERNEL_VIRT_BASE (kernel image)
```

### PML4 Management

A fresh PML4 for a user process is created by:
1. Allocate PML4 page via `bitmap_alloc_page`
2. Copy kernel-half entries (indices 256-511) from a kernel template PML4
3. User-half entries (indices 0-255) start all zero

This ensures all processes share the kernel address space while having isolated user spaces. The kernel template is the Limine PML4 (or a copy of it) saved at boot.

---

## 5. Page Fault Handler

**File:** `kernel/arch/x86_64/page_fault.cpp`

Registered as ISR vector 14 (#PF). Reads CR2 for the faulting address.

### Flow

```
page_fault_handler(error_code, cr2):
    1. If fault is in kernel space (cr2 >= 0xFFFF800000000000):
       - Kernel page fault → KPANIC (shouldn't happen; all kernel pages pre-mapped)
    
    2. Process* proc = current_thread()->process
       If no process → KPANIC (idle thread shouldn't page fault)
    
    3. VmRegion* region = proc->FindRegion(cr2)
       If no region → kill process (unmapped access)
    
    4. Check permissions:
       - If write to read-only, non-COW mapping → kill process
       - If exec to no-exec mapping → kill process
    
    5. uint64_t vmo_off = (cr2 - region->base_va) + region->vmo_offset
       bool is_cow_write = (error_code & 0x2) && (region->flags & VM_COW)
       uint64_t phys = region->vmo->GetPage(vmo_off, is_cow_write)
    
    6. If is_cow_write and page was shared → 
       - COW resolved: page is now private. Clear VM_COW on the PTE flags
       - Mark PTE writable
    
    7. Install PTE:
       page_table_walk(proc->pml4_phys, cr2, phys, 
           PageFlags::Present | PageFlags::User |
           (region->flags & VM_WRITE ? PageFlags::Writable : 0) |
           (region->flags & VM_EXEC  ? 0 : PageFlags::NoExec))
    
    8. invlpg cr2
       return (iretq is in the common ISR stub)
```

### Page Table Walker

A recursive function that walks PML4→PDPT→PD→PT, allocating intermediate page tables via `bitmap_alloc_page` as needed. Each intermediate table is zeroed before use.

```cpp
bool page_table_walk(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags);
```

---

## 6. Thread → Process Association

**File:** `kernel/core/sched/sched.hpp`, `kernel/core/sched/sched.cpp`, `kernel/core/sched/switch.S`

### Thread Changes

```cpp
struct Thread {
    // ... existing fields unchanged ...
    Process*   process;     // owning process (nullptr for idle threads)
    Thread*    proc_next;   // next thread in process->threads list
};
```

### Context Switch with CR3

`switch_to` is extended to reload CR3 when crossing process boundaries:

```asm
switch_to:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)          # prev->rsp = rsp

    # Check process boundary
    movq 104(%rdi), %rax       # prev->process
    movq 104(%rsi), %rcx       # next->process
    cmpq %rax, %rcx
    je   1f                    # same process, skip CR3
    movq (%rcx), %rdx          # next->process->pml4_phys (offset 0 = pml4_phys)
    addq $16, %rdx             # skip KernelObject vtable + refcount to pml4_phys
    # Actually need correct offset — compute from Process layout
    movq %rdx, %cr3
1:
    movq (%rsi), %rsp          # rsp = next->rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq
    ret
```

**Important subtlety:** The CR3 reload must happen *after* saving prev's state and *before* loading next's state. The `addq $16, %rdx` accounts for the KernelObject vtable pointer and ref_count at the start of Process. The exact offset of `pml4_phys` within Process must be validated via `static_assert`.

### Idle Threads

Idle threads have `process = nullptr`. They never page-fault (only run `sti; hlt; cli` in kernel space). `switch_to` handles the nullptr case by skipping CR3 reload.

---

## 7. TSS: Ring-0 Stack for Interrupts

**File:** `kernel/arch/x86_64/tss.hpp`, `kernel/arch/x86_64/tss.cpp`, `kernel/arch/x86_64/gdt.cpp`

When a user-space thread traps into the kernel (interrupt, exception, syscall), the CPU loads RSP from the TSS. Each CPU needs `RSP0` pointing to a per-CPU kernel stack.

### TSS Layout

```cpp
struct Tss {
    uint32_t reserved0;
    uint64_t rsp0;   // ring 0 stack pointer (loaded on interrupt from ring 3)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;   // IST pointers (optional, for NMI/MCE/DF)
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
    // I/O permission bitmap follows
};
```

### Per-CPU Kernel Stacks

Each CPU gets a dedicated kernel interrupt stack (16KB). On thread creation:
- Thread gets a user stack VMO (4KB, VM_READ|VM_WRITE|VM_USER)
- Thread's `rsp` (initial) points to the **kernel** switch stack, same as now
- The TSS `rsp0` for each CPU points to the top of the per-CPU interrupt stack

When a thread is running in user space and an interrupt fires:
1. CPU loads RSP from TSS.RSP0 → kernel interrupt stack
2. ISR stub pushes registers onto this stack
3. ISR may context-switch (timer) → saves state to Thread::rsp
4. On `iretq` back to user: CPU restores user RSP from the IRET frame

### Boot-Time Setup

```
tss_init_per_cpu(cpu_id):
    Allocate 16KB kernel interrupt stack via bitmap
    g_per_cpu[cpu_id].tss.rsp0 = stack_top_virt
    Load TSS via LTR
```

---

## 8. ELF Loader

**File:** `kernel/core/elf_loader.hpp`, `kernel/core/elf_loader.cpp`

Parses ELF64 executables and creates a Process with the segments mapped.

### Interface

```cpp
// Load an ELF from a memory buffer. Creates Process, maps segments, creates
// main Thread with entry = elf->entry. Returns the Process.
// On failure, returns nullptr.
Process* elf_load(const void* elf_data, size_t elf_size,
                  const char* proc_name, uint8_t priority);
```

### Flow

```
elf_load(data, size, name, priority):
    1. Verify ELF magic: EI_MAG0..3 = 0x7F,'E','L','F'
    2. Verify class = ELFCLASS64, machine = EM_X86_64
    3. Create Process via Process::Create(name)
    4. For each PT_LOAD program header:
       a. page-align p_vaddr and p_memsz
       b. Create Anonymous VMO of page-aligned size
       c. flags = (PF_R→VM_READ | PF_W→VM_WRITE | PF_X→VM_EXEC) | VM_USER
       d. proc->Map(vmo, page_va, 0, vmo_size, flags)
       e. Copy segment data from ELF buffer into VMO pages via HHDM:
          - For each page of p_filesz: vmo->GetPage(i, true), memcpy via HHDM
       f. vmo->Release() (mapping holds ref)
    5. Create user stack VMO (16KB, Anonymous)
    6. uint64_t stack_top = USER_STACK_TOP (just below kernel boundary)
    7. proc->Map(stack_vmo, stack_top - 16KB, 0, 16KB, VM_READ|VM_WRITE|VM_USER)
    8. Create Thread via thread_create, set thread->process = proc
    9. Set thread's initial rsp to stack_top (with return-to-thread_exit frame)
    10. Set thread's initial rip to elf->entry
    11. proc->AddThread(thread)
    12. return proc
```

### ELF64 Header

Only the minimal fields needed are parsed:
- `e_entry` — entry point VA
- `e_phoff`, `e_phnum`, `e_phentsize` — program headers
- `PT_LOAD` segments: `p_vaddr`, `p_memsz`, `p_filesz`, `p_offset`, `p_flags`

---

## 9. Ring 3 Entry

**File:** `kernel/arch/x86_64/usermode.S`, `kernel/arch/x86_64/usermode.hpp`

### enter_usermode

Jumps from ring 0 to ring 3 using `iretq` with a constructed stack frame:

```asm
enter_usermode:
    # rdi = entry_rip (user-space instruction pointer)
    # rsi = user_rsp  (top of user stack)
    # rdx = user_rdi  (argument to pass in %rdi, for init process argc-like)

    # Switch to user data segments
    movw $0x23, %ax    # user data segment (GDT index 4 | RPL 3)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Build IRET frame on stack
    pushq $0x23        # SS = user data selector
    pushq %rsi         # RSP = user stack top
    pushfq             # RFLAGS (IF already set from scheduler_start)
    pushq $0x1B        # CS = user code selector (GDT index 3 | RPL 3)
    pushq %rdi         # RIP = entry point

    # Move user arg into rdi before iret
    movq %rdx, %rdi

    iretq
```

### GDT User Segments

The existing GDT must have user-mode selectors:
- Index 3: user code (64-bit, ring 3), selector 0x18|3 = 0x1B
- Index 4: user data (ring 3), selector 0x20|3 = 0x23

If not present, they are added during `gdt_init()`.

---

## 10. Init Process

**File:** `kernel/init/init.cpp`

A simple but capable user-space init program that exercises the new syscalls.

### Capabilities

```cpp
// init process runs in ring 3 and demonstrates:
// 1. Debug print via syscall
// 2. Channel create, write, read
// 3. Port create, register, connect
// 4. Spawn child process (stretch goal: process_create + thread_start)
// 5. Service discovery via port namespace
```

### Build Integration

The init ELF is compiled as a separate freestanding binary (`kernel/init/BUILD.bazel`) and embedded into the kernel via a linker symbol, similar to how kernels embed initramfs:

```
kernel/init:init_elf → objcopy to raw binary → .incbin in kernel or linked via
a generated .o file that exports _binary_init_elf_start / _binary_init_elf_end
```

The kernel accesses it as:
```cpp
extern "C" uint8_t _binary_init_elf_start[];
extern "C" uint8_t _binary_init_elf_end[];
size_t size = _binary_init_elf_end - _binary_init_elf_start;
Process* init = elf_load(_binary_init_elf_start, size, "init", 1);
```

---

## 11. Boot Sequence Update

**File:** `kernel/arch/x86_64/boot.cpp`

Updated boot flow after Phase 7:

```
Phase 1: GDT, IDT
Phase 2: PMM, bitmap, slab
Phase 3: APIC, timer, IRQ, syscall
Phase 4: SMP bringup
Phase 5: Scheduler init
Phase 6: (handle table init now per-process)
Phase 7:
  1. paging_init(hhdm, ...) — take over page tables (fix CR3 bug)
  2. Save kernel PML4 template
  3. Process* kernel_proc = Process::Create("kernel")
  4. Update BSP thread → kernel_proc
  5. Register #PF handler (vector 14)
  6. Initialize per-CPU TSS with RSP0
  7. Load init ELF, create init process
  8. thread_start(init_main_thread)
  9. timer_periodic(10000, preempt_callback)
  10. scheduler_start() — never returns
```

---

## 12. Fix: paging_init CR3 Reload

The existing `paging_init` causes triple-fault on CR3 reload. The root cause is likely that intermediate page table pages aren't being properly zeroed or mapped through the HHDM during construction. This is a pre-requisite for Phase 7 since we need per-process page tables.

The fix approach:
- Ensure `alloc_table()` zeroes all 512 entries (already done)
- Verify the kernel-half PML4 entries are preserved correctly in the new PML4
- After CR3 reload, flush TLB via `invlpg` for all kernel code pages
- Use `map_4k_pages` for kernel code/data mapping (more robust than huge pages)

---

## 13. Syscall Updates for Per-Process Handles

**File:** `kernel/arch/x86_64/syscall.cpp`

All syscall handlers that use handles (close, dup, channel ops, port ops) must resolve from the current process:

```cpp
// New helper
static Process* current_process() {
    Thread* t = current_thread();
    return t ? t->process : nullptr;
}

// Example: sys_handle_close
uint64_t sys_handle_close(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return -1;
    proc->handles.Free(static_cast<handle_t>(a1));
    return 0;
}
```

Channel handle transfer (`Channel::Write/Read` with handle arrays) must:
1. Lookup handle in source process
2. Remove from source process handle table (no close — just table removal)
3. Allocate new handle in destination process via `msg->handles[i] = dest->handles.Alloc(obj, rights)`

---

## 14. Process Destruction

When a Process's last handle is closed and `Release()` triggers destruction:

```
Process::~Process():
    1. Mark all threads as Dead, remove from scheduler run queues
    2. For each VmRegion: Unmap (free page table pages recursively)
    3. Release VMO references (which may free physical pages)
    4. Free handle table entries (Release each object)
    5. Free PML4 page + recursively free all page table pages
    6. Free Process struct (kmalloc)
```

A Process is destroyed only when all its handles are closed AND all threads have exited AND all VMOs have no remaining references.

---

## 15. Testing Strategy

### Host-Side Tests (GTest)

| Test file | What it tests |
|-----------|---------------|
| `test/mm/vmo_test.cpp` | VMO create, GetPage, CloneCoW, COW fault semantics, destroy cleanup |
| `test/mm/vmm_test.cpp` | VmRegion insert/sort/find, Map/Unmap overlap detection, PML4 creation |
| `test/object/process_test.cpp` | Process create/destroy, handle table per-process isolation, thread list |
| `test/mm/elf_loader_test.cpp` | ELF parse valid/invalid, segment layout |

### Integration Tests (QEMU)

- Init process boots and prints to serial
- Channel write/read between kernel and init process
- Port-based service discovery
- Page fault triggers demand paging (verify via debug klog)
- COW: clone VMO, write, verify pages are independent

---

## 16. Files Summary

### New Files

| File | Purpose |
|------|---------|
| `kernel/core/object/process.hpp` | Process class declaration |
| `kernel/core/object/process.cpp` | Process implementation |
| `kernel/core/object/handle_table.hpp` | HandleTable class (extracted from handle_table.hpp) |
| `kernel/core/object/handle_table.cpp` | HandleTable implementation (extracted) |
| `kernel/core/mm/vmo.hpp` | VMO class, CowPage declaration |
| `kernel/core/mm/vmo.cpp` | VMO implementation |
| `kernel/core/mm/vmm.hpp` | VmRegion, VMM constants |
| `kernel/core/mm/vmm.cpp` | Address space ops: map, unmap, find, PML4 creation |
| `kernel/core/mm/page_walk.cpp` | Page table walker (walk PML4→PT, allocate intermediates) |
| `kernel/arch/x86_64/page_fault.cpp` | #PF ISR handler |
| `kernel/arch/x86_64/usermode.S` | enter_usermode: ring 0 → ring 3 via iretq |
| `kernel/arch/x86_64/usermode.hpp` | enter_usermode declaration |
| `kernel/core/elf_loader.hpp` | ELF loader declaration |
| `kernel/core/elf_loader.cpp` | ELF64 loading implementation |
| `kernel/init/init.cpp` | Init user-space program |
| `kernel/init/BUILD.bazel` | Build init ELF, embed into kernel |

### Modified Files

| File | Changes |
|------|---------|
| `kernel/core/object/object.hpp` | Add `Process`, `Vmo` to ObjectType enum |
| `kernel/core/object/handle_table.hpp` | Refactor global state into HandleTable class |
| `kernel/core/object/handle_table.cpp` | ditto |
| `kernel/core/sched/sched.hpp` | Thread: add `process`, `proc_next` fields |
| `kernel/core/sched/sched.cpp` | thread_create: accept process param; context switch: CR3 reload |
| `kernel/core/sched/switch.S` | Add CR3 reload across process boundaries |
| `kernel/arch/x86_64/syscall.cpp` | Per-process handle resolution; add Process/VMO syscalls |
| `kernel/arch/x86_64/syscall.hpp` | New syscall numbers for Process, VMO |
| `kernel/arch/x86_64/idt.cpp` | Register #PF handler (vector 14) |
| `kernel/arch/x86_64/gdt.cpp` | Verify/add user-mode code/data segments |
| `kernel/arch/x86_64/paging.cpp` | Fix CR3 reload bug; add kernel PML4 template save |
| `kernel/arch/x86_64/paging.hpp` | `page_table_walk` declaration |
| `kernel/arch/x86_64/boot.cpp` | Wire Phase 7 init sequence |
| `kernel/BUILD.bazel` | Add new sources, init ELF embedding |
| `test/mm/BUILD.bazel` | Add VMO, VMM, ELF loader tests |
| `CLAUDE.md` | Update phase status |

---

## 17. New Syscall Numbers

```cpp
// Process
constexpr uint64_t SYSCALL_PROCESS_CREATE  = 30;
constexpr uint64_t SYSCALL_PROCESS_START   = 31;
constexpr uint64_t SYSCALL_PROCESS_EXIT    = 32;

// Thread (per-process)
constexpr uint64_t SYSCALL_THREAD_CREATE   = 33;
constexpr uint64_t SYSCALL_THREAD_START    = 34;
constexpr uint64_t SYSCALL_THREAD_EXIT     = 35;

// VMO
constexpr uint64_t SYSCALL_VMO_CREATE      = 40;
constexpr uint64_t SYSCALL_VMO_MAP         = 41;
constexpr uint64_t SYSCALL_VMO_UNMAP       = 42;
constexpr uint64_t SYSCALL_VMO_RESIZE      = 43;
```
