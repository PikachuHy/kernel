#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"

uint64_t g_hhdm = 0;

namespace {

uint64_t g_kernel_template_pml4 = 0;

inline uint64_t* hhdm_ptr(uint64_t phys) {
    return reinterpret_cast<uint64_t*>(g_hhdm + phys);
}

// Allocate a zeroed page table page. Returns physical address, or 0 on OOM.
// buddy_alloc_pages returns a PHYSICAL address (idx_to_phys).
static uint64_t alloc_table_phys() {
    void* phys = buddy_alloc_pages(0);
    if (!phys) return 0;
    uint64_t phys_addr = reinterpret_cast<uint64_t>(phys);
    uint64_t* virt = hhdm_ptr(phys_addr);
    for (int i = 0; i < 512; i++) virt[i] = 0;
    return phys_addr;
}

// Walk the page table for `va`, creating intermediate page tables as needed.
// On return, *pte_out points to the leaf PTE slot (accessed via HHDM).
// Returns true on success, false on OOM.
static bool walk_create(uint64_t pml4_phys, uint64_t va, uint64_t** pte_out) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    // Intermediate page tables need User|Writable so that the hardware
    // page-table walker can descend into them from ring 3. Without User,
    // the walk stops at the first non-User intermediate entry and the CPU
    // raises a #PF even though the leaf PTE grants access.
    constexpr uint64_t kIntFlags = PageFlags::Present | PageFlags::Writable | PageFlags::User;

    uint64_t* pml4 = hhdm_ptr(pml4_phys);

    // PML4 -> PDPT
    if (!(pml4[i4] & PageFlags::Present)) {
        uint64_t pdpt_phys = alloc_table_phys();
        if (!pdpt_phys) return false;
        pml4[i4] = make_pte(pdpt_phys, kIntFlags);
    }
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));

    // PDPT -> PD
    if (!(pdpt[i3] & PageFlags::Present)) {
        uint64_t pd_phys = alloc_table_phys();
        if (!pd_phys) return false;
        pdpt[i3] = make_pte(pd_phys, kIntFlags);
    }
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));

    // PD may be a huge page (2MB) — split it before adding 4K PTEs
    if (pd[i2] & PageFlags::Huge) {
        uint64_t huge_pa = pte_phys_addr(pd[i2]);
        uint64_t huge_flags = pd[i2] & ~PageFlags::Huge;

        uint64_t pt_phys = alloc_table_phys();
        if (!pt_phys) return false;

        uint64_t* pt_virt = hhdm_ptr(pt_phys);
        for (int j = 0; j < 512; j++) {
            pt_virt[j] = make_pte(huge_pa + j * PAGE_SIZE, huge_flags);
        }

        pd[i2] = make_pte(pt_phys, kIntFlags);
    } else if (!(pd[i2] & PageFlags::Present)) {
        uint64_t pt_phys = alloc_table_phys();
        if (!pt_phys) return false;
        pd[i2] = make_pte(pt_phys, kIntFlags);
    }

    // PT -> leaf entry
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    *pte_out = &pt[i1];
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
    uint64_t* old_pml4 = hhdm_ptr(old_cr3);

    // Build new PML4
    uint64_t new_pml4_phys = alloc_table_phys();
    if (!new_pml4_phys) KPANIC("paging_init: failed to allocate PML4");
    uint64_t* new_pml4 = hhdm_ptr(new_pml4_phys);

    // Copy ALL Limine entries EXCEPT the kernel's PML4 entry
    uint16_t ki4 = pml4_index(kernel_virt_base);
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = (i == ki4) ? 0 : old_pml4[i];
    }

    // Map kernel pages as 4K mappings.
    // Don't assume kernel_phys_base == kernel_virt_base - hhdm — Limine loads
    // the kernel ELF at some physical address that may differ from the simple
    // linear calculation. Instead, look up each page from Limine's PML4.
    uint64_t kernel_pages = (kernel_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kernel_pages += PAGE_SIZE * 16;  // Extra 64KB for BSS tail + stack beyond _end
    klog("Paging: mapping kernel (");
    klog_hex(kernel_pages);
    klog(" bytes)...\n");

    for (uint64_t off = 0; off < kernel_pages; off += PAGE_SIZE) {
        uint64_t va = kernel_virt_base + off;
        // Look up physical address from Limine's existing PML4
        uint64_t pa = page_table_lookup(old_cr3, va);
        if (!pa) {
            // Fallback: assume linear mapping from kernel_phys_base
            pa = kernel_phys_base + off;
        }
        uint64_t* pte = nullptr;
        if (!walk_create(new_pml4_phys, va, &pte)) {
            KPANIC("paging_init: OOM during kernel mapping");
        }
        *pte = make_pte(pa, PageFlags::Present | PageFlags::Writable);
    }

    // Map direct-map region (DIRECT_MAP_BASE -> phys 0)
    // Use 2MB huge pages for efficiency. Map 128GB range.
    klog("Paging: mapping direct map...\n");
    // Clear any pre-existing direct map entry inherited from Limine's PML4
    // so that we always allocate fresh PDPT/PD page tables below.
    new_pml4[pml4_index(DIRECT_MAP_BASE)] = 0;
    for (uint64_t pa = 0; pa < 0x2000000000ULL; pa += LARGE_PAGE_SIZE) {
        uint64_t va = DIRECT_MAP_BASE + pa;
        uint16_t i4 = pml4_index(va);
        uint16_t i3 = pdpt_index(va);
        uint16_t i2 = pd_index(va);

        if (!(new_pml4[i4] & PageFlags::Present)) {
            uint64_t pdpt = alloc_table_phys();
            if (!pdpt) break;
            new_pml4[i4] = make_pte(pdpt, PageFlags::Present | PageFlags::Writable);
        }
        uint64_t* pdpt = hhdm_ptr(pte_phys_addr(new_pml4[i4]));

        if (!(pdpt[i3] & PageFlags::Present)) {
            uint64_t pd = alloc_table_phys();
            if (!pd) break;
            pdpt[i3] = make_pte(pd, PageFlags::Present | PageFlags::Writable);
        }
        uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));

        pd[i2] = make_pte(pa, PageFlags::Present | PageFlags::Writable | PageFlags::Huge);
    }

    // Load new page tables and cut over from Limine HHDM to kernel direct map
    uint64_t cr3_val = new_pml4_phys;
    uint64_t limine_hhdm = g_hhdm;
    asm volatile("mov %0, %%cr3" :: "r"(cr3_val) : "memory");
    g_hhdm = DIRECT_MAP_BASE;
    klog_reinit_fb(limine_hhdm);
    klog("Paging: kernel PML4 active\n");
}

