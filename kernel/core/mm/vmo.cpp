#include "kernel/core/mm/vmo.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/lib/panic.hpp"

// Placement new (freestanding — no <new> header)
inline void* operator new(size_t, void* p) noexcept { return p; }

uint64_t Vmo::s_direct_map_offset_ = DIRECT_MAP_BASE;

void Vmo::SetDirectMapOffset(uint64_t offset) {
    s_direct_map_offset_ = offset;
}

Vmo::Vmo(Type t, uint64_t size)
    : KernelObject(KernelObject::Type::Vmo)
    , type_(t)
    , size_(size)
    , num_pages_(size / PAGE_SIZE)
{
    if (num_pages_ > 0) {
        size_t arr_sz = num_pages_ * sizeof(CowPage*);
        pages_ = static_cast<CowPage**>(kmalloc(arr_sz));
        if (!pages_) KPANIC("Vmo: OOM allocating page array");
        for (uint64_t i = 0; i < num_pages_; i++) pages_[i] = nullptr;
    } else {
        pages_ = nullptr;
    }
}

Vmo::~Vmo() {
    for (uint64_t i = 0; i < num_pages_; i++) {
        CowPage* cp = pages_[i];
        if (!cp) continue;
        cp->cow_refs--;
        if (cp->cow_refs == 0) {
            if (cp->phys_addr) bitmap_free_page(reinterpret_cast<void*>(cp->phys_addr));
            kfree(cp);
        }
    }
    kfree(pages_);
}

Vmo* Vmo::CreateAnonymous(uint64_t size) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size == 0) return nullptr;
    void* mem = kmalloc(sizeof(Vmo));
    if (!mem) return nullptr;
    return new (mem) Vmo(Anonymous, size);
}

Vmo* Vmo::CreatePhysical(uint64_t size, uint64_t phys_base) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    void* mem = kmalloc(sizeof(Vmo));
    if (!mem) return nullptr;
    Vmo* vmo = new (mem) Vmo(Physical, size);

    // Pre-allocate all CowPage entries with cow_refs=1
    for (uint64_t i = 0; i < vmo->num_pages_; i++) {
        CowPage* cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!cp) KPANIC("Vmo: OOM creating Physical pages");
        cp->phys_addr = phys_base + i * PAGE_SIZE;
        cp->cow_refs = 1;
        vmo->pages_[i] = cp;
    }
    return vmo;
}

uint64_t Vmo::GetPage(uint64_t offset, bool for_write) {
    uint64_t page_idx = offset / PAGE_SIZE;
    if (page_idx >= num_pages_) return 0;

    lock_.lock();

    CowPage* cp = pages_[page_idx];

    if (!cp) {
        // First access — allocate and zero-fill
        void* phys = bitmap_alloc_page();
        if (!phys) { lock_.unlock(); return 0; }

        cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!cp) {
            bitmap_free_page(phys);
            lock_.unlock();
            return 0;
        }

        uint64_t pa = reinterpret_cast<uint64_t>(phys);
        // Zero via direct map
        uint64_t* virt = reinterpret_cast<uint64_t*>(s_direct_map_offset_ + pa);
        for (int j = 0; j < 512; j++) virt[j] = 0;

        cp->phys_addr = pa;
        cp->cow_refs = 1;
        pages_[page_idx] = cp;
        lock_.unlock();
        return pa;
    }

    // COW resolution: page is shared and caller wants to write
    if (for_write && cp->cow_refs > 1) {
        void* new_phys = bitmap_alloc_page();
        if (!new_phys) { lock_.unlock(); return 0; }

        uint64_t new_pa = reinterpret_cast<uint64_t>(new_phys);
        uint64_t old_pa = cp->phys_addr;

        // Copy old -> new via direct map
        uint64_t* src = reinterpret_cast<uint64_t*>(s_direct_map_offset_ + old_pa);
        uint64_t* dst = reinterpret_cast<uint64_t*>(s_direct_map_offset_ + new_pa);
        for (int j = 0; j < 512; j++) dst[j] = src[j];

        cp->cow_refs--;

        CowPage* new_cp = static_cast<CowPage*>(kmalloc(sizeof(CowPage)));
        if (!new_cp) {
            bitmap_free_page(new_phys);
            lock_.unlock();
            return 0;
        }
        new_cp->phys_addr = new_pa;
        new_cp->cow_refs = 1;
        pages_[page_idx] = new_cp;

        lock_.unlock();
        return new_pa;
    }

    lock_.unlock();
    return cp->phys_addr;
}

Vmo* Vmo::CloneCoW() {
    Vmo* child = CreateAnonymous(size_);
    if (!child) return nullptr;

    lock_.lock();
    for (uint64_t i = 0; i < num_pages_; i++) {
        CowPage* cp = pages_[i];
        if (cp) {
            cp->cow_refs++;
            child->pages_[i] = cp;
        }
    }
    lock_.unlock();

    return child;
}
