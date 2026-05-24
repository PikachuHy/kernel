#pragma once
#include <stdint.h>

using timer_callback_t = bool (*)(uint64_t elapsed_ms);

auto timer_init(uint64_t hhdm) -> void;
auto timer_oneshot(uint64_t delay_us, timer_callback_t cb) -> void;
auto timer_periodic(uint64_t interval_us, timer_callback_t cb) -> void;
auto timer_uptime_ms() -> uint64_t;
auto timer_ticks_per_ms() -> uint64_t;
