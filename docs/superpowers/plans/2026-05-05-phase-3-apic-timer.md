# Phase 3: APIC, Timer, and Interrupt Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Initialize Local APIC and I/O APIC, calibrate the LAPIC timer against PIT, set up IRQ dispatch with keyboard IRQ routed, and wire up syscall entry/exit — all verified via QEMU boot output with a 3-tick timer demo.

**Architecture:** Hardware-specific code lives in `kernel/arch/x86_64/` following the existing pattern: one .hpp/.cpp pair per subsystem (apic, irq, timer, syscall). Shared primitives (I/O port helpers, MSR access, MMIO helpers) go into `io.hpp` and `msr.hpp` as inline functions. IRQ dispatch uses a static array of handler lists per IRQ line. LAPIC timer calibrated against PIT once at boot; one-shot and periodic modes are supported. Syscall entry uses standard x86-64 MSR setup (STAR/LSTAR/SFMASK) with a temporary kernel stack and swapgs.

**Tech Stack:** LLVM/Clang cross-compilation, Bazel 9, Limine native protocol, QEMU. No new dependencies.

---

## File Structure

```
kernel/arch/x86_64/
├── io.hpp              NEW - x86 I/O port and MMIO inline helpers
├── msr.hpp             NEW - rdmsr/wrmsr inline wrappers
├── apic.hpp            NEW - LAPIC + IOAPIC constants and interfaces
├── apic.cpp            NEW - LAPIC init, IOAPIC init, PIC disable, IRQ routing
├── irq.hpp             NEW - IRQ handler type, registration, dispatch
├── irq.cpp             NEW - dispatch table, shared IRQ support, EOI
├── irq_stubs.S         NEW - per-vector IRQ assembly stubs (vectors 32-47)
├── timer.hpp           NEW - timer callback type, oneshot/periodic API
├── timer.cpp           NEW - PIT calibration, LAPIC timer modes
├── syscall.hpp         NEW - syscall handler type, init, set_handler
├── syscall.cpp         NEW - MSR setup (STAR/LSTAR/SFMASK/EFER), dispatcher
├── syscall_entry.S     NEW - syscall/sysret assembly entry/exit with swapgs
├── idt.hpp             MODIFY - expose idt_set_gate()
├── idt.cpp             MODIFY - add IRQ gate setup (vectors 32-47), rename set_entry to idt_set_gate
├── boot.cpp            MODIFY - call Phase 3 init functions, timer demo (3 ticks)
├── BUILD.bazel         MODIFY - add new .cpp/.S/.hpp files
kernel/lib/
├── serial.cpp          MODIFY - use io.hpp instead of local inb/outb
├── BUILD.bazel         MODIFY - add arch dep for io.hpp
test/
├── BUILD.bazel         MODIFY - add irq test target
├── irq/
│   ├── BUILD.bazel     NEW
│   └── irq_test.cpp    NEW - register, dispatch, shared IRQ, ordering
```

---

### Task 1: I/O port and MSR primitives

**Files:**
- Create: `kernel/arch/x86_64/io.hpp`
- Create: `kernel/arch/x86_64/msr.hpp`
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add `"io.hpp"` and `"msr.hpp"` to hdrs
- Modify: `kernel/lib/serial.cpp` — use `io.hpp` instead of local inb/outb
- Modify: `kernel/lib/BUILD.bazel` — add `//kernel/arch/x86_64:arch` to deps

- [ ] **Step 1: Write io.hpp**

```cpp
#pragma once
#include <stdint.h>

namespace x86 {

inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
inline uint32_t inl(uint16_t port) {
    uint32_t result;
    asm volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}
inline uint32_t mmio_read32(uint64_t hhdm, uint64_t phys) {
    return *reinterpret_cast<volatile uint32_t*>(hhdm + phys);
}
inline void mmio_write32(uint64_t hhdm, uint64_t phys, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(hhdm + phys) = val;
}
inline void pause() { asm volatile("pause"); }

} // namespace x86
```

- [ ] **Step 2: Write msr.hpp**

```cpp
#pragma once
#include <stdint.h>

namespace x86 {

inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}
inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

} // namespace x86
```

- [ ] **Step 3: Update BUILD files and serial.cpp**

In `kernel/arch/x86_64/BUILD.bazel`, add `"io.hpp"` and `"msr.hpp"` to `hdrs`.

