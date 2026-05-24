#pragma once
#include <stdint.h>

constexpr int MAX_CPUS = 16;

struct PerCpu {
    uint32_t cpu_id;
    uint32_t lapic_id;
    bool online;        // set true when AP reaches idle loop
    uint64_t reserved;  // padding (future: stack ptr, idle thread ptr)
};

static_assert(sizeof(PerCpu) == 24, "PerCpu size mismatch");

struct Spinlock {
    uint8_t locked;  // 0=free, 1=held
};

auto spinlock_acquire(Spinlock* lock) -> void;
auto spinlock_release(Spinlock* lock) -> void;

struct ScopedSpinlock {
    Spinlock* lock;
    explicit ScopedSpinlock(Spinlock& l) : lock(&l) { spinlock_acquire(lock); }
    ~ScopedSpinlock() { spinlock_release(lock); }

    ScopedSpinlock(const ScopedSpinlock&) = delete;
    auto operator=(const ScopedSpinlock&) -> ScopedSpinlock& = delete;
    ScopedSpinlock(ScopedSpinlock&&) = delete;
    auto operator=(ScopedSpinlock&&) -> ScopedSpinlock& = delete;
};

// Global per-CPU array
extern PerCpu g_per_cpu[MAX_CPUS];
extern uint32_t g_cpu_count;

// SMP init — called by BSP after memory, APIC, and timer are set up.
// hhdm: Limine HHDM offset for physical memory access
// rsdp_phys: physical address of ACPI RSDP (from Limine rsdp_request)
// Returns number of CPUs successfully brought online (including BSP).
auto smp_init(uint64_t hhdm, uint64_t rsdp_phys) -> uint32_t;

auto smp_cpu_count() -> uint32_t;

// AP entry point (called from trampoline after long-mode transition).
// id = cpu_id | (lapic_id << 32).  Defined in smp.cpp (Task 4+).
extern "C" auto smp_ap_entry(uint64_t id) -> void;
