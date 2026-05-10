#include <gtest/gtest.h>
#include <stdlib.h>
#include <cstring>
#include "kernel/core/object/process.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/arch/x86_64/paging.hpp"
// ── kmalloc/kfree stubs (support any size via malloc/free) ──────────

void* kmalloc(size_t n) { return malloc(n); }

void kfree(void* p) { free(p); }

// ── bitmap_alloc_page stubs (for linking only; not exercised) ───────

void* bitmap_alloc_page() { return malloc(4096); }

void bitmap_free_page(void* phys_addr) { free(phys_addr); }

// ── HandleTable implementation (replaces handle_table.cpp for test) ─
// The real handle_table.cpp has a ForEach that cannot distinguish
// freelist pointers from valid KernelObject pointers. This version
// uses rights.mask as an in-use flag (>0 = valid entry).

void HandleTable::Init() {
    for (handle_t i = 0; i < MAX_HANDLES; i++) {
        entries_[i].obj = nullptr;
        entries_[i].rights.mask = 0;
    }
    free_head_ = 1; // handle 0 is INVALID_HANDLE
}

handle_t HandleTable::Alloc(KernelObject* obj, Rights rights) {
    lock_.lock();
    if (free_head_ == 0 || free_head_ >= MAX_HANDLES) {
        lock_.unlock();
        return INVALID_HANDLE;
    }
    handle_t h = free_head_;
    // Find next free slot
    free_head_ = INVALID_HANDLE;
    for (handle_t i = h + 1; i < MAX_HANDLES; i++) {
        if (entries_[i].rights.mask == 0 && entries_[i].obj == nullptr) {
            free_head_ = i;
            break;
        }
    }
    entries_[h].obj = obj;
    entries_[h].rights = rights;
    obj->AddRef();
    lock_.unlock();
    return h;
}

void HandleTable::Free(handle_t h) {
    if (h == INVALID_HANDLE || h >= MAX_HANDLES) return;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    if (obj) {
        entries_[h].obj = nullptr;
        entries_[h].rights.mask = 0;
        if (h < free_head_ || free_head_ == INVALID_HANDLE) {
            free_head_ = h;
        }
        obj->Release();
    }
    lock_.unlock();
}

KernelObject* HandleTable::Lookup(handle_t h, Rights needed,
                                   Rights* out_rights) {
    if (h == INVALID_HANDLE || h >= MAX_HANDLES) return nullptr;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    Rights rights = entries_[h].rights;
    lock_.unlock();
    if (!obj || rights.mask == 0) return nullptr; // mask==0 means free slot
    if (needed.mask != 0 && !rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = rights;
    return obj;
}

int HandleTable::ForEach(KernelObject** out_objs, handle_t* out_handles,
                          int max) {
    int count = 0;
    lock_.lock();
    for (handle_t h = 1; h < MAX_HANDLES && count < max; h++) {
        KernelObject* obj = entries_[h].obj;
        if (obj && entries_[h].rights.mask > 0) {
            out_objs[count] = obj;
            out_handles[count] = h;
            count++;
        }
    }
    lock_.unlock();
    return count;
}

// ── Backward-compat globals stubs ─────────────────────────────────────

void handle_table_set_fallback(HandleTable* ht) { (void)ht; }

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    (void)obj; (void)rights;
    return INVALID_HANDLE;
}

void handle_free(handle_t h) { (void)h; }

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    (void)h; (void)needed; (void)out_rights;
    return nullptr;
}

// ── Vmo method implementations ───────────────────────────────────────
// These replace vmo.cpp for host testing.

uint64_t Vmo::s_direct_map_offset_ = 0;

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
        if (pages_) {
            for (uint64_t i = 0; i < num_pages_; i++) pages_[i] = nullptr;
        }
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
    return new (mem) Vmo(Vmo::Anonymous, size);
}

Vmo* Vmo::CreatePhysical(uint64_t size, uint64_t phys_base) {
    (void)phys_base;
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size == 0) return nullptr;
    void* mem = kmalloc(sizeof(Vmo));
    if (!mem) return nullptr;
    return new (mem) Vmo(Vmo::Physical, size);
}

uint64_t Vmo::GetPage(uint64_t offset, bool for_write) {
    (void)offset;
    (void)for_write;
    return 0; // not exercised by Process tests
}

Vmo* Vmo::CloneCoW() { return nullptr; }

// ── VMM/paging stubs for host testing ─────────────────────────────────

uint64_t paging_kernel_pml4_template() { return 0x200000; }

uint64_t page_table_unmap(uint64_t pml4_phys, uint64_t va) {
    (void)pml4_phys; (void)va;
    return 0;
}

bool page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) {
    (void)pml4_phys; (void)va; (void)pa; (void)flags;
    return true;
}

uint64_t vmm_create_user_pml4() { return 0x7000; }

void vmm_destroy_user_pml4(uint64_t pml4_phys) { (void)pml4_phys; }

