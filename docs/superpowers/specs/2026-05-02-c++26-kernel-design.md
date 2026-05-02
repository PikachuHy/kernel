# C++26 Kernel Design

## Overview

A modern hybrid kernel written in C++26 targeting x86-64, with production ambition and multi-arch portability designed in from the start.

**Key decisions:**

| Decision | Choice |
|----------|--------|
| Architecture | Object-based hybrid kernel (Approach B) |
| CPU | x86-64 first, multi-arch later |
| Bootloader | Limine |
| Build system | Bazel 9 with MODULE.bazel |
| Dev host | macOS with cross-compilation |
| v0 milestone | Full-featured: processes, VFS, drivers, networking |
| Kernel/user split | Performance-oriented: scheduler, MM, IPC, VFS, block, network in kernel; USB, HID, graphics, audio in user space |

## Architecture

### Subsystem Breakdown

**Kernel-space subsystems** (ring 0):

| Subsystem | Role |
|-----------|------|
| Platform Layer | x86-64 specifics: GDT, IDT, APIC, paging setup, syscall entry, ACPI parsing, SMP bringup |
| Object Manager | Unified capability-based access control. Every kernel resource is a typed object with a handle |
| IPC | Channels (bidirectional) and Ports (many-to-one). Uniform communication fabric for kernel↔kernel, kernel↔user, user↔user |
| Scheduler | Preemptive thread scheduler with SMP support. Priority-based with fairness guarantees |
| Virtual Memory Manager | Per-process address spaces, demand paging, VMO-backed regions, memory-mapped I/O |
| Driver Host Framework | In-kernel driver lifecycle: binding, device discovery (ACPI/PCI), interrupt routing, DMA |
| VFS | Virtual filesystem layer with inode/dentry cache, pluggable filesystem drivers |
| Block Layer | Block device abstraction, I/O scheduling, buffer cache |
| Network Stack | Protocol stack + network device drivers. Socket interface via object model |

**User-space subsystems** (ring 3):

| Subsystem | Role |
|-----------|------|
| USB Stack | USB host controller drivers + device enumeration |
| HID/Input | Keyboard, mouse, touchscreen drivers |
| Graphics Server | Compositor, framebuffer management, GPU driver interface |
| Audio Server | Audio routing, mixing, hardware drivers |

All inter-component communication uses the IPC subsystem with a uniform protocol. The Object Manager mediates access via capabilities.

### Object Model and Capabilities

Every kernel resource is a typed object accessed via an opaque handle. Handles live in per-process handle tables and carry a set of rights (read, write, execute, map, enumerate, etc.).

**Core object types:**

| Object | Purpose |
|--------|---------|
| Process | Address space + handle table. Unit of resource ownership |
| Thread | Unit of execution. Owns stack and register state, belongs to a process |
| Channel | Bidirectional message pipe. Primary IPC primitive |
| Port | Many-to-one endpoint. Server registers, clients connect. Used for service discovery |
| Virtual Memory Object (VMO) | Contiguous memory region, potentially paged. Can be mapped into address spaces, shared between processes, file-backed |
| Interrupt | Represents a hardware interrupt line. Drivers bind and receive notifications |
| Resource | Hardware resource (I/O port range, MMIO region, IRQ). Hierarchical parent/child ownership |

**Lifecycle:** Objects are reference-counted. When last handle is closed and all references released, the object is destroyed — no separate free path.

**Capability model:** Handle transfer over channels can downgrade rights. Receivers can only use rights they were given. A driver gets only the MMIO region it needs, not arbitrary physical memory.

**Syscall interface** is object-oriented: `handle_create`, `handle_duplicate`, `handle_close`, plus type-specific syscalls (`channel_write`, `vmo_map`, etc.). Each syscall validates the handle, checks rights, then dispatches to the object's C++ method.

**C++ mapping:** Each object type is a C++ class inheriting from `KernelObject`. Handle tables store `shared_ptr<KernelObject>`. Rights checked in syscall layer before dispatch.

### Memory Management

**Physical memory — tiered allocators:**

- Buddy allocator for 4KB pages
- Slab allocator on top for kernel heap (`kmalloc`), sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
- DMA region for legacy devices
- C++26 `constexpr` where possible for compile-time allocation parameters

**Virtual memory — per-process address spaces:**

x86-64 48-bit virtual address space: user space 0x0 to 0x0000'7FFF'FFFF'FFFF, kernel space 0xFFFF'8000'0000'0000 and above. Kernel space shared across all processes (ring 0 only, protected by SMEP/SMAP).

**VMO-backed memory:** All user-visible memory is VMO-mapped. Enables shared memory (same VMO in multiple processes), memory-mapped files (file-backed VMO), COW fork (VMO clone), and demand paging.

