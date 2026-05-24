#include "kernel/core/mm/vmm.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/arch/x86_64/paging.hpp"

// ── PML4 management ─────────────────────────────────────────────────

auto vmm_create_user_pml4() -> uint64_t {
    uint64_t template_pml4 = paging_kernel_pml4_template();
    if (!template_pml4) return 0;

    void* new_pml4_phys = buddy_alloc_pages(0);
    if (!new_pml4_phys) return 0;

    uint64_t np = reinterpret_cast<uint64_t>(new_pml4_phys);
    uint64_t* src = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + template_pml4);
    uint64_t* dst = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + np);

    // Copy kernel-half entries (indices 256-511) from the template
    for (int i = 256; i < 512; i++) {
        dst[i] = src[i];
    }
    // Zero user-half entries (buddy_alloc_pages does NOT zero memory)
    for (int i = 0; i < 256; i++) {
        dst[i] = 0;
    }
    return np;
}

auto vmm_destroy_user_pml4(uint64_t pml4_phys) -> void {
    if (!pml4_phys) return;
    uint64_t* pml4 = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pml4_phys);

    // Walk user-half PML4 entries (0-255), recursively freeing page tables
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PageFlags::Present)) continue;
        uint64_t pdpt_phys = pte_phys_addr(pml4[i4]);
        uint64_t* pdpt = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pdpt_phys);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PageFlags::Present)) continue;
            uint64_t pd_phys = pte_phys_addr(pdpt[i3]);
            uint64_t* pd = reinterpret_cast<uint64_t*>(DIRECT_MAP_BASE + pd_phys);

            for (int i2 = 0; i2 < 512; i2++) {
                if (pd[i2] & PageFlags::Huge) continue;
                if (!(pd[i2] & PageFlags::Present)) continue;
                uint64_t pt_phys = pte_phys_addr(pd[i2]);
                buddy_free_pages(reinterpret_cast<void*>(pt_phys), 0);
            }
            buddy_free_pages(reinterpret_cast<void*>(pd_phys), 0);
        }
        buddy_free_pages(reinterpret_cast<void*>(pdpt_phys), 0);
    }
    buddy_free_pages(reinterpret_cast<void*>(pml4_phys), 0);
}

// ── VmRegion list operations ────────────────────────────────────────

auto vmm_insert_region(VmRegion** head, VmRegion* region) -> bool {
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

auto vmm_find_region(VmRegion* head, uint64_t va) -> VmRegion* {
    while (head) {
        if (va >= head->base_va && va < head->base_va + head->size) {
            return head;
        }
        if (va < head->base_va) return nullptr; // regions are sorted, so stop early
        head = head->next;
    }
    return nullptr;
}

auto vmm_remove_region(VmRegion** head, uint64_t va, uint64_t size,
                         uint64_t pml4_phys) -> VmRegion* {
    while (*head) {
        if (va >= (*head)->base_va && va < (*head)->base_va + (*head)->size) {
            VmRegion* r = *head;
            *head = r->next;

            // Unmap all pages in the range
            for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
                page_table_unmap(pml4_phys, va + off);
            }

            kfree(r);
            return r;  // caller should not use this pointer (it's freed)
        }
        head = &(*head)->next;
    }
    return nullptr;
}
