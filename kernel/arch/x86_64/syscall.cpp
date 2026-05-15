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
static Process* current_process() {
    Thread* t = current_thread();
    return t ? t->process : nullptr;
}

// ── Syscall handlers ────────────────────────────────────────────

uint64_t sys_debug_print(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    klog(reinterpret_cast<const char*>(a1));
    return 0;
}

uint64_t sys_handle_close(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return -1;
    proc->handles.Free(static_cast<handle_t>(a1));
    return 0;
}

uint64_t sys_handle_dup(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    handle_t h = static_cast<handle_t>(a1);
    Rights needed{.mask = Rights::Duplicate};
    Rights existing;
    KernelObject* obj = proc->handles.Lookup(h, needed, &existing);
    if (!obj) return INVALID_HANDLE;

    Rights new_rights{.mask = static_cast<uint32_t>(a2)};
    new_rights.mask &= existing.mask;
    return proc->handles.Alloc(obj, new_rights);
}

uint64_t sys_channel_create(uint64_t, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    Channel* ch = static_cast<Channel*>(kmalloc(sizeof(Channel)));
    if (!ch) return INVALID_HANDLE;
    new (ch) Channel();

    Rights full{.mask = Rights::Read | Rights::Write |
                       Rights::Duplicate | Rights::Transfer};
    handle_t a = proc->handles.Alloc(ch, full);
    handle_t b = proc->handles.Alloc(ch, full);
    ch->Release(); // handles own refs

    return (static_cast<uint64_t>(a) << 32) | b;
}

uint64_t sys_channel_write(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return -1;

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = proc->handles.Lookup(h, Rights{.mask = Rights::Write});
    if (!obj || obj->type() != KernelObject::Type::Channel) return -1;

    // Args packed in struct (4-register ABI can't pass 5 scalars)
    struct ChannelWriteArgs {
        const void* data; size_t data_len;
        const handle_t* handles; size_t num_handles;
    };
    auto* args = reinterpret_cast<const ChannelWriteArgs*>(a2);

    return static_cast<Channel*>(obj)->Write(
        args->data, args->data_len, args->handles, args->num_handles);
}

uint64_t sys_channel_read(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t) {
    Process* proc = current_process();
    if (!proc) return static_cast<uint64_t>(-1);

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = proc->handles.Lookup(h, Rights{.mask = Rights::Read});
    if (!obj || obj->type() != KernelObject::Type::Channel)
        return static_cast<uint64_t>(-1);

    size_t out_len = 0;
    size_t out_handles = 0;
    int rc;

    // Block until a message is available
    while (true) {
        rc = static_cast<Channel*>(obj)->Read(
            reinterpret_cast<void*>(a2), a3, &out_len,
            nullptr, 0, &out_handles);
        if (rc != -2) break;  // -2 = empty
        thread_yield();       // give up CPU, wait for message
    }

    return (rc == 0) ? out_len : static_cast<uint64_t>(rc);
}

uint64_t sys_channel_read_handles(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    Process* proc = current_process();
    if (!proc) return static_cast<uint64_t>(-1);

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = proc->handles.Lookup(h, Rights{.mask = Rights::Read});
    if (!obj || obj->type() != KernelObject::Type::Channel)
        return static_cast<uint64_t>(-1);

    // Args packed in struct (4-register ABI can't pass 5 scalars)
    struct ChannelReadHandleArgs {
        handle_t* handle_buf;
        size_t    buf_capacity;
    };
    auto* args = reinterpret_cast<const ChannelReadHandleArgs*>(a2);

    size_t out_len = 0;
    size_t out_handles = 0;
    int rc;

    // Block until a message is available
    while (true) {
        rc = static_cast<Channel*>(obj)->Read(
            reinterpret_cast<void*>(a3), a4, &out_len,
            args ? args->handle_buf : nullptr,
            args ? args->buf_capacity : 0,
            &out_handles);
        if (rc != -2) break;
        thread_yield();
    }

    if (rc == 0) return (out_handles << 32) | (out_len & 0xFFFFFFFFULL);
    return static_cast<uint64_t>(rc);
}

uint64_t sys_port_create(uint64_t, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    Port* port = static_cast<Port*>(kmalloc(sizeof(Port)));
    if (!port) return INVALID_HANDLE;
    new (port) Port();

    Rights r{.mask = Rights::Read | Rights::Write |
                     Rights::Duplicate | Rights::Transfer};
    handle_t h = proc->handles.Alloc(port, r);
    port->Release();
    return h;
}

uint64_t sys_port_register(uint64_t a1, uint64_t a2, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return -1;

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = proc->handles.Lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Port) return -1;

    port_register_name(reinterpret_cast<const char*>(a2),
                       static_cast<Port*>(obj));
    return 0;
}

uint64_t sys_port_connect(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    const char* name = reinterpret_cast<const char*>(a1);
    Port* port = port_lookup_name(name);
    if (!port) return static_cast<uint64_t>(-1);

    handle_t new_chan;
    int rc = Port::Connect(port, proc->handles, &new_chan);
    if (rc != 0) return static_cast<uint64_t>(rc);
    return new_chan;
}

uint64_t sys_port_accept(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    Process* proc = current_process();
    if (!proc) return INVALID_HANDLE;

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = proc->handles.Lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Port) return INVALID_HANDLE;

    handle_t out_chan;
    int rc = static_cast<Port*>(obj)->Accept(&out_chan);
    return (rc == 0) ? out_chan : static_cast<uint64_t>(rc);
}

// Process syscalls
uint64_t sys_process_create(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    const char* name = reinterpret_cast<const char*>(a1);
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

uint64_t sys_process_exit(uint64_t, uint64_t, uint64_t, uint64_t) {
    thread_exit();
    return 0; // unreachable
}

uint64_t sys_vmo_create(uint64_t a1, uint64_t, uint64_t, uint64_t) {
    Process* cur = current_process();
    if (!cur) return INVALID_HANDLE;

    Vmo* vmo = Vmo::CreateAnonymous(a1);
    if (!vmo) return INVALID_HANDLE;

    Rights r{.mask = Rights::Read | Rights::Write |
                   Rights::Duplicate | Rights::Transfer};
    handle_t h = cur->handles.Alloc(vmo, r);
    vmo->Release(); // handle owns the ref
    return h;
}

uint64_t sys_vmo_map(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    Process* cur = current_process();
    if (!cur) return -1;

    handle_t h = static_cast<handle_t>(a1);
    KernelObject* obj = cur->handles.Lookup(h);
    if (!obj || obj->type() != KernelObject::Type::Vmo) return -1;

    Vmo* vmo = static_cast<Vmo*>(obj);
    uint64_t va = a2;
    uint64_t flags = a3;
    uint64_t vmo_offset = a4;

    if (va + vmo->size() < va) return -1; // overflow check

    bool ok = cur->Map(vmo, va, vmo_offset, vmo->size() - vmo_offset,
                       (flags & 0x7) | VM_USER | VM_COW);
    return ok ? 0 : -1;
}

// ── Dispatch table ───────────────────────────────────────────────

using syscall_fn_t = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);

constexpr int MAX_SYSCALL = 52;
syscall_fn_t g_syscall_table[MAX_SYSCALL];

void init_syscall_table() {
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
}

extern "C" uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4) {
    if (num >= MAX_SYSCALL || !g_syscall_table[num]) {
        klog("Syscall #"); klog_hex(num); klog(": invalid\n");
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    return g_syscall_table[num](a1, a2, a3, a4);
}
} // namespace

void syscall_init() {
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

void syscall_set_handler(syscall_handler_t h) { g_handler = h; }
