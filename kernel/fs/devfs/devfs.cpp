// kernel/fs/devfs/devfs.cpp
// devfs filesystem server -- ring-3 process at 0x500000
// Handle 0 = mount Channel (pre-allocated by kernel).
// Kernel sends OpenPayload{path, file_handle} for each open.
// file_handle is pre-allocated in OUR handle table -- use it directly.

#include "kernel/lib/user_types.hpp"

// -- Syscall numbers ----------------------------------------------------------
constexpr int SYS_DEBUG_PRINT          = 0;
constexpr int SYS_CHANNEL_WRITE        = 11;
constexpr int SYS_CHANNEL_READ         = 12;
constexpr int SYS_HANDLE_CLOSE         = 1;
constexpr int SYS_PROCESS_EXIT         = 31;
constexpr int SYS_BLKDEV_READ          = 52;
constexpr int SYS_BLKDEV_WRITE         = 53;

// -- Protocol types (must match kernel/fs/protocol.hpp) -----------------------
struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op       op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};

struct FileResponse {
    int32_t  result;
    uint64_t size;
};

struct Stat {
    uint64_t size;
    uint32_t type;       // 0 = file, 1 = directory
    uint32_t padding;
};

struct OpenPayload {
    char     path[256];
    uint32_t file_handle;
    uint32_t flags;
};

// -- Syscall wrappers ---------------------------------------------------------
static auto syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5) -> uint64_t {
    uint64_t ret;
    asm volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return ret;
}

static auto debug(const char* msg) -> void {
    syscall6(SYS_DEBUG_PRINT, (uint64_t)msg, 0, 0, 0, 0);
}

static auto channel_write(uint32_t h, const void* data, size_t len) -> int {
    struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
    WA a = {data, len, nullptr, 0};
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&a, 0, 0, 0);
}

static auto channel_read(uint32_t h, void* buf, size_t buf_size) -> int {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)buf, buf_size, 0, 0);
}

static auto handle_close(uint32_t h) -> void {
    syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
}

// -- Device handlers ----------------------------------------------------------

