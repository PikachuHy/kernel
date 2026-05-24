// kernel/fs/tmpfs/tmpfs.cpp
// tmpfs filesystem server — ring-3 process at 0x600000
// In-memory VMO-backed filesystem. Handle 0 = mount Channel.

#include "kernel/lib/user_types.hpp"

// ── Syscall numbers ──────────────────────────────────────────────
constexpr int SYS_CHANNEL_WRITE        = 11;
constexpr int SYS_CHANNEL_READ         = 12;
constexpr int SYS_HANDLE_CLOSE         = 1;
constexpr int SYS_PROCESS_EXIT         = 31;
constexpr int SYS_VMO_CREATE           = 40;
constexpr int SYS_VMO_MAP              = 41;

// ── Protocol types ───────────────────────────────────────────────
struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op       op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};
struct FileResponse { int32_t result; uint64_t size; };
struct Stat { uint64_t size; uint32_t type; uint32_t padding; };
struct Dirent { char name[256]; uint32_t type; uint64_t size; };
struct OpenPayload { char path[256]; uint32_t file_handle; uint32_t flags; };

constexpr uint32_t O_CREAT  = 1 << 3;

// ── Syscall wrappers ─────────────────────────────────────────────
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

static auto channel_read(uint32_t h, void* buf, size_t sz) -> int {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)buf, sz, 0, 0);
}

static auto channel_write(uint32_t h, const void* data, size_t sz) -> int {
    struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
    WA a = {data, sz, nullptr, 0};
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&a, 0, 0, 0);
}

static auto vmo_create(uint64_t size) -> uint32_t {
    return (uint32_t)syscall6(SYS_VMO_CREATE, size, 0, 0, 0, 0);
}

static auto vmo_map(uint32_t vmo_handle, uint64_t va, uint64_t flags, uint64_t off) -> int {
    return (int)syscall6(SYS_VMO_MAP, vmo_handle, va, flags, off, 0);
}

static auto handle_close(uint32_t h) -> void { syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0); }

// ── In-memory directory ──────────────────────────────────────────
struct DirEntry {
    char     name[256];
    uint32_t vmo_handle;
    bool     is_dir;
    uint64_t size;       // file size in bytes
    DirEntry* next;
};

static DirEntry* root = nullptr;
static DirEntry pool[64];
static int pool_idx = 0;

static auto find_entry(const char* name) -> DirEntry* {
    for (DirEntry* e = root; e; e = e->next) {
        int i = 0;
        while (name[i] && e->name[i] && name[i] == e->name[i]) i++;
        if (name[i] == '\0' && e->name[i] == '\0') return e;
    }
    return nullptr;
}

static auto add_entry(const char* name, uint32_t vmo_h, bool dir, uint64_t sz) -> DirEntry* {
    if (pool_idx >= 64) return nullptr;
    DirEntry* e = &pool[pool_idx++];
    int i = 0;
    while (name[i] && i < 255) { e->name[i] = name[i]; i++; }
    e->name[i] = '\0';
    e->vmo_handle = vmo_h;
    e->is_dir = dir;
    e->size = sz;
    e->next = root;
    root = e;
    return e;
}

// ── Per-open file state ──────────────────────────────────────────
struct FileState {
    uint32_t  file_chan;
    uint32_t  vmo_handle;
    uint64_t  cursor;
    bool      is_dir;
    DirEntry* entry;     // pointer to the directory entry (for size)
    FileState* next;
};

static FileState* file_list = nullptr;
static FileState fspool[64];
static int fspool_idx = 0;

static auto alloc_file_state(uint32_t fchan, uint32_t vmo, bool dir, DirEntry* e) -> FileState* {
    if (fspool_idx >= 64) return nullptr;
    FileState* fs = &fspool[fspool_idx++];
    fs->file_chan = fchan;
    fs->vmo_handle = vmo;
    fs->cursor = 0;
    fs->is_dir = dir;
    fs->entry = e;
    fs->next = file_list;
    file_list = fs;
    return fs;
}