In `kernel/lib/serial.cpp`:
- Add `#include "kernel/arch/x86_64/io.hpp"`
- Remove local `outb()` and `inb()` from anonymous namespace
- Replace `outb(...)` → `x86::outb(...)`, `inb(...)` → `x86::inb(...)`, `asm volatile("pause")` → `x86::pause()`
- Keep only `constexpr uint16_t COM1 = 0x3F8;` in anonymous namespace

In `kernel/lib/BUILD.bazel`, add `"//kernel/arch/x86_64:arch"` to deps of `klib`.

- [ ] **Step 4: Build and boot test**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

Run: `bash scripts/run.sh`
Expected: Kernel boots, same output as Phase 2

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/io.hpp kernel/arch/x86_64/msr.hpp kernel/arch/x86_64/BUILD.bazel kernel/lib/serial.cpp kernel/lib/BUILD.bazel
git commit -m "feat: add x86 I/O port and MSR primitives"
```

---

### Task 2: LAPIC initialization

**Files:**
- Create: `kernel/arch/x86_64/apic.hpp`
- Create: `kernel/arch/x86_64/apic.cpp`
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add `"apic.cpp"` to srcs, `"apic.hpp"` to hdrs

- [ ] **Step 1: Write apic.hpp**

```cpp
#pragma once
#include <stdint.h>

// Well-known x86 APIC addresses (QEMU defaults)
constexpr uint64_t LAPIC_BASE_PHYS  = 0xFEE00000;
constexpr uint64_t IOAPIC_BASE_PHYS = 0xFEC00000;

// LAPIC register offsets (all 32-bit)
constexpr uint16_t LAPIC_ID          = 0x020;
constexpr uint16_t LAPIC_VERSION     = 0x030;
constexpr uint16_t LAPIC_TPR         = 0x080;
constexpr uint16_t LAPIC_EOI         = 0x0B0;
constexpr uint16_t LAPIC_SVR         = 0x0F0;
constexpr uint16_t LAPIC_ICR_LO      = 0x300;
constexpr uint16_t LAPIC_ICR_HI      = 0x310;
constexpr uint16_t LAPIC_LVT_TIMER   = 0x320;
constexpr uint16_t LAPIC_LVT_LINT0   = 0x350;
constexpr uint16_t LAPIC_LVT_LINT1   = 0x360;
constexpr uint16_t LAPIC_LVT_ERROR   = 0x370;
constexpr uint16_t LAPIC_TIMER_INIT  = 0x380;
constexpr uint16_t LAPIC_TIMER_CURR  = 0x390;
constexpr uint16_t LAPIC_TIMER_DIV   = 0x3E0;

constexpr uint32_t LAPIC_SVR_ENABLE = 0x1FF;  // spurious vector 0xFF, APIC enabled

// IOAPIC registers (accessed via IOREGSEL + IOWIN)
constexpr uint16_t IOAPIC_IOREGSEL = 0x00;
constexpr uint16_t IOAPIC_IOWIN    = 0x10;
constexpr uint8_t  IOAPIC_ID       = 0x00;
constexpr uint8_t  IOAPIC_VER      = 0x01;
constexpr uint8_t  IOAPIC_REDTBL   = 0x10;

// Legacy PIC ports
constexpr uint16_t PIC1_CMD  = 0x20;
constexpr uint16_t PIC1_DATA = 0x21;
constexpr uint16_t PIC2_CMD  = 0xA0;
constexpr uint16_t PIC2_DATA = 0xA1;

// IRQ→vector mapping: ISA IRQs start at vector 32
constexpr uint8_t IRQ_BASE_VECTOR = 32;

void lapic_init(uint64_t hhdm);
void lapic_eoi();
uint32_t lapic_read(uint16_t offset);
void lapic_write(uint16_t offset, uint32_t value);

void ioapic_init(uint64_t hhdm);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id);
void pic_disable();
```

- [ ] **Step 2: Write apic.cpp**

```cpp
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/klog.hpp"

namespace {
uint64_t g_hhdm = 0;
uint64_t g_lapic_phys  = LAPIC_BASE_PHYS;
uint64_t g_ioapic_phys = IOAPIC_BASE_PHYS;

void ioapic_write(uint8_t reg, uint32_t value) {
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOREGSEL, reg);
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOWIN, value);
}
uint32_t ioapic_read(uint8_t reg) {
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOREGSEL, reg);
    return x86::mmio_read32(g_hhdm, g_ioapic_phys + IOAPIC_IOWIN);
}
} // namespace

