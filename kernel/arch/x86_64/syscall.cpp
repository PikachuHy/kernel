#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/port.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/sched/sched.hpp"     // current_thread()
#include "kernel/core/object/process.hpp"  // Process
#include "kernel/core/mm/vmo.hpp"          // Vmo
#include "kernel/fs/protocol.hpp"          // FileResponse
#include "kernel/fs/mount.hpp"             // MountEntry, mount_resolve, mount_add
#include "kernel/core/blk/blkdev.hpp"      // BlockDevice, blkdev_find, blkdev_read, blkdev_write

// Placement new for constructing objects in pre-allocated memory
inline void* operator new(size_t, void* p) noexcept { return p; }
inline void* operator new[](size_t, void* p) noexcept { return p; }

extern "C" void syscall_entry();

constexpr uint32_t IA32_EFER   = 0xC0000080;
constexpr uint32_t IA32_STAR   = 0xC0000081;
constexpr uint32_t IA32_LSTAR  = 0xC0000082;
constexpr uint32_t IA32_CSTAR  = 0xC0000083;
constexpr uint32_t IA32_SFMASK = 0xC0000084;

namespace {

syscall_handler_t g_handler = nullptr;

// Get the current process, or nullptr if no thread/process exists
static auto current_process() -> Process* {
    Thread* t = current_thread();
    return t ? t->process : nullptr;
}

// ── Syscall handlers ────────────────────────────────────────────

auto sys_debug_print(SyscallArgs args) -> uint64_t {
    if (args.a1) klog(reinterpret_cast<const char*>(args.a1));
    return 0;
}

auto sys_handle_close(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return -1;
    proc->handles.Free(static_cast<handle_t>(args.a1));
    return 0;
}

auto sys_handle_dup(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    handle_t h = static_cast<handle_t>(args.a1);
    Rights needed{.mask = Rights::Duplicate};
    Rights existing;
    KernelObject* obj = proc->handles.Lookup(h, needed, &existing);
    if (!obj) return INVALID_HANDLE;

    Rights new_rights{.mask = static_cast<uint32_t>(args.a2)};
    new_rights.mask &= existing.mask;
    return proc->handles.Alloc(obj, new_rights);
}

auto sys_channel_create(SyscallArgs) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    auto* ch = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!ch) return INVALID_HANDLE;
    new (ch) Channel();

    Rights full{.mask = Rights::Read | Rights::Write |
                       Rights::Duplicate | Rights::Transfer};
    handle_t a = proc->handles.Alloc(ch, full);  // endpoint A
    full.mask |= Rights::ChannelEndpointB;
    handle_t b = proc->handles.Alloc(ch, full);  // endpoint B
    ch->Release(); // handles own refs

    return (static_cast<uint64_t>(a) << 32) | b;
}

auto sys_channel_write(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return -1;

    Rights existing;
    proc->handles.Lookup(static_cast<handle_t>(args.a1), Rights{}, &existing);
    bool endpoint_b = (existing.mask & Rights::ChannelEndpointB) != 0;

    auto result = typed_lookup<Channel>(proc->handles, static_cast<handle_t>(args.a1),
                                         Rights{.mask = Rights::Write});
    if (!result) return static_cast<uint64_t>(-1);
    auto* ch = result.value();

    // Args packed in struct (4-register ABI can't pass 5 scalars)
    struct ChannelWriteArgs {
        const void* data; size_t data_len;
        const handle_t* handles; size_t num_handles;
    };
    auto* write_args = reinterpret_cast<const ChannelWriteArgs*>(args.a2);

    // Defensive: reject obviously-bogus user pointers (seen as 0x1 from
    // ring-3 processes with corrupted stack frames).
    if (reinterpret_cast<uint64_t>(write_args) < 4096) return -1;

    return ch->Write(
        write_args->data, write_args->data_len, write_args->handles, write_args->num_handles, endpoint_b);
}

auto sys_channel_read(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return static_cast<uint64_t>(-1);

    Rights existing;
    proc->handles.Lookup(static_cast<handle_t>(args.a1), Rights{}, &existing);
    bool endpoint_b = (existing.mask & Rights::ChannelEndpointB) != 0;

    auto result = typed_lookup<Channel>(proc->handles, static_cast<handle_t>(args.a1),
                                         Rights{.mask = Rights::Read});
    if (!result) return static_cast<uint64_t>(-1);
    auto* ch = result.value();

    size_t out_len = 0;
    size_t out_handles = 0;
    int rc;

    // Block until a message is available
    while (true) {
        rc = ch->Read(
            reinterpret_cast<void*>(args.a2), args.a3, &out_len,
            nullptr, 0, &out_handles, endpoint_b);
        if (rc != -2) break;  // -2 = empty
        thread_yield();       // give up CPU, wait for message
    }

    return (rc == 0) ? out_len : static_cast<uint64_t>(rc);
}