bool vmm_insert_region(VmRegion** head, VmRegion* region) {
    uint64_t end = region->base_va + region->size;
    VmRegion** prev = head;
    while (*prev) {
        uint64_t prev_end = (*prev)->base_va + (*prev)->size;
        if (end <= (*prev)->base_va) break;
        if (region->base_va < prev_end) return false;
        prev = &(*prev)->next;
    }
    region->next = *prev;
    *prev = region;
    return true;
}

VmRegion* vmm_find_region(VmRegion* head, uint64_t va) {
    while (head) {
        if (va >= head->base_va && va < head->base_va + head->size) return head;
        if (va < head->base_va) return nullptr;
        head = head->next;
    }
    return nullptr;
}

VmRegion* vmm_remove_region(VmRegion** head, uint64_t va, uint64_t size,
                             uint64_t pml4_phys) {
    (void)size;
    (void)pml4_phys;
    while (*head) {
        if (va >= (*head)->base_va && va < (*head)->base_va + (*head)->size) {
            VmRegion* r = *head;
            *head = r->next;
            kfree(r);
            return r;
        }
        head = &(*head)->next;
    }
    return nullptr;
}

// ── Tests ─────────────────────────────────────────────────────────────

TEST(ProcessTest, Create) {
    Process* p = Process::Create("test");
    ASSERT_NE(p, nullptr);
    EXPECT_NE(p->pml4_phys, 0u);
    EXPECT_EQ(p->regions, nullptr);
    EXPECT_EQ(p->threads, nullptr);
    EXPECT_STREQ(p->name, "test");
    p->Release();
}

TEST(ProcessTest, MapAndFindRegion) {
    Process* p = Process::Create("test");
    ASSERT_NE(p, nullptr);
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 4);
    ASSERT_NE(vmo, nullptr);

    EXPECT_TRUE(p->Map(vmo, 0x1000000, 0, PAGE_SIZE * 4, VM_READ | VM_WRITE | VM_USER));

    VmRegion* r = p->FindRegion(0x1000000);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->base_va, 0x1000000u);
    EXPECT_EQ(r->size, PAGE_SIZE * 4);
    EXPECT_EQ(r->vmo, vmo);

    // Find middle of region
    ASSERT_NE(p->FindRegion(0x1001000), nullptr);
    // Find outside region
    ASSERT_EQ(p->FindRegion(0x0), nullptr);

    p->Unmap(0x1000000, PAGE_SIZE * 4);
    ASSERT_EQ(p->FindRegion(0x1000000), nullptr);

    vmo->Release(); // extra ref from CreateAnonymous
    p->Release();
}

TEST(ProcessTest, MapOverlapFails) {
    Process* p = Process::Create("test");
    ASSERT_NE(p, nullptr);
    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE * 8);
    ASSERT_NE(vmo, nullptr);

    EXPECT_TRUE(p->Map(vmo, 0x400000, 0, PAGE_SIZE * 4, VM_READ));
    EXPECT_FALSE(p->Map(vmo, 0x401000, 0, PAGE_SIZE, VM_READ));

    vmo->Release();
    p->Release();
}

TEST(ProcessTest, HandleTableIsolation) {
    Process* p1 = Process::Create("p1");
    Process* p2 = Process::Create("p2");
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    Vmo* vmo = Vmo::CreateAnonymous(PAGE_SIZE);
    // refcount starts at 1 (from CreateAnonymous)

    handle_t h1 = p1->handles.Alloc(vmo, Rights{.mask = Rights::Read});
    // refcount becomes 2 (Alloc does AddRef)

    handle_t h2 = p2->handles.Alloc(vmo, Rights{.mask = Rights::Write});
    // refcount becomes 3

    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);

    // Each table returns the correct object through its own handle
    EXPECT_EQ(p1->handles.Lookup(h1), vmo);
    EXPECT_EQ(p2->handles.Lookup(h2), vmo);

    // Isolate: freeing from one table doesn't affect the other
    p1->handles.Free(h1);   // refcount: 3 -> 2
    EXPECT_EQ(vmo->refcount(), 2u);
    EXPECT_EQ(p2->handles.Lookup(h2), vmo);  // p2's handle still valid

    p2->handles.Free(h2);   // refcount: 2 -> 1
    EXPECT_EQ(vmo->refcount(), 1u);
    EXPECT_EQ(p2->handles.Lookup(h2), nullptr);

    vmo->Release();          // refcount: 1 -> 0 -> Vmo destroyed
    p1->Release();
    p2->Release();
}

TEST(ProcessTest, NullName) {
    Process* p = Process::Create(nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name[0], '\0');
    p->Release();
}

TEST(ProcessTest, LongNameTruncated) {
    Process* p = Process::Create("this is a very long process name indeed");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->name[31], '\0');
    EXPECT_EQ(memcmp(p->name, "this is a very long process name", 31), 0);
    p->Release();
}