// ── LAPIC ──────────────────────────────────────────────────────

void lapic_init(uint64_t hhdm) {
    g_hhdm = hhdm;
    klog("LAPIC: base="); klog_hex(g_lapic_phys); klog("\n");

    uint32_t ver = lapic_read(LAPIC_VERSION);
    klog("LAPIC: version="); klog_hex(ver); klog("\n");

    // Mask all LVT entries (bit 16 = mask)
    lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    lapic_write(LAPIC_LVT_LINT0, 1 << 16);
    lapic_write(LAPIC_LVT_LINT1, 1 << 16);
    lapic_write(LAPIC_LVT_ERROR, 1 << 16);

    lapic_write(LAPIC_TPR, 0);  // accept all interrupts
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE);
    klog("LAPIC: enabled\n");
}

void lapic_eoi() { lapic_write(LAPIC_EOI, 0); }

uint32_t lapic_read(uint16_t offset) {
    return x86::mmio_read32(g_hhdm, g_lapic_phys + offset);
}
void lapic_write(uint16_t offset, uint32_t value) {
    x86::mmio_write32(g_hhdm, g_lapic_phys + offset, value);
}

// ── PIC and I/O APIC ───────────────────────────────────────────

void pic_disable() {
    // ICW1: init + edge + cascade
    x86::outb(PIC1_CMD, 0x11);
    x86::outb(PIC2_CMD, 0x11);
    // ICW2: vector offsets
    x86::outb(PIC1_DATA, 0x20);
    x86::outb(PIC2_DATA, 0x28);
    // ICW3: cascade identity
    x86::outb(PIC1_DATA, 0x04);
    x86::outb(PIC2_DATA, 0x02);
    // ICW4: x86 mode
    x86::outb(PIC1_DATA, 0x01);
    x86::outb(PIC2_DATA, 0x01);
    // Mask all
    x86::outb(PIC1_DATA, 0xFF);
    x86::outb(PIC2_DATA, 0xFF);
    klog("PIC: disabled\n");
}

void ioapic_init(uint64_t hhdm) {
    g_hhdm = hhdm;

    klog("IOAPIC: base="); klog_hex(g_ioapic_phys); klog("\n");

    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint8_t max_redir = ((ver >> 16) & 0xFF) + 1;
    klog("IOAPIC: max_redir="); klog_hex(max_redir); klog("\n");

    // Mask all entries
    for (uint8_t i = 0; i < max_redir; i++) {
        uint32_t lo = ioapic_read(IOAPIC_REDTBL + 2 * i);
        ioapic_write(IOAPIC_REDTBL + 2 * i, lo | (1 << 16));
    }

    // Route IRQ1 (keyboard) -> vector 33
    // Note: vector 32 is reserved for the LAPIC timer (internal interrupt)
    ioapic_route_irq(1, IRQ_BASE_VECTOR + 1, 0);
    klog("IOAPIC: IRQ1->v33 routed\n");
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id) {
    uint8_t lo = IOAPIC_REDTBL + 2 * irq;
    ioapic_write(lo, vector);  // fixed delivery, physical dest, unmasked
    ioapic_write(lo + 1, static_cast<uint32_t>(lapic_id) << 24);
}
```

- [ ] **Step 3: Build**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/apic.hpp kernel/arch/x86_64/apic.cpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add LAPIC and I/O APIC initialization"
```

---

### Task 3: IRQ handler infrastructure

**Files:**
- Create: `kernel/arch/x86_64/irq.hpp`
- Create: `kernel/arch/x86_64/irq.cpp`
- Create: `kernel/arch/x86_64/irq_stubs.S`
- Modify: `kernel/arch/x86_64/idt.hpp` — expose `idt_set_gate()`
- Modify: `kernel/arch/x86_64/idt.cpp` — export `idt_set_gate`, load IRQ gates
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add new files

- [ ] **Step 1: Write irq.hpp**

```cpp
#pragma once
#include <stdint.h>

using irq_handler_t = bool (*)(uint8_t vector);

void irq_init();
int  irq_register(uint8_t irq, irq_handler_t handler);
void irq_dispatch(uint8_t vector);
```