auto sys_channel_read_handles(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return static_cast<uint64_t>(-1);

    Rights existing;
    proc->handles.Lookup(static_cast<handle_t>(args.a1), Rights{}, &existing);
    bool endpoint_b = (existing.mask & Rights::ChannelEndpointB) != 0;

    auto result = typed_lookup<Channel>(proc->handles, static_cast<handle_t>(args.a1),
                                         Rights{.mask = Rights::Read});
    if (!result) return static_cast<uint64_t>(-1);
    auto* ch = result.value();

    // Args packed in struct (4-register ABI can't pass 5 scalars)
    struct ChannelReadHandleArgs {
        handle_t* handle_buf;
        size_t    buf_capacity;
    };
    auto* rha = reinterpret_cast<const ChannelReadHandleArgs*>(args.a2);

    size_t out_len = 0;
    size_t out_handles = 0;
    int rc;

    // Block until a message is available
    while (true) {
        rc = ch->Read(
            reinterpret_cast<void*>(args.a3), args.a4, &out_len,
            rha ? rha->handle_buf : nullptr,
            rha ? rha->buf_capacity : 0,
            &out_handles, endpoint_b);
        if (rc != -2) break;
        thread_yield();
    }

    if (rc == 0) return (out_handles << 32) | (out_len & 0xFFFFFFFFULL);
    return static_cast<uint64_t>(rc);
}

auto sys_port_create(SyscallArgs) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    auto* port = static_cast<Port*>(kmalloc(sizeof(Port)));
    if (!port) return INVALID_HANDLE;
    new (port) Port();

    Rights r{.mask = Rights::Read | Rights::Write |
                     Rights::Duplicate | Rights::Transfer};
    handle_t h = proc->handles.Alloc(port, r);
    port->Release();
    return h;
}

auto sys_port_register(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return -1;

    handle_t h = static_cast<handle_t>(args.a1);
    auto result = typed_lookup<Port>(proc->handles, h);
    if (!result) return -1;

    port_register_name(reinterpret_cast<const char*>(args.a2), result.value());
    return 0;
}

auto sys_port_connect(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    const char* name = reinterpret_cast<const char*>(args.a1);
    Port* port = port_lookup_name(name);
    if (!port) return static_cast<uint64_t>(-1);

    handle_t new_chan;
    int rc = Port::Connect(port, proc->handles, &new_chan);
    if (rc != 0) return static_cast<uint64_t>(rc);
    return new_chan;
}

auto sys_port_accept(SyscallArgs args) -> uint64_t {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    handle_t h = static_cast<handle_t>(args.a1);
    auto result = typed_lookup<Port>(proc->handles, h);
    if (!result) return INVALID_HANDLE;

    handle_t out_chan;
    int rc = result.value()->Accept(&out_chan);
    return (rc == 0) ? out_chan : static_cast<uint64_t>(rc);
}

// Process syscalls
auto sys_process_create(SyscallArgs args) -> uint64_t {
    const char* name = reinterpret_cast<const char*>(args.a1);
    Process* new_proc = Process::Create(name);
    if (!new_proc) return INVALID_HANDLE;

    Process* cur = current_process();
    if (!cur) {
        new_proc->Release();
        return INVALID_HANDLE;
    }

    Rights r{.mask = Rights::Read | Rights::Write |
                   Rights::Duplicate | Rights::Transfer};
    handle_t h = cur->handles.Alloc(new_proc, r);
    new_proc->Release(); // handle owns the ref now
    return h;
}

auto sys_process_exit(SyscallArgs) -> uint64_t {
    thread_exit();
    return 0; // unreachable
}

auto sys_vmo_create(SyscallArgs args) -> uint64_t {
    Process* cur = current_process();
    if (!cur) return INVALID_HANDLE;

    Vmo* vmo = Vmo::CreateAnonymous(args.a1);
    if (!vmo) return INVALID_HANDLE;

    Rights r{.mask = Rights::Read | Rights::Write |
                   Rights::Duplicate | Rights::Transfer};
    handle_t h = cur->handles.Alloc(vmo, r);
    vmo->Release(); // handle owns the ref
    return h;
}

auto sys_vmo_map(SyscallArgs args) -> uint64_t {
    Process* cur = current_process();
    if (!cur) return -1;

    handle_t h = static_cast<handle_t>(args.a1);
    auto result = typed_lookup<Vmo>(cur->handles, h);
    if (!result) return -1;
    auto* vmo = result.value();

    uint64_t va = args.a2;
    uint64_t flags = args.a3;
    uint64_t vmo_offset = args.a4;

    if (va + vmo->size() < va) return -1; // overflow check

    bool ok = cur->Map(vmo, va, vmo_offset, vmo->size() - vmo_offset,
                       (flags & 0x7) | VM_USER | VM_COW);
    return ok ? 0 : -1;
}

