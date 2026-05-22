# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A modern hybrid kernel written in C++26 targeting x86-64, with production ambition. Object-based architecture with capability security model (Zircon-inspired). Performance-oriented: scheduler, MM, IPC, VFS, block, and network run in kernel space; USB, HID, graphics, and audio run as user-space drivers.

## Key Decisions

- **Architecture:** Object-based hybrid kernel (Approach B in design spec)
- **Bootloader:** Limine (native protocol)
- **Build system:** Bazel 9 with MODULE.bazel
- **Dev host:** macOS with LLVM/Clang cross-compilation to `x86_64-unknown-elf`
- **C++:** Freestanding C++26 — no exceptions (kernel panics on throw), no RTTI except for Object Manager type checking

## Design Docs

- Design spec: `docs/superpowers/specs/2026-05-02-c++26-kernel-design.md`
- Implementation plans: `docs/superpowers/plans/`

### Phase Plans

| Phase | Plan | Status |
|-------|------|--------|
| 1: Foundation | `docs/superpowers/plans/2026-05-02-phase-1-foundation.md` | Done |
| 2: Memory Management | `docs/superpowers/plans/2026-05-02-phase-2-memory-management.md` | Done |
| 3: Interrupt + Timer | `docs/superpowers/plans/2026-05-05-phase-3-apic-timer.md` | Done |
| 4: SMP | `docs/superpowers/plans/2026-05-05-phase-4-smp.md` | Done |
| 5: Scheduler | `docs/superpowers/plans/2026-05-05-phase-5-scheduler.md` | Done |
| 6: Object Manager + IPC | `docs/superpowers/plans/2026-05-05-phase-6-object-ipc.md` | Done |
| 7: VMM + Process | `docs/superpowers/plans/2026-05-10-phase-7-vmm-process.md` | Done (incl. 3 fixes) |
| 8: VFS | `docs/superpowers/plans/2026-05-16-phase-8-vfs.md` | Done (incl. syscall per-thread stack fix) |
| 9: Block Layer | `docs/superpowers/plans/2026-05-17-phase-9-block-layer.md` | Done |
| 10: FAT32 | `docs/superpowers/plans/2026-05-17-phase-10-fat32.md` | Done (BPB/FAT/dir, file open+read, ELF magic 0x7F'E'L'F' verified)
| Fix Known Issues | `docs/superpowers/plans/2026-05-05-fix-known-issues.md` | Done (TSS+buddy, FAT32 read, paging deferred)
| Timer #GP Debug | (N/A — ad-hoc) | Partial: isr_common CS/SS fix committed, irq_stub unresolved |

## Build / Test / Lint

```bash
# Build kernel ELF
bazel build //kernel:kernel

# Run host-side unit tests (PMM, buddy, slab, IRQ)
bazel test //test/mm:all
bazel test //test/irq:all
bazel test //test/sched:all
bazel test //test/object:all

# Build and boot in QEMU (serial output, no GUI)
bash scripts/run.sh
```

## Known Issues

- **Timer preemption #GP**: enabling timer_periodic with ring-3 processes causes #GP (selector 0x10) at `irq_stub_32` iretq. CS in the iretq frame becomes 0x10 (data segment). Seven attempted fixes failed (CS/SS force-correction, RFLAGS clear, frame save/restore, IST3, cli in enter_usermode, SMP=1). `isr_common` has a CS/SS force-correction (committed). Likely QEMU 11.0.0 emulation bug with LAPIC timer during ring transitions. Timer disabled, cooperative yield works.
- **paging_init**: CR3 reload causes crash with Limine's 2MB huge pages. Kernel uses Limine page tables via `paging_save_kernel_template()`. (Phase 2 legacy, deferred)
- **devfs single-file**: devfs server can only serve one open file at a time (single-threaded handler event loop). Workaround: init loads before FAT32 so `/dev/console` opens before `/dev/ahci0`.

## Key Implementation Patterns

- **Ring-3 syscall arg structs**: Channel write args (WA struct) must be static in ring-3 servers. Stack-local structs get corrupted (address → 0x1) in deep call chains during syscall (seen in FAT32 `ch_write`).
- **Channel IPC endpoints**: File handles allocated in FS server processes need `Rights::ChannelEndpointB` so reads/writes route to the correct queue. Client handles must NOT have this bit.
- **Kernel defense**: `sys_channel_write` validates user pointer > 4096 before dereference (prevents page-fault from ring-3 stack corruption).
- **ELF .bss pages**: Static zero-initialized globals in ring-3 ELFs may not have PTEs pre-installed by `elf_load`. Force .data placement with non-zero initializer (e.g. `= {0xAA}`) for large buffers.
- **Bare-metal tracing**: Use `outb` to serial port 0x3F8 for debugging when klog isn't available (exception handlers, IRQ stubs). Pattern: `asm("movw $0x3F8, %%dx; outb %%al, %%dx" : : "a"((uint8_t)val));`
- **Linter interference**: The environment linter auto-reverts edits to worktree files. For reliable edits, modify files in the main repo and use `git checkout --` carefully.

## Architecture

```
kernel/
├── arch/x86_64/        # boot, gdt, idt, apic, ioapic, irq, acpi, smp, trampoline, timer, syscall, paging, usermode, linker script
├── core/
│   ├── mm/             # pmm, bitmap_alloc, buddy, slab, vmo, vmm, new_delete
│   ├── sched/          # scheduler — thread, run queue, context switch
│   └── object/         # KernelObject, handle table, rights, channel, port, process
├── fs/
│   ├── fat32/          # FAT32 ring-3 server — MBR, BPB, FAT, dir, file read
│   ├── devfs/          # devfs ring-3 server — /dev/null, /dev/zero, /dev/console, block passthrough
│   ├── tmpfs/          # tmpfs ring-3 server — in-memory VMO-backed files
│   ├── mount.cpp/hpp   # Mount namespace (prefix-match routing)
│   └── protocol.hpp    # FileMsg/FileResponse/Stat/Dirent shared protocol
├── init/               # init process (ring-3 ELF), linker script
├── lib/                # klog, panic, serial, spinlock
├── BUILD.bazel
test/
├── mm/                 # Host-side allocator tests (GTest — currently broken: buddy.hpp not found)
├── irq/                # Host-side IRQ dispatch tests (GTest)
├── sched/              # Host-side scheduler tests (GTest)
├── object/             # Host-side object/IPC tests (GTest)
scripts/run.sh          # Build + QEMU boot (creates FAT32 disk image with Limine)
third_party/limine/     # Limine protocol header
```