// ── File handler ─────────────────────────────────────────────────
static auto handle_file(FileState* fs) -> void {
    // Map the VMO at a fixed address
    const uint64_t MAP_BASE = 0x70000000000ULL;
    if (fs->vmo_handle) {
        vmo_map(fs->vmo_handle, MAP_BASE, 0x3, 0);  // RW
    }

    while (true) {
        uint8_t buf[4096];
        int rc = channel_read(fs->file_chan, buf, sizeof(buf));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(buf);
        uint8_t* data = buf + sizeof(FileMsg);
        size_t data_len = (size_t)rc - sizeof(FileMsg);

        FileResponse resp = {};

        uint64_t file_size = fs->entry ? fs->entry->size : 0;

        switch (msg->op) {
        case FileMsg::Read: {
            size_t count = msg->length;
            if (count > 4096 - sizeof(FileResponse))
                count = 4096 - sizeof(FileResponse);
            if (msg->offset + count > file_size)
                count = file_size > msg->offset ? file_size - msg->offset : 0;

            uint8_t out[sizeof(FileResponse) + 4096];
            *(FileResponse*)out = {0, count};
            uint8_t* src = reinterpret_cast<uint8_t*>(MAP_BASE + msg->offset);
            for (size_t i = 0; i < count; i++)
                out[sizeof(FileResponse) + i] = src[i];
            channel_write(fs->file_chan, out, sizeof(FileResponse) + count);
            break;
        }
        case FileMsg::Write: {
            size_t count = data_len;
            if (msg->offset + count > file_size) {
                // Extend file up to VMO capacity (4096 bytes)
                uint64_t new_sz = msg->offset + count;
                if (new_sz > 4096) {
                    if (msg->offset < 4096)
                        count = 4096 - msg->offset;
                    else
                        count = 0;
                    new_sz = 4096;
                }
                if (fs->entry) fs->entry->size = new_sz;
            }
            uint8_t* dst = reinterpret_cast<uint8_t*>(MAP_BASE + msg->offset);
            for (size_t i = 0; i < count; i++) dst[i] = data[i];
            resp.result = 0;
            resp.size = count;
            channel_write(fs->file_chan, &resp, sizeof(resp));
            break;
        }
        case FileMsg::Seek:
            fs->cursor = msg->offset;
            if (fs->cursor > file_size) fs->cursor = file_size;
            resp.result = 0;
            resp.size = fs->cursor;
            channel_write(fs->file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Stat: {
            Stat st = {file_size, fs->is_dir ? 1u : 0u, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(fs->file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Readdir:
            resp.result = -1;  // not yet implemented
            channel_write(fs->file_chan, &resp, sizeof(resp));
            break;
        case FileMsg::Close:
            handle_close(fs->file_chan);
            return;
        default:
            resp.result = -1;
            channel_write(fs->file_chan, &resp, sizeof(resp));
            break;
        }
    }
}

// ── Entry point ──────────────────────────────────────────────────
extern "C" void _start() {
    const uint32_t MOUNT_CHAN = 1;

    while (true) {
        uint8_t buf[264];
        int rc = channel_read(MOUNT_CHAN, buf, sizeof(buf));
        if (rc < 0) break;

        OpenPayload* payload = reinterpret_cast<OpenPayload*>(buf);
        uint32_t file_handle = payload->file_handle;
        const char* rel = payload->path;

        // Strip leading '/'
        while (*rel == '/') rel++;

        // Look up existing entry
        DirEntry* entry = find_entry(rel);

        // Create if O_CREAT
        if (!entry && (payload->flags & O_CREAT)) {
            uint32_t vmo = vmo_create(4096);
            if (vmo) {
                entry = add_entry(rel, vmo, false, 0);
            }
        }

        if (!entry) {
            // No such file
            FileResponse resp = {-1, 0};
            channel_write(file_handle, &resp, sizeof(resp));
            handle_close(file_handle);
            continue;
        }

        // Allocate per-open state
        FileState* fs = alloc_file_state(file_handle, entry->vmo_handle,
                                          entry->is_dir, entry);
        if (!fs) {
            FileResponse resp = {-1, 0};
            channel_write(file_handle, &resp, sizeof(resp));
            handle_close(file_handle);
            continue;
        }

        // Ack success on file Channel
        FileResponse resp = {0, 0};
        channel_write(file_handle, &resp, sizeof(resp));

        // Enter per-file operation loop (blocks until Close)
        handle_file(fs);
    }

    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
