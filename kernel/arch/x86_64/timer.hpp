#pragma once
#include <stdint.h>

using timer_callback_t = bool (*)(uint64_t elapsed_ms);

void timer_init(uint64_t hhdm);
void timer_oneshot(uint64_t delay_us, timer_callback_t cb);
void timer_periodic(uint64_t interval_us, timer_callback_t cb);
uint64_t timer_uptime_ms();
uint64_t timer_ticks_per_ms();
