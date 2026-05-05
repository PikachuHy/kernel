#pragma once
#include <stdint.h>

class SpinLock {
public:
    void lock();
    void unlock();
    bool try_lock();

private:
    volatile uint32_t locked_{0};
};
