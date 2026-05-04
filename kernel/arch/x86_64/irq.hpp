#pragma once
#include <stdint.h>

using irq_handler_t = bool (*)(uint8_t vector);

void irq_init();
int  irq_register(uint8_t irq, irq_handler_t handler);
extern "C" void irq_dispatch(uint8_t vector);
