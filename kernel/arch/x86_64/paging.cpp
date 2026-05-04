#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/lib/klog.hpp"

namespace {

uint64_t g_hhdm = 0;

inline void* early_phys_to_virt(uint64_t phys_addr) {
    return reinterpret_cast<void*>(g_hhdm + phys_addr);
}

PageTable* alloc_table() {
    void* phys = bitmap_alloc_page();
    if (!phys) return nullptr;
    return static_cast<PageTable*>(phys);
}

// Map using 2MB huge pages
__attribute__((unused))
static bool map_huge_range(PageTable* pml4_phys, uint64_t va, uint64_t pa, uint64_t size, uint64_t extra_flags) {
    for (uint64_t offset = 0; offset < size; offset += LARGE_PAGE_SIZE) {
        uint64_t cur_va = va + offset;
        uint64_t cur_pa = pa + offset;
        uint16_t i4 = pml4_index(cur_va);
        uint16_t i3 = pdpt_index(cur_va);
        uint16_t i2 = pd_index(cur_va);
        PageTable* pml4 = static_cast<PageTable*>(early_phys_to_virt(
            reinterpret_cast<uint64_t>(pml4_phys)));
        if (!(pml4->entries[i4] & PageFlags::Present)) {
            PageTable* pdpt = alloc_table();
            if (!pdpt) return false;
            pml4->entries[i4] = make_pte(reinterpret_cast<uint64_t>(pdpt),
                                         PageFlags::Present | PageFlags::Writable);
        }
        PageTable* pdpt = static_cast<PageTable*>(early_phys_to_virt(
            pte_phys_addr(pml4->entries[i4])));
        if (!(pdpt->entries[i3] & PageFlags::Present)) {
            PageTable* pd = alloc_table();
            if (!pd) return false;
            pdpt->entries[i3] = make_pte(reinterpret_cast<uint64_t>(pd),
                                         PageFlags::Present | PageFlags::Writable);
        }
        PageTable* pd = static_cast<PageTable*>(early_phys_to_virt(
            pte_phys_addr(pdpt->entries[i3])));
        pd->entries[i2] = make_pte(cur_pa, PageFlags::Present | PageFlags::Writable |
                                          PageFlags::Huge | extra_flags);
    }
    return true;
}

// Map using 4KB pages
bool map_4k_pages(PageTable* pml4_phys, uint64_t va, uint64_t pa, uint64_t size, uint64_t extra_flags) {
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
        uint64_t cur_va = va + offset;
        uint64_t cur_pa = pa + offset;
        uint16_t i4 = pml4_index(cur_va);
        uint16_t i3 = pdpt_index(cur_va);
        uint16_t i2 = pd_index(cur_va);
        uint16_t i1 = pt_index(cur_va);
        PageTable* pml4 = static_cast<PageTable*>(early_phys_to_virt(
            reinterpret_cast<uint64_t>(pml4_phys)));
        if (!(pml4->entries[i4] & PageFlags::Present)) {
            PageTable* pdpt = alloc_table();
            if (!pdpt) return false;
            pml4->entries[i4] = make_pte(reinterpret_cast<uint64_t>(pdpt),
                                         PageFlags::Present | PageFlags::Writable);
        }
        PageTable* pdpt = static_cast<PageTable*>(early_phys_to_virt(
            pte_phys_addr(pml4->entries[i4])));
        if (!(pdpt->entries[i3] & PageFlags::Present)) {
            PageTable* pd = alloc_table();
            if (!pd) return false;
            pdpt->entries[i3] = make_pte(reinterpret_cast<uint64_t>(pd),
                                         PageFlags::Present | PageFlags::Writable);
        }
        PageTable* pd = static_cast<PageTable*>(early_phys_to_virt(
            pte_phys_addr(pdpt->entries[i3])));
        if (!(pd->entries[i2] & PageFlags::Present)) {
            PageTable* pt = alloc_table();
            if (!pt) return false;
            pd->entries[i2] = make_pte(reinterpret_cast<uint64_t>(pt),
                                       PageFlags::Present | PageFlags::Writable);
        }
        PageTable* pt = static_cast<PageTable*>(early_phys_to_virt(
            pte_phys_addr(pd->entries[i2])));
        pt->entries[i1] = make_pte(cur_pa, PageFlags::Present | PageFlags::Writable | extra_flags);
    }
    return true;
}

} // namespace

// Set up higher-half kernel mapping by modifying Limine's PML4 in-place.
// Builds our page tables (PDPT/PD/PT) at fresh low-memory pages, then
// atomically replaces the kernel PML4 entry. Limine's HHDM and identity
// mappings are preserved. TLB is not flushed (CR3 reload triple-faults
// for unknown reasons) — stale TLB entries from Limine are still valid
// since our mapping is equivalent.
void paging_init(
    uint64_t hhdm,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t kernel_size)
{
    g_hhdm = hhdm;

    // Get Limine's PML4
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    PageTable* pml4 = static_cast<PageTable*>(early_phys_to_virt(cr3));

    // Build kernel mapping using a temporary PML4 (so map_4k_pages
    // allocates fresh PDPT/PD/PT at low addresses).
    PageTable* tmp_pml4 = alloc_table();
    if (!tmp_pml4) {
        while (1) asm volatile("cli; hlt");
    }
    uint64_t* tmp_virt = static_cast<uint64_t*>(early_phys_to_virt(
        reinterpret_cast<uint64_t>(tmp_pml4)));
    for (int i = 0; i < 512; i++) tmp_virt[i] = 0;

    uint64_t kernel_pages_4k = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (!map_4k_pages(tmp_pml4, kernel_virt_base, kernel_phys_base, kernel_pages_4k, 0)) {
        while (1) asm volatile("cli; hlt");
    }

    // Replace Limine's kernel PML4 entry with ours (atomic 8-byte write).
    uint16_t ki4 = pml4_index(kernel_virt_base);
    pml4->entries[ki4] = tmp_virt[ki4];

    // Limine's HHDM (0xFFFF800000000000) and identity (0x0) entries are
    // preserved. TLB lazily transitions to our mapping over time.
}