void paging_save_kernel_template() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_template_pml4 = cr3;
    klog("Paging: kernel PML4 template saved at ");
    klog_hex(cr3); klog("\n");
}

uint64_t paging_kernel_pml4_template() {
    return g_kernel_template_pml4;
}

bool page_table_map(uint64_t pml4_phys, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t* pte = nullptr;
    if (!walk_create(pml4_phys, va, &pte)) return false;
    *pte = make_pte(pa, flags);
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    return true;
}

uint64_t page_table_unmap(uint64_t pml4_phys, uint64_t va) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    uint64_t* pml4 = hhdm_ptr(pml4_phys);
    if (!(pml4[i4] & PageFlags::Present)) return 0;
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));
    if (!(pdpt[i3] & PageFlags::Present)) return 0;
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));
    if (pd[i2] & PageFlags::Huge) return 0;
    if (!(pd[i2] & PageFlags::Present)) return 0;
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    if (!(pt[i1] & PageFlags::Present)) return 0;

    uint64_t old_pa = pte_phys_addr(pt[i1]);
    pt[i1] = 0;
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    return old_pa;
}

uint64_t page_table_lookup(uint64_t pml4_phys, uint64_t va) {
    uint16_t i4 = pml4_index(va);
    uint16_t i3 = pdpt_index(va);
    uint16_t i2 = pd_index(va);
    uint16_t i1 = pt_index(va);

    uint64_t* pml4 = hhdm_ptr(pml4_phys);
    if (!(pml4[i4] & PageFlags::Present)) return 0;
    uint64_t* pdpt = hhdm_ptr(pte_phys_addr(pml4[i4]));
    if (!(pdpt[i3] & PageFlags::Present)) return 0;
    uint64_t* pd = hhdm_ptr(pte_phys_addr(pdpt[i3]));
    if (pd[i2] & PageFlags::Huge) return 0;
    if (!(pd[i2] & PageFlags::Present)) return 0;
    uint64_t* pt = hhdm_ptr(pte_phys_addr(pd[i2]));
    if (!(pt[i1] & PageFlags::Present)) return 0;
    return pte_phys_addr(pt[i1]);
}
