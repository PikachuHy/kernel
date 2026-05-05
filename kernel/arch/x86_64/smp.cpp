#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/lib/klog.hpp"

PerCpu g_per_cpu[MAX_CPUS];
uint32_t g_cpu_count = 0;

void spinlock_acquire(Spinlock* lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        asm volatile("pause" ::: "memory");
    }
}

void spinlock_release(Spinlock* lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

// Stub — will be properly implemented in Task 4.
extern "C" void smp_ap_entry(uint64_t /*id*/) {
    while (true) { asm volatile("hlt"); }
}

// Placeholder — will be implemented in Task 4
uint32_t smp_init() {
    klog("SMP: smp_init() placeholder\n");
    g_cpu_count = 1;
    g_per_cpu[0].cpu_id = 0;
    g_per_cpu[0].online = true;
    return 1;
}

uint32_t smp_cpu_count() {
    return g_cpu_count;
}
