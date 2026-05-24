#pragma once
#include <stdint.h>

class SpinLock {
public:
    auto lock() -> void;
    auto unlock() -> void;
    auto try_lock() -> bool;

private:
    volatile uint32_t locked_{0};
};
