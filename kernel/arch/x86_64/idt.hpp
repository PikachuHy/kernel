#pragma once
#include <stdint.h>

auto idt_init() -> void;
auto idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr) -> void;
