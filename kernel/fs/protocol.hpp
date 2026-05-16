#pragma once
#include <stdint.h>
#include <stddef.h>

// File operation message sent over a file's Channel.
// For Open (on the mount Channel): path follows in the data payload.
struct FileMsg {
    enum Op : uint32_t {
        Open    = 0,
        Read    = 1,
        Write   = 2,
        Seek    = 3,
        Stat    = 4,
        Close   = 5,
        Readdir = 6,
    };
    Op       op;
    uint32_t flags;      // open flags (O_CREAT, O_RDONLY, O_WRONLY, O_RDWR)
    uint64_t offset;     // seek offset, or readdir cookie
    uint64_t length;     // bytes to read/write
};

// Open flags
constexpr uint32_t O_RDONLY = 1 << 0;
constexpr uint32_t O_WRONLY = 1 << 1;
constexpr uint32_t O_RDWR   = 1 << 2;
constexpr uint32_t O_CREAT  = 1 << 3;

// Response from FS server for any file operation.
struct FileResponse {
    int32_t  result;     // 0 = success, negative = errno
    uint64_t size;       // bytes transferred, file size for stat, new offset for seek
};

struct Stat {
    uint64_t size;
    uint32_t type;       // 0 = file, 1 = directory
    uint32_t padding;
};

struct Dirent {
    char     name[256];
    uint32_t type;       // 0 = file, 1 = directory
    uint64_t size;
};

// Open message sent from kernel to FS server via the mount Channel.
// The kernel pre-allocates handles in the FS server's handle table.
struct OpenPayload {
    char     path[256];
    uint32_t file_handle;   // FS server: use for file I/O (Read/Write/Close)
    uint32_t ack_handle;    // FS server: write ack here (kernel reads it)
    uint32_t flags;
};