**Kernel heap:** `kmalloc`/`kfree` via slab. C++ `new`/`delete` wired to `kmalloc`. Smart pointers work natively. Custom `kstd::allocator` for STL containers.

**Memory safety:** Per-type slab caches, delayed reuse for use-after-free detection, guard pages at stack boundaries, ASAN-compatible redzoning in debug builds.

### IPC and Communication Model

**Two primitives:**

- **Channel** — Bidirectional, point-to-point, FIFO message queue. Messages contain data + handles. No size limit. Handle transfer moves ownership.
- **Port** — Many-to-one endpoint. Server holds port, clients connect. Used for service name resolution (e.g., `"block/ahci/0"`).

**Message structure:**

```cpp
struct Message {
    span<byte> data;
    vector<Handle> handles;  // transferred to receiver
    uint64_t txid;           // request-reply correlation, 0 = fire-and-forget
};
```

Zero-copy for bulk data: sender passes a VMO handle instead of copying bytes.

**Call patterns:** fire-and-forget (`channel_write`), request-reply (`channel_call` — syntactic sugar with txid), async server (`port_wait` — demux multiple channels via port).

**Kernel-internal IPC:** Same channel/port mechanism but in-kernel calls skip the syscall boundary (shared address space). Same types, same handle passing semantics.

**Syscall list:**

```
handle_create(process, object_type, rights) → handle
handle_duplicate(handle, new_rights) → handle
handle_close(handle)
channel_create() → (handle_a, handle_b)
channel_write(handle, data, handles)
channel_read(handle) → Message
port_create() → handle
port_connect(port_handle, channel_handle)
vmo_create(size) → handle
vmo_map(vmo_handle, addr, size, rights)
process_create(name) → handle
process_start(process_handle, entry, stack_vmo)
thread_create(process_handle, name) → handle
thread_start(thread_handle, entry_point, arg)
```

### Boot Sequence

Limine provides: 64-bit long mode, memory map, framebuffer, SMBIOS, ACPI RSDP, SMP info, kernel modules.

```
Limine → kernel entry (long mode)
  → 1. Early init (BSP, no allocator): GDT, IDT, bitmap page allocator, higher half map
  → 2. Core init (BSP): buddy + slab allocators, per-CPU areas, APIC, Object Manager,
       kernel process, ACPI parsing
  → 3. SMP bringup: INIT-SIPI to APs, trampoline to long mode, AP setup, park in idle
  → 4. Subsystem init: scheduler live, VFS (devfs + initramfs), PCI enumeration,
       driver binding, IPC namespace
  → 5. User-space bootstrap: init process from embedded ELF, driver hosts, servers
```

### Project Structure and Build

**Bazel 9 with MODULE.bazel.** Custom `cc_toolchain` for x86-64-elf cross-compilation from macOS using LLVM/Clang. Freestanding C++26 (no libc, no exceptions by default, no RTTI except opt-in).

```
kernel/
├── MODULE.bazel
├── BUILD.bazel
├── toolchain/
│   └── x86_64_elf.BUILD
├── kernel/
│   ├── arch/x86_64/    # boot, gdt, idt, syscall, apic, smp
│   ├── core/           # object, ipc, sched, mm, process
│   ├── fs/             # VFS, devfs, initramfs
│   ├── blk/            # block layer, I/O scheduler
│   ├── net/            # network stack
│   ├── drivers/        # bus (PCI), block (AHCI, NVMe)
│   └── lib/            # kstd, klog, panic
├── userspace/          # init, usb, gfx, audio (separate ELFs)
├── lib/                # shared user-space libraries (libc, libipc)
├── scripts/            # run.sh, debug.sh, iso.sh
├── test/               # host-side tests, integration tests
└── third_party/
    └── limine/
```

**Build commands:**
```
bazel build //kernel:iso         # Full bootable image
bazel build //kernel:kernel      # Just the kernel ELF
bazel test //test:...            # All tests
bazel run //scripts:run          # Build + QEMU
```

### Testing Strategy

**Host-side unit tests:** Subsystems (object manager, allocators, VFS, IPC) compiled for macOS with mocked dependencies. GTest via Bazel. Runs in under a second.

**User-space test harness:** Minimal ELF loaded as init inside QEMU. Exercises syscalls end-to-end: process creation, threads, channels, VMO mapping.

**Integration tests:** Full QEMU boot with driver binding, filesystem mount, init process. Runs in CI but not on every local build.

**Sanitizers:** UBSan and ASan-equivalent in kernel debug builds. Full ASan/UBSan for host-side tests.

**Tools:** QEMU for emulation, GDB for remote debugging (`-s -S`), `KASSERT`/`KPANIC` for kernel assertions with distinctive CPU halt patterns.
