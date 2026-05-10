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
    // Zero the page — it may contain garbage from previous use.
    // Uninitialised page table entries with random Present bits set
    // will crash the page-table walker after CR3 reload.
    uint64_t* virt = static_cast<uint64_t*>(early_phys_to_virt(
        reinterpret_cast<uint64_t>(phys)));
    for (int i = 0; i < 512; i++) virt[i] = 0;
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
__attribute__((unused))
static bool map_4k_pages(PageTable* pml4_phys, uint64_t va, uint64_t pa, uint64_t size, uint64_t extra_flags) {
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
        // Limine often maps the kernel with 2MB huge pages. If the PD entry
        // is a huge page, we must replace it with a real Page Table before
        // installing 4K PTEs — otherwise we'd write PTEs into kernel code.
        if (pd->entries[i2] & PageFlags::Huge) {
            PageTable* pt_phys = alloc_table();
            if (!pt_phys) return false;
            uint64_t huge_pa = pte_phys_addr(pd->entries[i2]);
            uint64_t huge_flags = pd->entries[i2] & ~PageFlags::Huge;
            pd->entries[i2] = make_pte(reinterpret_cast<uint64_t>(pt_phys),
                                       PageFlags::Present | PageFlags::Writable);
            // Pre-fill the new PT with 4K entries covering the same 2MB range.
            // pt_phys is a physical address — convert via HHDM to access.
            PageTable* pt_virt = static_cast<PageTable*>(early_phys_to_virt(
                reinterpret_cast<uint64_t>(pt_phys)));
            for (int j = 0; j < 512; j++) {
                pt_virt->entries[j] = make_pte(huge_pa + j * PAGE_SIZE,
                                                huge_flags);
            }
        } else if (!(pd->entries[i2] & PageFlags::Present)) {
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

void paging_init(
    uint64_t hhdm,
    uint64_t kernel_phys_base,
    uint64_t kernel_virt_base,
    uint64_t kernel_size)
{
    g_hhdm = hhdm;

    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    PageTable* old_pml4 = static_cast<PageTable*>(early_phys_to_virt(old_cr3));

    // Build new PML4: copy ALL Limine entries EXCEPT the kernel entry,
    // which we zero out so map_4k_pages creates fresh PDPT/PD/PT.
    PageTable* new_pml4_phys = alloc_table();
    if (!new_pml4_phys) { while (1) asm volatile("cli; hlt"); }
    uint64_t* new_pml4 = static_cast<uint64_t*>(early_phys_to_virt(
        reinterpret_cast<uint64_t>(new_pml4_phys)));
    uint16_t ki4 = pml4_index(kernel_virt_base);
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = (i == ki4) ? 0 : old_pml4->entries[i];
    }

    klog("Paging: mapping kernel (");
    klog_hex(kernel_size);
    klog(" bytes) into new page tables...\n");

    uint64_t kernel_pages_4k = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bool ok = map_4k_pages(new_pml4_phys, kernel_virt_base, kernel_phys_base,
                           kernel_pages_4k, 0);
    if (!ok) {
        klog("Paging: FAILED\n");
        while (1) asm volatile("cli; hlt");
    }

    klog("Paging: loading new page tables...\n");
    uint64_t new_cr3 = reinterpret_cast<uint64_t>(new_pml4_phys);
    asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
    klog("Paging: new page tables active\n");
}