- [ ] **Step 2: Write irq.cpp**

```cpp
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/lib/klog.hpp"

namespace {
constexpr int MAX_IRQ = 16;
constexpr int MAX_PER_IRQ = 4;

struct IrqLine { irq_handler_t h[MAX_PER_IRQ]; int count; };
IrqLine g_irqs[MAX_IRQ];
} // namespace

void irq_init() {
    for (auto& irq : g_irqs) { irq.count = 0; for (auto& h : irq.h) h = nullptr; }
    klog("IRQ: table initialized\n");
}

int irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= MAX_IRQ || !handler) return -1;
    auto& line = g_irqs[irq];
    if (line.count >= MAX_PER_IRQ) return -2;
    line.h[line.count++] = handler;
    return 0;
}

void irq_dispatch(uint8_t vector) {
    if (vector < 32 || vector >= 48) return;
    uint8_t irq = vector - 32;
    if (irq >= MAX_IRQ) return;
    for (int i = 0; i < g_irqs[irq].count; i++) {
        if (g_irqs[irq].h[i]) g_irqs[irq].h[i](vector);
    }
    lapic_eoi();
}
```

- [ ] **Step 3: Write irq_stubs.S**

```asm
.macro IRQ_STUB n
.globl irq_stub_\n
irq_stub_\n:
    pushq $0; pushq $\n
    pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx
    pushq %rsi; pushq %rdi; pushq %rbp
    pushq %r8; pushq %r9; pushq %r10; pushq %r11
    pushq %r12; pushq %r13; pushq %r14; pushq %r15
    movq %rsp, %rdi
    callq irq_dispatch
    popq %r15; popq %r14; popq %r13; popq %r12
    popq %r11; popq %r10; popq %r9; popq %r8
    popq %rbp; popq %rdi; popq %rsi; popq %rdx
    popq %rcx; popq %rbx; popq %rax
    addq $16, %rsp
    iretq
.endm

IRQ_STUB 32; IRQ_STUB 33; IRQ_STUB 34; IRQ_STUB 35
IRQ_STUB 36; IRQ_STUB 37; IRQ_STUB 38; IRQ_STUB 39
IRQ_STUB 40; IRQ_STUB 41; IRQ_STUB 42; IRQ_STUB 43
IRQ_STUB 44; IRQ_STUB 45; IRQ_STUB 46; IRQ_STUB 47
```

- [ ] **Step 4: Modify idt.hpp and idt.cpp**

In `idt.hpp`, add:
```cpp
void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr);
```

In `idt.cpp`:
- Rename `set_entry()` → `idt_set_gate()` and remove from anonymous namespace
- Update all 32 calls in `idt_init()` from `set_entry(...)` to `idt_set_gate(...)`
- At the end of `idt_init()`, add:

```cpp
    // IRQ gates: vectors 32-47, interrupt gate (type_attr = 0x8E)
    extern "C" {
        void irq_stub_32(); void irq_stub_33(); void irq_stub_34(); void irq_stub_35();
        void irq_stub_36(); void irq_stub_37(); void irq_stub_38(); void irq_stub_39();
        void irq_stub_40(); void irq_stub_41(); void irq_stub_42(); void irq_stub_43();
        void irq_stub_44(); void irq_stub_45(); void irq_stub_46(); void irq_stub_47();
    }
    uint64_t stubs[] = {
        (uint64_t)&irq_stub_32, (uint64_t)&irq_stub_33, (uint64_t)&irq_stub_34, (uint64_t)&irq_stub_35,
        (uint64_t)&irq_stub_36, (uint64_t)&irq_stub_37, (uint64_t)&irq_stub_38, (uint64_t)&irq_stub_39,
        (uint64_t)&irq_stub_40, (uint64_t)&irq_stub_41, (uint64_t)&irq_stub_42, (uint64_t)&irq_stub_43,
        (uint64_t)&irq_stub_44, (uint64_t)&irq_stub_45, (uint64_t)&irq_stub_46, (uint64_t)&irq_stub_47,
    };
    for (int i = 0; i < 16; i++) idt_set_gate(32 + i, stubs[i], 0, 0x8E);
```

- [ ] **Step 5: Build and boot test**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

Run: `bash scripts/run.sh`
Expected: Kernel boots (no IRQ activity yet, but IDT gates loaded)

- [ ] **Step 6: Commit**