// ── VFS syscalls ──────────────────────────────────────────────────

auto sys_open(SyscallArgs args) -> uint64_t {
    // Stack variables declared up front for predictable frame layout.
    const char* path;
    uint64_t flags;
    MountEntry* mount;
    const char* prefix;
    Channel* file_chan;
    Process* fs_proc;
    handle_t fs_file_handle;
    int rc;
    Process* cur;
    handle_t client_handle;
    int rc2;
    size_t out_len;
    FileResponse resp;

    path = reinterpret_cast<const char*>(args.a1);
    flags = args.a2;

    mount = mount_resolve(path);
    if (!mount) return INVALID_HANDLE;

    // Compute the relative path after the mount prefix
    prefix = mount->path;
    while (*prefix) { path++; prefix++; }
    if (*path == '/') path++;

    // Create the file Channel
    file_chan = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!file_chan) return INVALID_HANDLE;
    new (file_chan) Channel();

    // Allocate a handle for the file Channel in the FS server's handle table.
    // The FS server is endpoint B — set ChannelEndpointB so its reads/writes
    // go to the correct queue.
    fs_proc = mount->fs_process;
    Rights fs_rights{.mask = Rights::Read | Rights::Write |
                             Rights::Duplicate | Rights::Transfer |
                             Rights::ChannelEndpointB};
    fs_file_handle = fs_proc->handles.Alloc(file_chan, fs_rights);

    // Build the Open message payload
    struct OpenPayload {
        char     path[256];
        uint32_t file_handle;
        uint32_t flags;
    };
    OpenPayload payload;
    payload.flags = static_cast<uint32_t>(flags);
    {
        int i = 0;
        while (path[i] && i < 255) { payload.path[i] = path[i]; i++; }
        payload.path[i] = '\0';
    }
    payload.file_handle = fs_file_handle;

    // Send Open request to FS server via the mount Channel
    rc = mount->fs_channel->Write(&payload, sizeof(payload), nullptr, 0);
    if (rc != 0) {
        fs_proc->handles.Free(fs_file_handle);
        file_chan->Release();
        return INVALID_HANDLE;
    }

    // Blocking read: wait for FS server response on the file Channel.
    out_len = 0;
    while (true) {
        rc2 = file_chan->Read(&resp, sizeof(resp), &out_len, nullptr, 0, nullptr);
        if (rc2 != -2) break;
        thread_yield();
    }

    if (rc2 != 0 || resp.result != 0) {
        fs_proc->handles.Free(fs_file_handle);
        file_chan->Release();
        return INVALID_HANDLE;
    }

    // Allocate a handle for the calling process
    cur = current_process();
    if (!cur) {
        fs_proc->handles.Free(fs_file_handle);
        file_chan->Release();
        return INVALID_HANDLE;
    }

    // Client is endpoint A — no ChannelEndpointB
    Rights client_rights{.mask = Rights::Read | Rights::Write |
                                 Rights::Duplicate | Rights::Transfer};
    client_handle = cur->handles.Alloc(file_chan, client_rights);

    // Release temporary ref from construction; handles own the refs now
    file_chan->Release();

    return client_handle;
}

auto sys_mount(SyscallArgs args) -> uint64_t {
    const char* path = reinterpret_cast<const char*>(args.a1);
    handle_t h = static_cast<handle_t>(args.a2);

    Process* cur = current_process();
    if (!cur) return static_cast<uint64_t>(-1);

    auto result = typed_lookup<Channel>(cur->handles, h);
    if (!result) return static_cast<uint64_t>(-1);

    // The FS server process is the current thread's process
    Process* fs_proc = current_thread()->process;

    return static_cast<uint64_t>(mount_add(path, result.value(), fs_proc));
}

// ── Block device syscalls ──────────────────────────────────────────

auto sys_blkdev_read(SyscallArgs args) -> uint64_t {
    const char* name = reinterpret_cast<const char*>(args.a1);
    uint64_t lba = args.a2;
    void* buf = reinterpret_cast<void*>(args.a3);
    size_t count = static_cast<size_t>(args.a4);

    BlockDevice* dev = blkdev_find(name);
    if (!dev) return static_cast<uint64_t>(-1);

    int rc = blkdev_read(dev, lba, buf, count);
    return static_cast<uint64_t>(rc);
}

