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
| 8: VFS | `docs/superpowers/plans/2026-05-16-phase-8-vfs.md` | Kernel-side done; FS server IPC wake-up bug |
| Fix Known Issues | `docs/superpowers/plans/2026-05-05-fix-known-issues.md` | Done (TSS+buddy; paging deferred) |

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

- **paging_init**: CR3 reload causes crash with Limine's 2MB huge pages. Kernel uses Limine page tables via `paging_save_kernel_template()`. (Phase 2 legacy, deferred)
- **Channel IPC write/read**: untested from ring 3 — requires `ChannelWriteArgs` packed struct in user-space syscall convention. Basic syscalls (create, close, debug_print, process_exit) verified.
- **Phase 8 FS server wake-up**: FS servers (devfs, tmpfs) crash with PAGE FAULT at 0x0000000F0000000F when woken from blocking `channel_read` after `thread_yield`. Suspected stack corruption in syscall context switch path. Kernel-side VFS (mount namespace, sys_open routing, handle allocation, Channel IPC) verified working. Timer preemption disabled pending fix.
- **Timer preemption iretq #GP**: LAPIC timer preemption crashes at `iretq` in IRQ stubs when returning to ring 3. Same root cause as syscall stack issue — global `user_rsp_save`/`temp_kstack` in interrupt path. #PF timer interaction deferred.

## Architecture

```
kernel/
├── arch/x86_64/        # boot, gdt, idt, apic, ioapic, irq, acpi, smp, trampoline, timer, syscall, paging, usermode, linker script
├── core/
│   ├── mm/             # pmm, bitmap_alloc, buddy, slab, vmo, vmm, new_delete
│   ├── sched/          # scheduler — thread, run queue, context switch
│   └── object/         # KernelObject, handle table, rights, channel, port, process
├── fs/                 # VFS mount namespace, protocol, devfs/tmpfs ELF servers
├── init/               # init process (ring-3 ELF), linker script
├── lib/                # klog, panic, serial, spinlock
├── BUILD.bazel
test/
├── mm/                 # Host-side allocator tests (GTest)
├── irq/                # Host-side IRQ dispatch tests (GTest)
├── sched/              # Host-side scheduler tests (GTest)
├── object/             # Host-side object/IPC tests (GTest)
scripts/run.sh          # Build + QEMU boot
third_party/limine/     # Limine protocol header
```
