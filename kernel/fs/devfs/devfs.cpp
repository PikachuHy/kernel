// kernel/fs/devfs/devfs.cpp
// devfs filesystem server -- ring-3 process at 0x500000
// Handle 0 = mount Channel (pre-allocated by kernel).
// Kernel sends OpenPayload{path, file_handle} for each open.
// file_handle is pre-allocated in OUR handle table -- use it directly.

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using int32_t  = int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

// -- Syscall numbers ----------------------------------------------------------
constexpr int SYS_DEBUG_PRINT          = 0;
constexpr int SYS_CHANNEL_WRITE        = 11;
constexpr int SYS_CHANNEL_READ         = 12;
constexpr int SYS_HANDLE_CLOSE         = 1;
constexpr int SYS_PROCESS_EXIT         = 31;

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
    uint32_t ack_handle;
    uint32_t flags;
};

// -- Syscall wrappers ---------------------------------------------------------
static uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
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

static void debug(const char* msg) {
    syscall6(SYS_DEBUG_PRINT, (uint64_t)msg, 0, 0, 0, 0);
}

static int channel_write(uint32_t h, const void* data, size_t len) {
    struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
    WA a = {data, len, nullptr, 0};
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&a, 0, 0, 0);
}

static int channel_read(uint32_t h, void* buf, size_t buf_size) {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)buf, buf_size, 0, 0);
}

static void handle_close(uint32_t h) {
    syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
}

// -- Device handlers ----------------------------------------------------------

__attribute__((unused)) static void handle_null(uint32_t file_chan) {
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

__attribute__((unused)) static void handle_zero(uint32_t file_chan) {
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

__attribute__((unused)) static void handle_console(uint32_t file_chan) {
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

// -- Entry point --------------------------------------------------------------
extern "C" void _start() {
    debug("devfs: starting\n");
    const uint32_t MOUNT_CHAN = 1;

    // Wait for the first Open request
    uint8_t dummy[sizeof(OpenPayload)];
    int rc = channel_read(MOUNT_CHAN, dummy, sizeof(dummy));
    if (rc < 0) {
        debug("devfs: read error\n");
        syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
        while(1) asm volatile("hlt");
    }

    debug("devfs: got open\n");

    uint32_t file_handle = *(uint32_t*)(dummy + 256);
    uint32_t ack_handle = *(uint32_t*)(dummy + 260);
    (void)file_handle;

    // Ack on ack Channel
    FileResponse resp = {0, 0};
    channel_write(ack_handle, &resp, sizeof(resp));

    debug("devfs: ack sent\n");

    // Close ack handle
    handle_close(ack_handle);

    debug("devfs: done\n");
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
