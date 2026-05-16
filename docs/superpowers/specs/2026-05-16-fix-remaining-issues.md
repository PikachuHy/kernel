# Fix Remaining Issues — Design Spec

## Issue 1: FS Server Ack Race

### Problem

VFS `sys_open` creates a file Channel for client↔FS server I/O. The FS server
writes a `FileResponse{0,0}` ack on this Channel to signal "open succeeded".
`socket_open` blocks reading this ack.

When the FS server enters an event loop (e.g., `handle_console`), it reads from
the SAME file Channel for `FileMsg` operations. The single FIFO queue means the
FS server handler may consume the ack before the kernel does. The kernel blocks
forever, the client never gets a handle.

Root cause: one Channel used for two different purposes (ack + file I/O),
with two consumers (kernel + FS server handler).

### Design: Dedicated Ack Channel

`sys_open` creates **two** Channels:

```
sys_open creates:
  ├── file_chan  → bidirectional file I/O (client↔FS server handler)
  └── ack_chan   → one-shot ack (FS server → kernel)
```

**OpenPayload extended:**

```cpp
struct OpenPayload {
    char     path[256];
    uint32_t file_handle;   // FS server: file I/O
    uint32_t ack_handle;    // FS server: write ack here (NEW)
    uint32_t flags;
};
```

**Flow:**

1. `sys_open`: create `file_chan` + `ack_chan`
2. Allocate handles in FS server's handle table for both
3. Send OpenPayload (with `file_handle` + `ack_handle`) via mount Channel
4. Block on `ack_chan->Read()` waiting for ack
5. FS server: read OpenPayload from mount Channel
6. FS server: write `FileResponse{0,0}` to `ack_handle`
7. FS server: enter handler loop on `file_handle` (Read/Write/Close)
8. Kernel: ack received → allocate handle for `file_chan` in caller process
9. Return `file_handle` to caller

`ack_chan` has exactly one consumer (kernel `sys_open`) and one producer
(FS server). No race possible.

**Files modified:**
- `kernel/fs/protocol.hpp` — `OpenPayload` add `ack_handle` field
- `kernel/arch/x86_64/syscall.cpp` — `sys_open` create `ack_chan`, read ack from it
- `kernel/fs/devfs/devfs.cpp` — `_start()` write ack to `ack_handle`, then dispatch
- `kernel/fs/tmpfs/tmpfs.cpp` — `_start()` write ack to `ack_handle`, then dispatch

---

## Issue 2: paging_init CR3 Reload

### Problem

`paging_init()` should take over page tables from Limine, creating the kernel's
own PML4 with 4KB kernel mappings and a 2MB direct map. The CR3 reload crashes
(triple fault).

There are two layers:

1. **Immediate crash during construction**: `hhdm_ptr()` uses `DIRECT_MAP_BASE`
   (PML4[256]), but Limine maps its HHDM at PML4[510]. During page table
   construction (before CR3 switch, still on Limine's tables), any access
   through `hhdm_ptr()` faults.

2. **Post-CR3 framebuffer crash**: After CR3 switch, the kernel's new PML4
   has a fresh direct map at PML4[256], but Limine's HHDM at PML4[510] is
   preserved. However, the framebuffer address from Limine (passed to `klog`)
   may be a Limine HHDM virtual address that needs conversion to the new
   direct map.

### Design: Transitional HHDM + Framebuffer Fix

**Step 1 — Store Limine HHDM, use it during construction**

```cpp
static uint64_t g_hhdm = 0;  // Limine HHDM offset

// During page table construction, use g_hhdm (Limine's HHDM at PML4[510])
inline uint64_t* hhdm_ptr(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(g_hhdm + phys);
}
```

`paging_init(hhdm)` saves `g_hhdm = hhdm` then builds the new PML4.
All physical accesses go through `g_hhdm + phys` → Limine's PML4[510]. Safe.

**Step 2 — Build fresh direct map at PML4[256]**

Construct 2MB huge pages at `DIRECT_MAP_BASE` (0xFFFF800000000000) covering
physical memory 0..highest_phys_addr.

**Step 3 — CR3 switch, then cut over to direct map**

```cpp
asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
g_hhdm = DIRECT_MAP_BASE;  // Now use our own direct map
```

After this, `hhdm_ptr(phys)` → `DIRECT_MAP_BASE + phys` → PML4[256]. No
dependency on Limine.

**Step 4 — Fix framebuffer address**

`klog_init()` stores the framebuffer address from Limine. After CR3 switch,
call `klog_reinit_fb()` to convert the framebuffer address to the new direct
map (`DIRECT_MAP_BASE + fb_phys`).

Limine provides the framebuffer address as a virtual address in its HHDM:
`fb_virt = g_hhdm_old + fb_phys`. After the switch, we need:
`fb_virt_new = DIRECT_MAP_BASE + fb_phys`.

**Boot sequence adjustment:**

`buddy_init()` must run before `paging_init()` because page table
construction uses `buddy_alloc_pages(0)` for intermediate table pages.
Current order has `paging_init` commented out before `buddy_init`.

Fixed order:
```
bitmap_init → slab_init → buddy_init → paging_init → tss_init
```

**Files modified:**
- `kernel/arch/x86_64/paging.cpp` — fix `hhdm_ptr`, add `g_hhdm`, add CR3 cut-over
- `kernel/arch/x86_64/paging.hpp` — add `g_hhdm` extern
- `kernel/arch/x86_64/boot.cpp` — reorder init, enable `paging_init`
- `kernel/lib/klog.cpp` — fix `klog_reinit_fb` for framebuffer address conversion

---

## Verification

### Ack Race Fix
```bash
bash scripts/run.sh
```
Expected: init opens both `/dev/console` and `/test.txt`, both succeed.
After VFS open, init performs file I/O (write+read) successfully.

### Paging Fix
```bash
bash scripts/run.sh
```
Expected: Boot log shows "Paging: kernel PML4 initialized" (instead of
"Using Limine page tables"). No crash, all 7 init tests pass.