```bash
git add kernel/arch/x86_64/irq.hpp kernel/arch/x86_64/irq.cpp kernel/arch/x86_64/irq_stubs.S kernel/arch/x86_64/idt.hpp kernel/arch/x86_64/idt.cpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add IRQ handler dispatch infrastructure"
```

---

### Task 4: LAPIC timer with PIT calibration

**Files:**
- Create: `kernel/arch/x86_64/timer.hpp`
- Create: `kernel/arch/x86_64/timer.cpp`
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add `"timer.cpp"` to srcs, `"timer.hpp"` to hdrs

- [ ] **Step 1: Write timer.hpp**

```cpp
#pragma once
#include <stdint.h>

using timer_callback_t = bool (*)(uint64_t elapsed_ms);

void timer_init(uint64_t hhdm);
void timer_oneshot(uint64_t delay_us, timer_callback_t cb);
void timer_periodic(uint64_t interval_us, timer_callback_t cb);
uint64_t timer_uptime_ms();
uint64_t timer_ticks_per_ms();
```

- [ ] **Step 2: Write timer.cpp**

```cpp
#include "kernel/arch/x86_64/timer.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/lib/klog.hpp"

namespace {

constexpr uint16_t PIT_CH0 = 0x40;
constexpr uint16_t PIT_CMD = 0x43;
constexpr uint8_t  TIMER_VEC = 32;
constexpr uint32_t PIT_HZ = 1193182;
constexpr uint32_t CAL_MS = 10;
constexpr uint8_t  LAPIC_DIV = 0b1011;  // divide by 1

uint64_t g_ticks_per_ms = 0;
uint64_t g_uptime_ms = 0;
timer_callback_t g_oneshot_cb = nullptr;
timer_callback_t g_periodic_cb = nullptr;

void lapic_timer_unmask(uint8_t vector, bool periodic) {
    uint32_t lvt = vector;
    if (periodic) lvt |= (1 << 17);  // periodic mode
    lapic_write(LAPIC_LVT_TIMER, lvt);
}

bool timer_handler(uint8_t) {
    g_uptime_ms++;
    if (g_oneshot_cb) {
        auto cb = g_oneshot_cb; g_oneshot_cb = nullptr;
        if (!cb(g_uptime_ms)) lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    }
    if (g_periodic_cb) {
        if (!g_periodic_cb(g_uptime_ms)) {
            g_periodic_cb = nullptr;
            lapic_write(LAPIC_LVT_TIMER, 1 << 16);
        }
    }
    return true;
}

} // namespace

void timer_init(uint64_t hhdm) {
    (void)hhdm;
    g_uptime_ms = 0;
    g_ticks_per_ms = 0;

    // Calibrate against PIT
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // PIT: channel 0, mode 2, CAL_MS ms reload
    uint16_t reload = (PIT_HZ * CAL_MS) / 1000;
    x86::outb(PIT_CMD, 0x34);
    x86::outb(PIT_CH0, reload & 0xFF);
    x86::outb(PIT_CH0, (reload >> 8) & 0xFF);

    // Wait 2 PIT cycles (poll bit 5 of port 0x61)
    for (volatile int i = 0; i < 2; i++) {
        while ((x86::inb(0x61) & 0x20) == 0) x86::pause();
        while ((x86::inb(0x61) & 0x20) != 0) x86::pause();
    }

    uint32_t remaining = lapic_read(LAPIC_TIMER_CURR);
    uint32_t elapsed = 0xFFFFFFFF - remaining;
    g_ticks_per_ms = elapsed / CAL_MS;
    if (g_ticks_per_ms == 0) g_ticks_per_ms = 1;

    klog("Timer: ");
    klog_hex(g_ticks_per_ms);
    klog(" ticks/ms (elapsed ");
    klog_hex(elapsed);
    klog(" over ");
    klog_hex(CAL_MS);
    klog("ms)\n");

    // Mask timer until used
    lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    irq_register(0, timer_handler);
    klog("Timer: hardware init complete\n");
}

void timer_oneshot(uint64_t delay_us, timer_callback_t cb) {
    if (!g_ticks_per_ms || !cb) return;
    g_oneshot_cb = cb;
    uint64_t ticks = (delay_us * g_ticks_per_ms) / 1000;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_timer_unmask(TIMER_VEC, false);
    lapic_write(LAPIC_TIMER_INIT, static_cast<uint32_t>(ticks));
}

void timer_periodic(uint64_t interval_us, timer_callback_t cb) {
    if (!g_ticks_per_ms || !cb) return;
    g_periodic_cb = cb;
    uint64_t ticks = (interval_us * g_ticks_per_ms) / 1000;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_timer_unmask(TIMER_VEC, true);
    lapic_write(LAPIC_TIMER_INIT, static_cast<uint32_t>(ticks));
}

uint64_t timer_uptime_ms() { return g_uptime_ms; }
uint64_t timer_ticks_per_ms() { return g_ticks_per_ms; }
```

