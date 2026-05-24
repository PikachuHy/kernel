#pragma once
#include <stdint.h>

// Transition from ring 0 to ring 3. Never returns.
// entry_rip: user-space instruction pointer
// user_rsp:  initial user stack pointer (top of stack, grows down)
[[noreturn]] auto enter_usermode(uint64_t entry_rip, uint64_t user_rsp) -> void;
