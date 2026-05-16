# Fix Remaining Issues — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix FS server ack race (enabling event loops for multi-open) and paging_init CR3 reload (enabling kernel-owned page tables replacing Limine's).

**Architecture:** Ack race: dedicated ack Channel per open — `sys_open` creates two Channels, one for ack (no contention), one for file I/O. Paging: use Limine HHDM during page table construction, switch to kernel direct map after CR3 reload.

**Tech Stack:** C++26 freestanding, x86-64, Bazel 9

---

## Task 1: Ack Race — Protocol + sys_open

**Files:**
- Modify: `kernel/fs/protocol.hpp` — add `ack_handle` to `OpenPayload`
- Modify: `kernel/arch/x86_64/syscall.cpp` — `sys_open` create `ack_chan`, read ack from it

- [ ] **Step 1: Add `ack_handle` to OpenPayload**

Edit `kernel/fs/protocol.hpp`. There is no `OpenPayload` in protocol.hpp — it's defined inline in syscall.cpp and the FS servers. Add a shared definition to protocol.hpp:

```cpp
// Open message sent from kernel to FS server via the mount Channel.
// The kernel pre-allocates handles in the FS server's handle table.
struct OpenPayload {
    char     path[256];
    uint32_t file_handle;   // FS server: use for file I/O (Read/Write/Close)
    uint32_t ack_handle;    // FS server: write ack here (kernel reads it)
    uint32_t flags;
};
```

- [ ] **Step 2: Update FS server OpenPayload to match**

Edit `kernel/fs/devfs/devfs.cpp` — remove the local `OpenPayload` definition (lines 40-44) and use shared `#include "kernel/fs/protocol.hpp"` if not already present. Same for `kernel/fs/tmpfs/tmpfs.cpp` (lines 31).

Actually, the FS servers are ring-3 ELFs that cannot include kernel headers. They define their own copies. Update the devfs copy:

```cpp
struct OpenPayload {
    char     path[256];
    uint32_t file_handle;
    uint32_t ack_handle;
    uint32_t flags;
};
```

Same update in tmpfs.cpp.

- [ ] **Step 3: Update `sys_open` to create `ack_chan` and read ack from it**

Edit `kernel/arch/x86_64/syscall.cpp`. After creating `file_chan` (around line 312), add `ack_chan` creation:

```cpp
// Create the ack Channel (dedicated — no contention with file I/O handler)
Channel* ack_chan = static_cast<Channel*>(kmalloc(sizeof(Channel)));
if (!ack_chan) {
    file_chan->Release();
    return INVALID_HANDLE;
}
new (ack_chan) Channel();

// Allocate handle in FS server's table for ack_chan
handle_t fs_ack_handle = fs_proc->handles.Alloc(ack_chan, full_rights);
```

Then in the OpenPayload, set the new field:

```cpp
payload.ack_handle = fs_ack_handle;
```

Then change the blocking read from `file_chan` to `ack_chan`:

```cpp
// Blocking read: wait for FS server ack on ack_chan (no contention)
out_len = 0;
while (true) {
    rc2 = ack_chan->Read(&resp, sizeof(resp), &out_len, nullptr, 0, nullptr);
    if (rc2 != -2) break;
    thread_yield();
}
```

On error, free BOTH handles:

```cpp
if (rc2 != 0 || resp.result != 0) {
    fs_proc->handles.Free(fs_file_handle);
    fs_proc->handles.Free(fs_ack_handle);
    file_chan->Release();
    ack_chan->Release();
    return INVALID_HANDLE;
}
```

After returning the client handle, release `ack_chan` (its ref is held by the FS server handle only — the ack is one-shot, so the FS server will close it after use):

```cpp
// ack_chan is no longer needed; the FS server will close its handle
ack_chan->Release();
return client_handle;
```

- [ ] **Step 4: Build and verify compilation**

```bash
bazel build //kernel:kernel
```
Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add kernel/fs/protocol.hpp kernel/arch/x86_64/syscall.cpp \
        kernel/fs/devfs/devfs.cpp kernel/fs/tmpfs/tmpfs.cpp
