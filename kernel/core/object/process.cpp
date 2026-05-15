// Placement new: use system <new> when available, otherwise provide our own.
#if __has_include(<new>)
#include <new>
#else
#include <stddef.h>
inline void* operator new(size_t, void* p) noexcept { return p; }
#endif

#include "kernel/core/object/process.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/arch/x86_64/paging.hpp"

Process* Process::Create(const char* name, bool kernel_process) {
    uint64_t pml4;
    if (kernel_process) {
        // Kernel process shares the kernel PML4 template (no user mappings).
        pml4 = paging_kernel_pml4_template();
    } else {
        // User process gets its own PML4 with fresh user-half entries.
        pml4 = vmm_create_user_pml4();
    }
    if (!pml4) return nullptr;

    void* mem = kmalloc(sizeof(Process));
    if (!mem) {
        if (!kernel_process) vmm_destroy_user_pml4(pml4);
        return nullptr;
    }
    return new (mem) Process(name, pml4);
}

Process::Process(const char* name, uint64_t pml4)
    : KernelObject(KernelObject::Type::Process)
    , pml4_phys(pml4)
    , regions(nullptr)
    , threads(nullptr)
    , parent(nullptr)
{
    handles.Init();
    if (name) {
        int i = 0;
        while (i < 31 && name[i]) { this->name[i] = name[i]; i++; }
        this->name[i] = '\0';
    } else {
        this->name[0] = '\0';
    }
}

Process::~Process() {
    // Free all VmRegions (unmap + release VMOs + free region structs)
    while (regions) {
        VmRegion* r = regions;
        regions = r->next;
        for (uint64_t off = 0; off < r->size; off += PAGE_SIZE) {
            uint64_t va = r->base_va + off;
            page_table_unmap(pml4_phys, va);
        }
        if (r->vmo) r->vmo->Release();
        kfree(r);
    }

    // Free all handle table entries
    KernelObject* objs[128];
    handle_t handles_arr[128];
    int n;
    while ((n = handles.ForEach(objs, handles_arr, 128)) > 0) {
        for (int i = 0; i < n; i++) {
            handles.Free(handles_arr[i]);
        }
    }

    // Free the handle table backing array
    handles.Destroy();
}

bool Process::Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
                  uint64_t size, uint64_t flags) {
    if (va + size < va) return false;
    if (size == 0) return false;

    VmRegion* r = static_cast<VmRegion*>(kmalloc(sizeof(VmRegion)));
    if (!r) return false;

    r->base_va    = va;
    r->size       = size;
    r->vmo        = vmo;
    r->vmo_offset = vmo_offset;
    r->flags      = flags;
    r->next       = nullptr;

    if (!vmm_insert_region(&regions, r)) {
        kfree(r);
        return false;
    }

    vmo->AddRef();
    return true;
}

bool Process::Unmap(uint64_t va, uint64_t size) {
    // Find the region first to get the VMO reference
    VmRegion* r = vmm_find_region(regions, va);
    if (!r) return false;

    Vmo* vmo = r->vmo;  // save before removing

    VmRegion* removed = vmm_remove_region(&regions, va, size, pml4_phys);
    if (!removed) return false;

    if (vmo) vmo->Release();
    return true;
}

VmRegion* Process::FindRegion(uint64_t va) {
    return vmm_find_region(regions, va);
}

bool Process::HandlePageFault(uint64_t fault_addr, bool was_write) {
    VmRegion* r = FindRegion(fault_addr);
    if (!r) return false;

    // Check permissions
    if (was_write && !(r->flags & VM_WRITE) && !(r->flags & VM_COW)) {
        return false;
    }
    if (!was_write && !(r->flags & VM_READ)) {
        return false;
    }

    uint64_t vmo_off = (fault_addr - r->base_va) + r->vmo_offset;
    bool cow_write = was_write && (r->flags & VM_COW);
    uint64_t phys = r->vmo->GetPage(vmo_off, cow_write);
    if (!phys) return false;

    // Build PTE flags from VmRegion flags
    uint64_t pte_flags = PageFlags::Present;
    // If the region is writable OR we just resolved COW, make the PTE writable
    if ((r->flags & VM_WRITE) || cow_write) {
        pte_flags |= PageFlags::Writable;
    }
    if (r->flags & VM_USER) {
        pte_flags |= PageFlags::User;
    }
    if (!(r->flags & VM_EXEC)) {
        pte_flags |= PageFlags::NoExec;
    }

    return page_table_map(pml4_phys, fault_addr, phys, pte_flags);
}

void Process::AddThread(Thread* t) {
    t->proc_next = threads;
    threads = t;
    t->process = this;
}

void Process::RemoveThread(Thread* t) {
    Thread** prev = &threads;
    while (*prev) {
        if (*prev == t) {
            *prev = t->proc_next;
            t->proc_next = nullptr;
            t->process = nullptr;
            return;
        }
        prev = &(*prev)->proc_next;
    }
}
