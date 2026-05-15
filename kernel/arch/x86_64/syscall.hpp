#pragma once
#include <stdint.h>

using syscall_handler_t = uint64_t (*)(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4);

void syscall_init();
void syscall_set_handler(syscall_handler_t handler);

// Syscall numbers
constexpr uint64_t SYSCALL_DEBUG_PRINT    = 0;
constexpr uint64_t SYSCALL_HANDLE_CLOSE   = 1;
constexpr uint64_t SYSCALL_HANDLE_DUP     = 2;
constexpr uint64_t SYSCALL_CHANNEL_CREATE = 10;
constexpr uint64_t SYSCALL_CHANNEL_WRITE  = 11;
constexpr uint64_t SYSCALL_CHANNEL_READ   = 12;
constexpr uint64_t SYSCALL_CHANNEL_READ_HANDLES = 13;
constexpr uint64_t SYSCALL_PORT_CREATE    = 20;
constexpr uint64_t SYSCALL_PORT_REGISTER  = 21;
constexpr uint64_t SYSCALL_PORT_CONNECT   = 22;
constexpr uint64_t SYSCALL_PORT_ACCEPT    = 23;

// Process
constexpr uint64_t SYSCALL_PROCESS_CREATE  = 30;
constexpr uint64_t SYSCALL_PROCESS_EXIT    = 31;

// VMO
constexpr uint64_t SYSCALL_VMO_CREATE      = 40;
constexpr uint64_t SYSCALL_VMO_MAP         = 41;

// VFS
constexpr uint64_t SYSCALL_OPEN            = 50;
constexpr uint64_t SYSCALL_MOUNT           = 51;
