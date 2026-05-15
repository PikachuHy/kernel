#include "kernel/core/elf_loader.hpp"
#include "kernel/core/object/process.hpp"
#include "kernel/core/mm/vmo.hpp"
#include "kernel/core/mm/vmm.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/lib/klog.hpp"

// ── ELF64 header structures ───────────────────────────────────────

struct Elf64Header {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct Elf64PHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

constexpr uint32_t PT_LOAD  = 1;
constexpr uint32_t PF_R     = 4;
constexpr uint32_t PF_W     = 2;
constexpr uint32_t PF_X     = 1;

// Stack: 16KB at top of user address space
constexpr uint64_t USER_STACK_SIZE = PAGE_SIZE * 4;
constexpr uint64_t USER_STACK_TOP  = 0x00007FFFFFFFFFFFULL & ~(PAGE_SIZE - 1);

// Assembly trampoline
extern "C" void elf_trampoline();

// ── elf_load ──────────────────────────────────────────────────────

Process* elf_load(const void* elf_data, size_t elf_size,
                  const char* proc_name, uint8_t priority,
                  Thread** out_thread) {
    if (elf_size < sizeof(Elf64Header)) {
        klog("elf_load: file too small\n");
        return nullptr;
    }

    auto* hdr = static_cast<const Elf64Header*>(elf_data);

    // Verify ELF magic
    if (hdr->ident[0] != 0x7F || hdr->ident[1] != 'E' ||
        hdr->ident[2] != 'L'  || hdr->ident[3] != 'F') {
        klog("elf_load: bad ELF magic\n");
        return nullptr;
    }

    // Verify 64-bit little-endian x86-64 executable
    if (hdr->ident[4] != 2) {
        klog("elf_load: not 64-bit\n");
        return nullptr;
    }
    if (hdr->machine != 0x3E) {
        klog("elf_load: not x86-64\n");
        return nullptr;
    }

    // Create the process
    Process* proc = Process::Create(proc_name);
    if (!proc) {
        klog("elf_load: failed to create process\n");
        return nullptr;
    }

    const auto* phdr_base = reinterpret_cast<const uint8_t*>(elf_data) + hdr->phoff;

    // Process each PT_LOAD segment
    for (int i = 0; i < hdr->phnum; i++) {
        auto* ph = reinterpret_cast<const Elf64PHeader*>(
            phdr_base + i * hdr->phentsize);

        if (ph->type != PT_LOAD) continue;

        // Page-align the segment
        uint64_t va_page  = ph->vaddr & ~(PAGE_SIZE - 1);
        uint64_t va_off   = ph->vaddr & (PAGE_SIZE - 1);
        uint64_t size_pg  = (va_off + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        if (size_pg == 0) continue;

        // Build VM flags from ELF PF_* flags
        uint64_t vm_flags = VM_USER | VM_COW;
        if (ph->flags & PF_R) vm_flags |= VM_READ;
        if (ph->flags & PF_W) vm_flags |= VM_WRITE;
        if (ph->flags & PF_X) vm_flags |= VM_EXEC;

        // Check if a region already covers this VA (overlapping PT_LOAD
        // segments). If so, copy data into the existing VMO instead of
        // creating a new one.
        VmRegion* existing = proc->FindRegion(va_page);
        Vmo* vmo;
        bool own_vmo = true;

        if (existing && existing->base_va == va_page) {
            // Overlapping segment — merge data into the existing VMO.
            vmo = existing->vmo;
            own_vmo = false;
            // Merge VM flags (e.g. R+X from .text + R from .rodata -> R+WX)
            existing->flags |= vm_flags;
        } else {
            vmo = Vmo::CreateAnonymous(size_pg);
            if (!vmo) {
                klog("elf_load: VMO alloc failed\n");
                proc->Release();
                return nullptr;
            }
        }

        // Copy segment data from ELF into the VMO.
        const auto* src      = reinterpret_cast<const uint8_t*>(elf_data) + ph->offset;
        uint64_t    vmo_off  = va_off;
        uint64_t    src_off  = 0;
        uint64_t    remaining = ph->filesz;

        while (remaining > 0) {
            uint64_t phys = vmo->GetPage(vmo_off, true);
            if (!phys) {
                klog("elf_load: GetPage failed\n");
                if (own_vmo) vmo->Release();
                proc->Release();
                return nullptr;
            }

            auto*  dst      = reinterpret_cast<uint8_t*>(DIRECT_MAP_BASE + phys);
            uint64_t page_off = vmo_off & (PAGE_SIZE - 1);
            uint64_t copy_sz  = PAGE_SIZE - page_off;
            if (copy_sz > remaining) copy_sz = remaining;

            for (uint64_t j = 0; j < copy_sz; j++) {
                dst[page_off + j] = src[src_off + j];
            }

            vmo_off   += copy_sz;
            src_off   += copy_sz;
            remaining -= copy_sz;
        }

        // Map the VMO into the process address space (only if new).
        if (own_vmo) {
            if (!proc->Map(vmo, va_page, 0, size_pg, vm_flags)) {
                klog("elf_load: Map failed at ");
                klog_hex(va_page); klog("\n");
                vmo->Release();
                proc->Release();
                return nullptr;
            }
        }

        if (own_vmo) vmo->Release(); // Map holds the ref now
    }

    // ── Create user stack ──────────────────────────────────────────
    uint64_t stack_va = USER_STACK_TOP - USER_STACK_SIZE;
    Vmo* stack_vmo = Vmo::CreateAnonymous(USER_STACK_SIZE);
    if (!stack_vmo) {
        klog("elf_load: stack VMO failed\n");
        proc->Release();
        return nullptr;
    }

    if (!proc->Map(stack_vmo, stack_va, 0, USER_STACK_SIZE,
                   VM_READ | VM_WRITE | VM_USER)) {
        klog("elf_load: stack Map failed\n");
        stack_vmo->Release();
        proc->Release();
        return nullptr;
    }
    stack_vmo->Release();

    // Pre-commit the top page of the stack so the thread can push
    // before any page fault fires
    stack_vmo->GetPage(USER_STACK_SIZE - PAGE_SIZE, true);

    // ── Create main thread ─────────────────────────────────────────
    // The thread runs elf_trampoline() in kernel mode, which reads r15/r14
    // from the saved frame and calls enter_usermode(entry, stack_top).
    Thread* thread = thread_create(elf_trampoline, proc_name, priority, proc);
    if (!thread) {
        klog("elf_load: thread_create failed\n");
        proc->Release();
        return nullptr;
    }

    // Overload r15 and r14 in the thread's initial stack frame
    // to pass the ELF entry point and user stack top.
    // thread_create stores rsp as a physical offset; use the direct map
    // to write the values.
    uint64_t* frame = reinterpret_cast<uint64_t*>(
        DIRECT_MAP_BASE + thread->rsp);
    frame[0] = hdr->entry;        // r15 slot -> entry_rip
    frame[1] = USER_STACK_TOP;    // r14 slot -> user_rsp

    *out_thread = thread;
    return proc;
}

// ── elf_load_init_process ────────────────────────────────────────
// Symbols provided by kernel/init/init_embed.o via llvm-objcopy.
extern "C" uint8_t _binary_init_bin_start[];
extern "C" uint8_t _binary_init_bin_end[];

extern "C" void elf_load_init_process() {
    uint64_t init_size = _binary_init_bin_end - _binary_init_bin_start;
    klog("Loading init process (");
    klog_hex(init_size);
    klog(" bytes)...\n");

    Thread* init_thread = nullptr;
    Process* init_proc = elf_load(_binary_init_bin_start, init_size,
                                   "init", 1, &init_thread);
    if (init_proc && init_thread) {
        klog("  Init process loaded, starting...\n");
        thread_start(init_thread);
    } else {
        klog("  FAILED to load init process\n");
    }
}
