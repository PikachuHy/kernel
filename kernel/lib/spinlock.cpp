#include "kernel/lib/spinlock.hpp"

void SpinLock::lock() {
    while (__sync_lock_test_and_set(&locked_, 1)) {
        while (locked_) {
            asm volatile("pause");
        }
    }
}

void SpinLock::unlock() {
    __sync_lock_release(&locked_);
}

bool SpinLock::try_lock() {
    return __sync_lock_test_and_set(&locked_, 1) == 0;
}
