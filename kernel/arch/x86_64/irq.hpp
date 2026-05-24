#pragma once
#include <stdint.h>

using irq_handler_t = auto (*)(uint8_t vector) -> bool;

auto irq_init() -> void;
auto irq_register(uint8_t irq, irq_handler_t handler) -> int;
extern "C" auto irq_dispatch(uint8_t vector) -> void;