- [ ] **Step 3: Build**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add kernel/arch/x86_64/timer.hpp kernel/arch/x86_64/timer.cpp kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add LAPIC timer with PIT calibration"
```

---

### Task 5: Syscall entry/exit

**Files:**
- Create: `kernel/arch/x86_64/syscall.hpp`
- Create: `kernel/arch/x86_64/syscall.cpp`
- Create: `kernel/arch/x86_64/syscall_entry.S`
- Modify: `kernel/arch/x86_64/BUILD.bazel` — add new files

- [ ] **Step 1: Write syscall.hpp**

```cpp
#pragma once
#include <stdint.h>

using syscall_handler_t = uint64_t (*)(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4);

void syscall_init();
void syscall_set_handler(syscall_handler_t handler);
```

- [ ] **Step 2: Write syscall.cpp**

```cpp
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/lib/klog.hpp"

constexpr uint32_t IA32_EFER   = 0xC0000080;
constexpr uint32_t IA32_STAR   = 0xC0000081;
constexpr uint32_t IA32_LSTAR  = 0xC0000082;
constexpr uint32_t IA32_CSTAR  = 0xC0000083;
constexpr uint32_t IA32_SFMASK = 0xC0000084;

namespace {
syscall_handler_t g_handler = nullptr;

extern "C" uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4) {
    if (g_handler) return g_handler(num, a1, a2, a3, a4);
    klog("Syscall #"); klog_hex(num); klog("\n");
    return 0;
}
} // namespace

