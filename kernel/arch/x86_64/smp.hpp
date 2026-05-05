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
    uint32_t locked;  // 0=free, 1=held
};

void spinlock_acquire(Spinlock* lock);
void spinlock_release(Spinlock* lock);

struct ScopedSpinlock {
    Spinlock* lock;
    explicit ScopedSpinlock(Spinlock& l) : lock(&l) { spinlock_acquire(lock); }
    ~ScopedSpinlock() { spinlock_release(lock); }
};

// Global per-CPU array
extern PerCpu g_per_cpu[MAX_CPUS];
extern uint32_t g_cpu_count;

// SMP init — called by BSP after Phase 3 setup.
// Parameters and return type TBD in Task 4.
uint32_t smp_init();

uint32_t smp_cpu_count();
