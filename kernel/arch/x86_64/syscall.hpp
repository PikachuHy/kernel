#pragma once
#include <stdint.h>

using syscall_handler_t = uint64_t (*)(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4);

void syscall_init();
void syscall_set_handler(syscall_handler_t handler);