void syscall_init() {
    uint64_t efer = x86::rdmsr(IA32_EFER);
    x86::wrmsr(IA32_EFER, efer | 1);  // SCE bit

    // STAR: syscall CS = 0x08 (kernel), sysret CS = 0x1B (user compat)
    x86::wrmsr(IA32_STAR, (0x1BULL << 48) | (0x08ULL << 32));

    extern void syscall_entry();
    x86::wrmsr(IA32_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    x86::wrmsr(IA32_CSTAR, 0);
    x86::wrmsr(IA32_SFMASK, (1 << 9) | (1 << 10));  // clear IF, DF on entry

    klog("Syscall: LSTAR=");
    klog_hex(reinterpret_cast<uint64_t>(&syscall_entry));
    klog(" SCE=enabled\n");
}

void syscall_set_handler(syscall_handler_t h) { g_handler = h; }
```

- [ ] **Step 3: Write syscall_entry.S**

```asm
.globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:0           // save user RSP
    leaq temp_kstack_top(%rip), %rsp

    pushq %rcx                  // user RIP (for sysret)
    pushq %r11                  // user RFLAGS
    pushq %r10                  // arg4
    pushq %rdx                  // arg3
    pushq %rsi                  // arg2
    pushq %rdi                  // arg1
    pushq %rax                  // syscall number

    // Call dispatcher(num, arg1, arg2, arg3, arg4)
    movq %rax, %rdi
    popq %rax                   // real num from stack
    pushq %rax
    movq 0(%rsp), %rdi          // num
    movq 8(%rsp), %rsi          // arg1
    movq 16(%rsp), %rdx         // arg2
    movq 24(%rsp), %rcx         // arg3
    movq 32(%rsp), %r8          // arg4
    callq syscall_dispatcher

    addq $40, %rsp              // skip saved regs
    popq %r11
    popq %rcx

    movq %gs:0, %rsp            // restore user RSP
    swapgs
    sysretq

.section .bss
.align 16
temp_kstack:
    .space 16384
temp_kstack_top:
.text
```

- [ ] **Step 4: Build**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/x86_64/syscall.hpp kernel/arch/x86_64/syscall.cpp kernel/arch/x86_64/syscall_entry.S kernel/arch/x86_64/BUILD.bazel
git commit -m "feat: add syscall entry/exit infrastructure"
```

---

### Task 6: Wire into boot sequence with timer demo

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp` — add Phase 3 init + timer demo
- Test: Boot in QEMU and observe timer ticks

- [ ] **Step 1: Update boot.cpp**

After the Phase 2 slab demo block (`klog("  [demo] All allocator tests passed\n\n");`), add:

```cpp
    // ── Phase 3: APIC, Timer, Interrupts ──
    klog("=== Phase 3: APIC + Timer + Interrupts ===\n\n");

    klog("Disabling legacy PIC...\n");
    pic_disable();

    klog("Initializing LAPIC...\n");
    lapic_init(hhdm);

    klog("Initializing I/O APIC...\n");
    ioapic_init(hhdm);

    klog("Initializing IRQ dispatch...\n");
    irq_init();

    klog("Initializing timer...\n");
    timer_init(hhdm);

    klog("Initializing syscall...\n");
    syscall_init();

    // ── Timer demo: 3 periodic ticks ──
    {
        klog("\n  [demo] Periodic timer: 1 tick/sec, 3 ticks...\n");
        static volatile int ticks = 0;
        timer_periodic(1000000, [](uint64_t elapsed) -> bool {
            ticks++;
            klog("  [demo] Tick #");
            klog_hex(ticks);
            klog(" @ ");
            klog_hex(elapsed);
            klog(" ms\n");
            return ticks < 3;
        });

        asm volatile("sti");
        while (ticks < 3) asm volatile("pause");
        asm volatile("cli");

        klog("  [demo] Timer demo done (3 ticks fired)\n\n");
    }

    klog("=== Kernel booted successfully ===\n");
    klog("  (Ctrl+A then X to exit QEMU)\n");

    while (1) { asm volatile("hlt"); }
```

Also add the new includes at the top of boot.cpp:
```cpp
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/timer.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
```

- [ ] **Step 2: Build and boot test**

Run: `bazel build //kernel:kernel`
Expected: Build succeeds

Run: `bash scripts/run.sh`
Expected output should include:
```
=== Phase 3: APIC + Timer + Interrupts ===
Disabling legacy PIC...
PIC: disabled
Initializing LAPIC...
LAPIC: base=0x00000000FEE00000
LAPIC: version=...
LAPIC: enabled
Initializing I/O APIC...
IOAPIC: base=0x00000000FEC00000
IOAPIC: max_redir=0x18
IOAPIC: IRQ1->v33 routed
Initializing IRQ dispatch...
IRQ: table initialized
Initializing timer...
Timer: ... ticks/ms ...
Timer: hardware init complete
Initializing syscall...
Syscall: LSTAR=... SCE=enabled

  [demo] Periodic timer: 1 tick/sec, 3 ticks...
  [demo] Tick #1 @ ... ms
  [demo] Tick #2 @ ... ms
  [demo] Tick #3 @ ... ms
  [demo] Timer demo done (3 ticks fired)

=== Kernel booted successfully ===
```

- [ ] **Step 3: Commit**

```bash
git add kernel/arch/x86_64/boot.cpp
git commit -m "feat: wire Phase 3 (APIC/timer/IRQ/syscall) into boot with timer demo"
```

---

### Task 7: Host-side tests for IRQ dispatch

**Files:**
- Create: `test/irq/BUILD.bazel`
- Create: `test/irq/irq_test.cpp`
- Modify: `test/BUILD.bazel` — add irq test target (if needed, or just reference subdir)

- [ ] **Step 1: Write test/irq/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "irq_test",
    size = "small",
    srcs = ["irq_test.cpp"],
    deps = [
        "//kernel/arch/x86_64:arch",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

- [ ] **Step 2: Write test/irq/irq_test.cpp**

```cpp
#include <gtest/gtest.h>
#include "kernel/arch/x86_64/irq.hpp"

// irq_dispatch and irq_register don't need hardware — they operate on static tables.
// lapic_eoi() does MMIO — we must NOT call it on host. We test only
// the registration and dispatch logic by calling irq_dispatch() with
// a known vector and checking that the handler was invoked.

static int g_call_count = 0;
static uint8_t g_last_vector = 0;

static bool test_handler(uint8_t vector) {
    g_call_count++;
    g_last_vector = vector;
    return true;
}

static bool test_handler2(uint8_t vector) {
    g_call_count += 10;
    g_last_vector = vector;
    return true;
}

TEST(IrqTest, RegisterAndDispatch) {
    irq_init();

    g_call_count = 0;
    g_last_vector = 0;

    int rc = irq_register(1, test_handler);
    EXPECT_EQ(rc, 0);

    irq_dispatch(33);  // vector 33 = IRQ 1
    EXPECT_EQ(g_call_count, 1);
    EXPECT_EQ(g_last_vector, 33);
}

TEST(IrqTest, SharedIrq) {
    irq_init();

    g_call_count = 0;
    irq_register(1, test_handler);
    irq_register(1, test_handler2);

    irq_dispatch(33);
    EXPECT_EQ(g_call_count, 11);  // 1 from test_handler + 10 from test_handler2
    EXPECT_EQ(g_last_vector, 33);
}

TEST(IrqTest, NoHandlerNoCrash) {
    irq_init();
    irq_dispatch(32);  // no handlers registered — must not crash
}

TEST(IrqTest, BadIrqNoCrash) {
    irq_init();
    irq_dispatch(0);    // below range
    irq_dispatch(255);  // above range
    // must not crash
}

TEST(IrqTest, TooManyHandlers) {
    irq_init();
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    irq_register(0, test_handler);
    int rc = irq_register(0, test_handler);  // 5th handler — should fail
    EXPECT_NE(rc, 0);
}

TEST(IrqTest, NullHandler) {
    irq_init();
    int rc = irq_register(0, nullptr);
    EXPECT_NE(rc, 0);
}
```

**Note on host test safety:** `irq_dispatch()` calls `lapic_eoi()` which does MMIO via HHDM. On the host, `g_hhdm` is 0 (set by lapic_init), so the MMIO write at address `0x0 + 0xFEE00000 + 0xB0` will segfault. The stub must guard against this. In `irq.cpp`, modify `irq_dispatch` to skip EOI when `g_hhdm` is 0:

In `irq.cpp`, after the handler loop and before `lapic_eoi()`, add:
```cpp
    // Skip EOI if LAPIC not initialized (host test mode, hhdm == 0)
    // The lapic_eoi function checks a flag; or we guard here.
```

Better: add a file-scope `bool g_lapic_ready = false` set by `apic_init` and checked before EOI. But for simplicity, we can compile the IRQ test with a stub `lapic_eoi`. 

Simplest approach: Add `g_lapic_ready` flag to apic.cpp:

In `apic.cpp`, add after `g_hhdm`:
```cpp
bool g_apic_ready = false;
```

In `lapic_init()`, add `g_apic_ready = true;` at the end. Export via apic.hpp:
```cpp
bool apic_is_ready();
```

In `irq.cpp`, guard the EOI:
```cpp
    bool handled = false;
    for (int i = 0; i < g_irqs[irq].count; i++) {
        if (g_irqs[irq].h[i]) { g_irqs[irq].h[i](vector); handled = true; }
    }
    if (apic_is_ready()) lapic_eoi();
```

Update apic.hpp to add `bool apic_is_ready();` and apic.cpp to implement it.

- [ ] **Step 3: Run tests**

Run: `bazel test //test/irq:irq_test`
Expected: 6/6 tests pass

- [ ] **Step 4: Commit**

```bash
git add test/irq/BUILD.bazel test/irq/irq_test.cpp kernel/arch/x86_64/apic.cpp kernel/arch/x86_64/apic.hpp kernel/arch/x86_64/irq.cpp
git commit -m "test: add host-side IRQ dispatch tests"
```

---

### Task 8: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md` — update Phase 3 status, add new architecture entries

- [ ] **Step 1: Update CLAUDE.md**

Add to the Phase Plan table:
```
| 3: Interrupt + Timer | `docs/superpowers/plans/2026-05-05-phase-3-apic-timer.md` | Done |
```

Add to Architecture tree under `arch/x86_64/`:
```
kernel/
├── arch/x86_64/        # boot, gdt, idt, apic, ioapic, irq, timer, syscall, linker script
├── core/mm/            # pmm, bitmap_alloc, buddy, slab, new_delete
├── lib/                # klog, panic, serial
```

Add to Known Issues (if any) — e.g., keyboard IRQ routed but no driver yet.

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md for Phase 3 completion"
```
