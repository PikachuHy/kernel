#include "kernel/lib/spinlock.hpp"

auto SpinLock::lock() -> void {
    while (__sync_lock_test_and_set(&locked_, 1)) {
        while (locked_) {
            asm volatile("pause");
        }
    }
}

auto SpinLock::unlock() -> void {
    __sync_lock_release(&locked_);
}

auto SpinLock::try_lock() -> bool {
    return __sync_lock_test_and_set(&locked_, 1) == 0;
}
