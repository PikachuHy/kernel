#pragma once
#include <stdint.h>

struct SyscallArgs {
    uint64_t a1, a2, a3, a4;
};

using syscall_handler_t = auto (*)(SyscallArgs args) -> uint64_t;

auto syscall_init() -> void;
auto syscall_set_handler(syscall_handler_t h) -> void;

// Syscall numbers
constexpr int SYSCALL_DEBUG_PRINT    = 0;
constexpr int SYSCALL_HANDLE_CLOSE   = 1;
constexpr int SYSCALL_HANDLE_DUP     = 2;
constexpr int SYSCALL_CHANNEL_CREATE = 10;
constexpr int SYSCALL_CHANNEL_WRITE  = 11;
constexpr int SYSCALL_CHANNEL_READ   = 12;
constexpr int SYSCALL_CHANNEL_READ_HANDLES = 13;
constexpr int SYSCALL_PORT_CREATE    = 20;
constexpr int SYSCALL_PORT_REGISTER  = 21;
constexpr int SYSCALL_PORT_CONNECT   = 22;
constexpr int SYSCALL_PORT_ACCEPT    = 23;

// Process
constexpr int SYSCALL_PROCESS_CREATE  = 30;
constexpr int SYSCALL_PROCESS_EXIT    = 31;

// VMO
constexpr int SYSCALL_VMO_CREATE      = 40;
constexpr int SYSCALL_VMO_MAP         = 41;

// VFS
constexpr int SYSCALL_OPEN            = 50;
constexpr int SYSCALL_MOUNT           = 51;

// Block Device
constexpr int SYSCALL_BLKDEV_READ     = 52;
constexpr int SYSCALL_BLKDEV_WRITE    = 53;

// Serial I/O
constexpr int SYSCALL_SERIAL_READ     = 54;

extern "C" auto syscall_entry() -> void;
extern "C" auto syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4) -> uint64_t;