auto sys_blkdev_write(SyscallArgs args) -> uint64_t {
    const char* name = reinterpret_cast<const char*>(args.a1);
    uint64_t lba = args.a2;
    const void* buf = reinterpret_cast<const void*>(args.a3);
    size_t count = static_cast<size_t>(args.a4);

    BlockDevice* dev = blkdev_find(name);
    if (!dev) return static_cast<uint64_t>(-1);

    int rc = blkdev_write(dev, lba, buf, count);
    return static_cast<uint64_t>(rc);
}

// ── Serial I/O ───────────────────────────────────────────────────

auto sys_serial_read(SyscallArgs) -> uint64_t {
    uint8_t byte;
    // Spin-wait on serial port data-ready (LSR bit 0).
    while (true) {
        asm volatile("inb %1, %0" : "=a"(byte) : "Nd"((uint16_t)0x3FD));
        if (byte & 1) break;
        // Brief pause to reduce KVM exit rate; then check again.
        // thread_yield() is intentionally omitted — with proper channel
        // blocking, no other thread needs CPU while shell waits for input.
        for (int i = 0; i < 1000; i++) {
            asm volatile("pause" : : : "memory");
        }
    }
    asm volatile("inb %1, %0" : "=a"(byte) : "Nd"((uint16_t)0x3F8));
    return byte;
}

// ── Dispatch table ───────────────────────────────────────────────

using syscall_fn_t = auto (*)(SyscallArgs) -> uint64_t;

constexpr int MAX_SYSCALL = 55;
syscall_fn_t g_syscall_table[MAX_SYSCALL];

auto init_syscall_table() -> void {
    g_syscall_table[SYSCALL_DEBUG_PRINT]    = sys_debug_print;
    g_syscall_table[SYSCALL_HANDLE_CLOSE]   = sys_handle_close;
    g_syscall_table[SYSCALL_HANDLE_DUP]     = sys_handle_dup;
    g_syscall_table[SYSCALL_CHANNEL_CREATE] = sys_channel_create;
    g_syscall_table[SYSCALL_CHANNEL_WRITE]  = sys_channel_write;
    g_syscall_table[SYSCALL_CHANNEL_READ]          = sys_channel_read;
    g_syscall_table[SYSCALL_CHANNEL_READ_HANDLES]  = sys_channel_read_handles;
    g_syscall_table[SYSCALL_PORT_CREATE]    = sys_port_create;
    g_syscall_table[SYSCALL_PORT_REGISTER]  = sys_port_register;
    g_syscall_table[SYSCALL_PORT_CONNECT]   = sys_port_connect;
    g_syscall_table[SYSCALL_PORT_ACCEPT]    = sys_port_accept;
    g_syscall_table[SYSCALL_PROCESS_CREATE] = sys_process_create;
    g_syscall_table[SYSCALL_PROCESS_EXIT]   = sys_process_exit;
    g_syscall_table[SYSCALL_VMO_CREATE]     = sys_vmo_create;
    g_syscall_table[SYSCALL_VMO_MAP]        = sys_vmo_map;
    g_syscall_table[SYSCALL_OPEN]           = sys_open;
    g_syscall_table[SYSCALL_MOUNT]          = sys_mount;
    g_syscall_table[SYSCALL_BLKDEV_READ]    = sys_blkdev_read;
    g_syscall_table[SYSCALL_BLKDEV_WRITE]   = sys_blkdev_write;
    g_syscall_table[SYSCALL_SERIAL_READ]    = sys_serial_read;
}

extern "C" auto syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4) -> uint64_t {
    if (num >= MAX_SYSCALL || !g_syscall_table[num]) {
        klog("Syscall #"); klog_hex(num); klog(": invalid\n");
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    SyscallArgs args{a1, a2, a3, a4};
    return g_syscall_table[num](args);
}

} // namespace

auto syscall_init() -> void {
    init_syscall_table();

    uint64_t efer = x86::rdmsr(IA32_EFER);
    x86::wrmsr(IA32_EFER, efer | 1);  // SCE bit

    // SYSRET computes CS = (STAR[63:48] + 16) | 3.
    // For user code CS=0x1B (GDT index 3, RPL 3):
    //   (STAR[63:48] + 16) | 3 = 0x1B → STAR[63:48] = 0x18 - 16 = 0x08
    // SYSCALL loads CS = STAR[47:32] = 0x08 (kernel code, ring 0).
    x86::wrmsr(IA32_STAR, (0x08ULL << 48) | (0x08ULL << 32));

    x86::wrmsr(IA32_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    x86::wrmsr(IA32_CSTAR, 0);
    x86::wrmsr(IA32_SFMASK, (1 << 9) | (1 << 10));

    klog("Syscall: LSTAR=");
    klog_hex(reinterpret_cast<uint64_t>(&syscall_entry));
    klog(" SCE=enabled\n");
}

auto syscall_set_handler(syscall_handler_t h) -> void { g_handler = h; }
