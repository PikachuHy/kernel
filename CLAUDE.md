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
| 4-10: Remaining | TBD | — |

## Build / Test / Lint

```bash
# Build kernel ELF
bazel build //kernel:kernel

# Run host-side unit tests (PMM, buddy, slab, IRQ)
bazel test //test/mm:all
bazel test //test/irq:all

# Build and boot in QEMU (serial output, no GUI)
bash scripts/run.sh
```

## Known Issues

- **paging_init**: CR3 reload causes triple-fault (root cause TBD). Kernel currently runs on Limine's page tables. Slab allocator accesses memory via Limine's HHDM.
- **buddy allocator**: Implemented and host-tested, but not yet wired into kernel boot (slab uses bitmap_alloc directly as temporary backing).
- **I/O ports**: No I/O permission bitmap in TSS — currently relying on IOPL=0 default.

## Architecture

```
kernel/
├── arch/x86_64/        # boot, gdt, idt, apic, ioapic, irq, timer, syscall, paging, linker script
├── core/mm/            # pmm, bitmap_alloc, buddy, slab, new_delete
├── lib/                # klog, panic, serial
├── BUILD.bazel
test/
├── mm/                 # Host-side allocator tests (GTest)
├── irq/                # Host-side IRQ dispatch tests (GTest)
scripts/run.sh          # Build + QEMU boot
third_party/limine/     # Limine protocol header
```