static auto handle_null(uint32_t file_chan) -> void {
    while (true) {
        uint8_t buf[4096];
        int rc = channel_read(file_chan, buf, sizeof(buf));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(buf);

        FileResponse resp = {};
        switch (msg->op) {
        case FileMsg::Read:
            resp.result = 0;
            resp.size = 0;  // EOF
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Write:
            resp.result = 0;
            resp.size = msg->length;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Stat: {
            Stat st = {0, 0, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Close:
            handle_close(file_chan);
            return;
        default:
            resp.result = -1;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
    }
}

static auto handle_zero(uint32_t file_chan) -> void {
    while (true) {
        uint8_t buf[4096];
        int rc = channel_read(file_chan, buf, sizeof(buf));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(buf);

        FileResponse resp = {};
        switch (msg->op) {
        case FileMsg::Read: {
            size_t count = msg->length;
            if (count > 4096 - sizeof(FileResponse))
                count = 4096 - sizeof(FileResponse);
            uint8_t out[sizeof(FileResponse) + 4096];
            *(FileResponse*)out = {0, count};
            for (size_t i = 0; i < count; i++)
                out[sizeof(FileResponse) + i] = 0;
            channel_write(file_chan, out, sizeof(FileResponse) + count);
            break;
        }
        case FileMsg::Write:
            resp.result = 0;
            resp.size = msg->length;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Stat: {
            Stat st = {0, 0, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Close:
            handle_close(file_chan);
            return;
        default:
            resp.result = -1;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
    }
}

static auto handle_console(uint32_t file_chan) -> void {
    while (true) {
        uint8_t buf[4096];
        int rc = channel_read(file_chan, buf, sizeof(buf));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(buf);
        uint8_t* data = buf + sizeof(FileMsg);
        size_t data_len = (size_t)rc - sizeof(FileMsg);

        FileResponse resp = {};
        switch (msg->op) {
        case FileMsg::Write:
            // Print write data via debug_print
            for (size_t i = 0; i < data_len && i < msg->length; i++) {
                char c[2] = { (char)data[i], '\0' };
                debug(c);
            }
            resp.result = 0;
            resp.size = msg->length;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Read:
            resp.result = -1;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Stat: {
            Stat st = {0, 0, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Close:
            handle_close(file_chan);
            return;
        default:
            resp.result = -1;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
    }
}

// -- Block device handler -----------------------------------------------------
static auto handle_block(uint32_t file_chan, const char* dev_name) -> void {
    // Read sector 0 to verify the block device is accessible
    uint8_t probe[512];
    int rc = (int)syscall6(SYS_BLKDEV_READ, (uint64_t)dev_name, 0,
                           (uint64_t)probe, 1, 0);
    if (rc != 0) {
        FileResponse err = {-1, 0};
        channel_write(file_chan, &err, sizeof(err));
        handle_close(file_chan);
        return;
    }

    while (true) {
        uint8_t buf[4096];
        rc = channel_read(file_chan, buf, sizeof(buf));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(buf);
        uint8_t* data = buf + sizeof(FileMsg);
        size_t data_len = (size_t)rc - sizeof(FileMsg);

        switch (msg->op) {
        case FileMsg::Read: {
            // msg->offset = LBA, msg->length = sector count
            size_t sectors = msg->length;
            if (sectors == 0) sectors = 1;
            if (sectors > 8) sectors = 8;  // max 4KB per read

            uint8_t rbuf[4096];
            int r = (int)syscall6(SYS_BLKDEV_READ, (uint64_t)dev_name,
                                  msg->offset, (uint64_t)rbuf, sectors, 0);
            if (r != 0) {
                FileResponse resp = {(int32_t)-1, 0};
                channel_write(file_chan, &resp, sizeof(resp));
            } else {
                uint8_t out[sizeof(FileResponse) + 4096];
                *(FileResponse*)out = {0, sectors * 512};
                for (size_t i = 0; i < sectors * 512; i++)
                    out[sizeof(FileResponse) + i] = rbuf[i];
                channel_write(file_chan, out, sizeof(FileResponse) + sectors * 512);
            }
            break;
        }
        case FileMsg::Write: {
            // msg->offset = LBA, msg->length = bytes to write
            size_t count = msg->length;
            if (count > data_len) count = data_len;
            if (count == 0) {
                FileResponse resp = {-1, 0};
                channel_write(file_chan, &resp, sizeof(resp));
                break;
            }
            size_t sectors = (count + 511) / 512;
            int r = (int)syscall6(SYS_BLKDEV_WRITE, (uint64_t)dev_name,
                                  msg->offset, (uint64_t)data, sectors, 0);
            FileResponse resp = {r, r == 0 ? count : 0u};
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
        case FileMsg::Stat: {
            // Return block device as a large file
            Stat st = {0xFFFFFFFFFFFFFFFFULL, 0, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Close:
            handle_close(file_chan);
            return;
        default: {
            FileResponse resp = {-1, 0};
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
        }
    }
}

// -- Entry point --------------------------------------------------------------
extern "C" void _start() {
    const uint32_t MOUNT_CHAN = 1;

    while (true) {
        uint8_t buf[264];
        int rc = channel_read(MOUNT_CHAN, buf, sizeof(buf));
        if (rc < 0) break;

        OpenPayload* payload = reinterpret_cast<OpenPayload*>(buf);
        uint32_t file_handle = payload->file_handle;
        const char* rel = payload->path;

        // Strip leading "/dev/" prefix if present
        if (rel[0] == '/' && rel[1] == 'd' && rel[2] == 'e' &&
            rel[3] == 'v' && rel[4] == '/')
            rel += 5;

        // Ack on file Channel before dispatching to handler
        FileResponse resp = {0, 0};
        channel_write(file_handle, &resp, sizeof(resp));

        // Dispatch to device handler
        if (rel[0] == 'n' && rel[1] == 'u' && rel[2] == 'l' &&
            rel[3] == 'l' && rel[4] == '\0') {
            handle_null(file_handle);
        } else if (rel[0] == 'z' && rel[1] == 'e' && rel[2] == 'r' &&
                   rel[3] == 'o' && rel[4] == '\0') {
            handle_zero(file_handle);
        } else if (rel[0] == 'c' && rel[1] == 'o' && rel[2] == 'n' &&
                   rel[3] == 's' && rel[4] == 'o' && rel[5] == 'l' &&
                   rel[6] == 'e' && rel[7] == '\0') {
            handle_console(file_handle);
        } else {
            // Unknown device: try block device (e.g., ahci0)
            handle_block(file_handle, rel);
        }
    }

    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