git commit -m "feat: add dedicated ack Channel — fix FS server ack race"
```

---

## Task 2: Ack Race — devfs Event Loop

**Files:**
- Modify: `kernel/fs/devfs/devfs.cpp` — `_start()` write ack to `ack_handle`, dispatch to handler
- Modify: `kernel/fs/devfs/BUILD.bazel` — remove `-Wno-unused-function`

- [ ] **Step 1: Remove `__attribute__((unused))` from handlers**

In devfs.cpp, remove `__attribute__((unused))` from `handle_null`, `handle_zero`, `handle_console`:

```cpp
static void handle_null(uint32_t file_chan) {  // was: __attribute__((unused)) static void
```

- [ ] **Step 2: Rewrite `_start()` to be a long-lived event loop**

Replace the current `_start()` (lines 219-248) with:

```cpp
extern "C" void _start() {
    const uint32_t MOUNT_CHAN = 1;

    while (true) {
        uint8_t buf[264];
        int rc = channel_read(MOUNT_CHAN, buf, sizeof(buf));
        if (rc < 0) break;

        OpenPayload* payload = reinterpret_cast<OpenPayload*>(buf);
        uint32_t file_handle = payload->file_handle;
        uint32_t ack_handle   = payload->ack_handle;
        const char* rel = payload->path;

        // Strip leading "/dev/" prefix if present
        if (rel[0] == '/' && rel[1] == 'd' && rel[2] == 'e' &&
            rel[3] == 'v' && rel[4] == '/')
            rel += 5;

        // Write ack on dedicated ack Channel (no contention!)
        FileResponse ack = {0, 0};
        channel_write(ack_handle, &ack, sizeof(ack));
        handle_close(ack_handle);  // ack done, not needed anymore

        // Dispatch to device handler (enters per-file I/O loop)
        if (rel[0] == 'n' && rel[1] == 'u' && rel[2] == 'l' &&
            rel[3] == 'l' && rel[4] == '\0') {
            handle_null(file_handle);
        } else if (rel[0] == 'z' && rel[1] == 'e' && rel[2] == 'r' &&
                   rel[3] == 'o' && rel[4] == '\0') {
            handle_zero(file_handle);
        } else if (rel[0] == 'c' && rel[1] == 'o' && rel[2] == 'n' &&
                   rel[3] == 's' && rel[4] == 'o' && rel[5] == 'l' &&
                   rel[6] == 'e' && rel[7] == '\0') {
            handle_console(file_handle);
        } else {
            FileResponse err = {-1, 0};
            channel_write(file_handle, &err, sizeof(err));
            handle_close(file_handle);
        }
    }

    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
```

- [ ] **Step 3: Remove `debug()` function and `SYS_DEBUG_PRINT`**

Since `debug()` is no longer used, remove:
- The `debug()` function definition (lines 66-68)
- `SYS_DEBUG_PRINT` from the syscall numbers (line 14)

- [ ] **Step 4: Remove `-Wno-unused-function` from BUILD.bazel**

Edit `kernel/fs/devfs/BUILD.bazel`, revert to `-Wall -Wextra -Werror` (remove the `-Wno-unused-function` flag).

- [ ] **Step 5: Build and verify**

```bash
bazel build //kernel:kernel
```
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/devfs/devfs.cpp kernel/fs/devfs/BUILD.bazel
git commit -m "feat: devfs event loop — long-lived server with ack Channel"
```

---

## Task 3: Ack Race — tmpfs Event Loop

**Files:**
- Modify: `kernel/fs/tmpfs/tmpfs.cpp` — `_start()` write ack to `ack_handle`, use DirEntry/FileState
- Modify: `kernel/fs/tmpfs/BUILD.bazel` — remove `-Wno-unused-function -Wno-unused-const-variable`

- [ ] **Step 1: Remove `__attribute__((unused))` from `vmo_create`**

```cpp
static uint32_t vmo_create(uint64_t size) {  // was: __attribute__((unused)) static
```

- [ ] **Step 2: Uncomment `O_CREAT` constant**

```cpp
constexpr uint32_t O_CREAT = 1 << 3;  // was: // constexpr ...
```

- [ ] **Step 3: Fix `handle_file` Write to extend file size**

Replace the Write case (lines 172-186) with:

```cpp
case FileMsg::Write: {
    size_t count = data_len;
    if (msg->offset + count > file_size) {
        uint64_t new_sz = msg->offset + count;
        if (new_sz > 4096) {
            if (msg->offset < 4096) count = 4096 - msg->offset;
            else count = 0;
            new_sz = 4096;
        }
        if (fs->entry) fs->entry->size = new_sz;
    }
    uint8_t* dst = reinterpret_cast<uint8_t*>(MAP_BASE + msg->offset);
    for (size_t i = 0; i < count; i++) dst[i] = data[i];
    resp.result = 0;
    resp.size = count;
    channel_write(fs->file_chan, &resp, sizeof(resp));
    break;
}
```

- [ ] **Step 4: Rewrite `_start()` for event loop with VMO files**

Replace the current `_start()` (lines 219-242) with:

```cpp
extern "C" void _start() {
    const uint32_t MOUNT_CHAN = 1;

    while (true) {
        uint8_t buf[264];
        int rc = channel_read(MOUNT_CHAN, buf, sizeof(buf));
        if (rc < 0) break;

        OpenPayload* payload = reinterpret_cast<OpenPayload*>(buf);
        uint32_t file_handle = payload->file_handle;
        uint32_t ack_handle   = payload->ack_handle;
        const char* rel = payload->path;

        while (*rel == '/') rel++;

        DirEntry* entry = find_entry(rel);
        if (!entry && (payload->flags & O_CREAT)) {
            uint32_t vmo = vmo_create(4096);
            if (vmo) entry = add_entry(rel, vmo, false, 0);
        }

        if (!entry) {
            FileResponse resp = {-1, 0};
            channel_write(file_handle, &resp, sizeof(resp));
            handle_close(ack_handle);
            handle_close(file_handle);
            continue;
        }

        FileState* fs = alloc_file_state(file_handle, entry->vmo_handle,
                                          entry->is_dir, entry);
        if (!fs) {
            FileResponse resp = {-1, 0};
            channel_write(file_handle, &resp, sizeof(resp));
            handle_close(ack_handle);
            handle_close(file_handle);
            continue;
        }

        // Ack on dedicated ack Channel (no contention!)
        FileResponse ack = {0, 0};
        channel_write(ack_handle, &ack, sizeof(ack));
        handle_close(ack_handle);

        // Enter per-file I/O loop (blocks until Close)
        handle_file(fs);
    }

    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
```

- [ ] **Step 5: Remove `debug()` and `SYS_DEBUG_PRINT`**

Remove:
- The `debug()` function
- `SYS_DEBUG_PRINT` from the constexpr list

- [ ] **Step 6: Remove suppression flags from BUILD.bazel**

Edit `kernel/fs/tmpfs/BUILD.bazel`, revert to `-Wall -Wextra -Werror`.

- [ ] **Step 7: Build and verify**

```bash
bazel build //kernel:kernel
```
Expected: Build succeeds.

- [ ] **Step 8: QEMU boot test**

```bash
bash scripts/run.sh
```
Expected: All 7 init tests pass, devfs and tmpfs event loops handle multiple opens.

- [ ] **Step 9: Commit**

```bash
git add kernel/fs/tmpfs/tmpfs.cpp kernel/fs/tmpfs/BUILD.bazel
git commit -m "feat: tmpfs event loop — VMO-backed files with ack Channel"
```

---

## Task 4: Paging Fix — hhdm_ptr + Boot Order

**Files:**
- Modify: `kernel/arch/x86_64/paging.cpp` — fix `hhdm_ptr`, add `g_hhdm`
- Modify: `kernel/arch/x86_64/paging.hpp` — add `g_hhdm` extern declaration
- Modify: `kernel/arch/x86_64/boot.cpp` — reorder init, enable `paging_init`

- [ ] **Step 1: Add `g_hhdm` to paging.hpp**

Edit `kernel/arch/x86_64/paging.hpp`, add after includes:

```cpp
// Limine HHDM offset — set by paging_init(), used during page table construction.
// After CR3 switch, updated to DIRECT_MAP_BASE so hhdm_ptr() uses kernel's own
// direct map instead of Limine's.
extern uint64_t g_hhdm;
```

- [ ] **Step 2: Fix `hhdm_ptr` and `alloc_table_phys` in paging.cpp**

Edit `kernel/arch/x86_64/paging.cpp`:

Define `g_hhdm` at file scope (before any functions):

```cpp
uint64_t g_hhdm = 0;
```

Change `hhdm_ptr`:

```cpp
// Before paging_init: g_hhdm = Limine HHDM offset (access via Limine PML4[510])
// After  paging_init: g_hhdm = DIRECT_MAP_BASE (access via kernel PML4[256])
inline uint64_t* hhdm_ptr(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(g_hhdm + phys);
}
```

In `paging_init(uint64_t hhdm)`, save `g_hhdm` at the start:

```cpp
void paging_init(uint64_t hhdm) {
    g_hhdm = hhdm;  // Use Limine's HHDM during construction
    ...
```

After the CR3 reload (around line 162), cut over to the kernel's own direct map:

```cpp
    asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
    g_hhdm = DIRECT_MAP_BASE;  // Now use our own direct map
```

- [ ] **Step 3: Fix `alloc_table_phys` to use buddy correctly**

The current `alloc_table_phys` uses `buddy_alloc_pages(0)` (1 page). Ensure buddy is initialized before `paging_init` is called. No change needed in paging.cpp itself — this is a boot order fix.

- [ ] **Step 4: Reorder boot sequence and enable paging_init**

Edit `kernel/arch/x86_64/boot.cpp`. Current order (around lines 203-211):

```cpp
    // paging_init has a CR3-reload triple-fault bug (debugging in progress).
    // For now, use Limine's page tables and save them as the kernel template.
    klog("Using Limine page tables...\n");
    paging_save_kernel_template();

    // 4. Buddy allocator
    klog("Initializing buddy allocator...\n");
    buddy_init(hhdm, 0);
```

Change to:

```cpp
    // 4. Buddy allocator (needed by paging_init for page table pages)
    klog("Initializing buddy allocator...\n");
    buddy_init(hhdm, 0);
    klog("  buddy ready\n\n");

    // 3. Higher-half paging takeover
    klog("Initializing kernel page tables...\n");
    paging_init(hhdm);
    klog("  kernel PML4 active\n\n");
```

- [ ] **Step 5: Build kernel**

```bash
bazel build //kernel:kernel
```
Expected: Build succeeds.

- [ ] **Step 6: QEMU boot test**

```bash
bash scripts/run.sh
```
Expected: Boot log shows "kernel PML4 active" instead of "Using Limine page tables". No crash, all 7 init tests pass.

If crash: the framebuffer address needs fixing (Task 5).

- [ ] **Step 7: Commit**

```bash
git add kernel/arch/x86_64/paging.cpp kernel/arch/x86_64/paging.hpp \
        kernel/arch/x86_64/boot.cpp
git commit -m "fix: paging_init CR3 reload — transitional HHDM, kernel direct map"
```

---

## Task 5: Paging Fix — Framebuffer + Final Verification

**Files:**
- Modify: `kernel/lib/klog.cpp` — fix `klog_reinit_fb`
- Modify: `kernel/arch/x86_64/boot.cpp` — call `klog_reinit_fb` after `paging_init`
- Modify: `CLAUDE.md` — update known issues

- [ ] **Step 1: Fix `klog_reinit_fb` for framebuffer address conversion**

Edit `kernel/lib/klog.cpp`. The `klog_reinit_fb` function needs to convert the framebuffer address from Limine's HHDM to the kernel's direct map. If the framebuffer address is already a physical address, it needs to be converted to virtual via the new direct map.

```cpp
extern uint64_t g_hhdm;  // from paging.cpp — now = DIRECT_MAP_BASE after paging_init

void klog_reinit_fb(void* fb_addr_phys_or_virt) {
    // If fb_addr is already a virtual address in Limine's HHDM range,
    // it may need conversion. For simplicity: if it's below 4GB, treat
    // as physical and remap through the kernel's direct map.
    uint64_t addr = reinterpret_cast<uint64_t>(fb_addr_phys_or_virt);
    if (addr < 0x100000000ULL) {
        // Physical address — map through direct map
        s_fb.addr = reinterpret_cast<uint8_t*>(g_hhdm + addr);
    }
    // Otherwise it's already a virtual address — keep as-is
    // (or it was already converted by paging_init)
}
```

- [ ] **Step 2: Call `klog_reinit_fb` after `paging_init`**

Edit `kernel/arch/x86_64/boot.cpp`. After `paging_init(hhdm)`:

```cpp
    paging_init(hhdm);
    // Framebuffer address from Limine may be physical — convert to kernel direct map
    if (framebuffer_request.response) {
        klog_reinit_fb(framebuffer_request.response->framebuffers[0]->address);
    }
```

- [ ] **Step 3: Full QEMU boot test**

```bash
bash scripts/run.sh
```
Expected: Clean boot, "kernel PML4 active", all init tests pass.

- [ ] **Step 4: Run host tests**

```bash
bazel test //test/fs:all //test/irq:irq_test //test/mm:all //test/sched:sched_test --test_output=errors
```
Expected: 7 tests pass.

- [ ] **Step 5: Update CLAUDE.md**

Mark both issues as resolved, update known issues section.

- [ ] **Step 6: Commit**

```bash
git add kernel/lib/klog.cpp kernel/arch/x86_64/boot.cpp CLAUDE.md
git commit -m "fix: framebuffer address after paging_init, update known issues"
```
