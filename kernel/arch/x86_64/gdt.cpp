#include "kernel/arch/x86_64/gdt.hpp"
#include <stddef.h>
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"
#include "kernel/lib/panic.hpp"

namespace {

constexpr int GDT_ENTRIES = 7;
GDTEntry gdt[GDT_ENTRIES];
GDTR gdtr;

auto set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) -> void {
    gdt[idx].base_low = base & 0xFFFF;
    gdt[idx].base_mid = (base >> 16) & 0xFF;
    gdt[idx].base_high = (base >> 24) & 0xFF;
    gdt[idx].limit_low = limit & 0xFFFF;
    gdt[idx].limit_high = (limit >> 16) & 0xF;
    gdt[idx].access = access;
    gdt[idx].flags = flags;
}

} // namespace

struct TSS {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_bitmap_offset;
} __attribute__((packed));

static uint8_t g_tss_data[sizeof(TSS) + 8192 + 1]
    __attribute__((aligned(PAGE_SIZE)));

auto gdt_init() -> void {
    // GDT layout chosen so that SYSRET computes correct CS/SS.
    // SYSRET CS = (STAR[63:48] + 16) | 3 → (0x08+16)|3 = 0x1B (entry 3)
    // SYSRET SS = (STAR[63:48] + 8)  | 3 → (0x08+8)|3  = 0x13 (entry 2)
    // 0: null
    set_entry(0, 0, 0, 0, 0);
    // 1: kernel code 64-bit (0x08)
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA);
    // 2: user data (0x13) — DPL=3 for SYSRET SS compatibility
    set_entry(2, 0, 0xFFFFF, 0xF2, 0xC);
    // 3: user code 64-bit (0x1B)
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xA);
    // 4: kernel data (0x20)
    set_entry(4, 0, 0xFFFFF, 0x92, 0xC);
    // 5: TSS low
    // 6: TSS high

    gdtr.limit = sizeof(GDTEntry) * GDT_ENTRIES - 1;
    gdtr.base = (uint64_t)&gdt[0];

    asm volatile(
        "lgdt (%0)\n"
        "movw $0x20, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "r"(&gdtr)
        : "rax", "memory"
    );
}

auto tss_init() -> void {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    for (size_t i = 0; i < sizeof(TSS); i++) {
        reinterpret_cast<uint8_t*>(tss)[i] = 0;
    }
    tss->io_bitmap_offset = sizeof(TSS);

    // RSP0: shared kernel stack for ring-3 → ring-0 transitions.
    void* kstack_phys = buddy_alloc_pages(2);  // 16KB
    if (!kstack_phys) KPANIC("TSS: failed to allocate RSP0 stack");
    tss->rsp0 = DIRECT_MAP_BASE + reinterpret_cast<uint64_t>(kstack_phys) + PAGE_SIZE * 4;

    // IST1 (tss->ist[0]): dedicated stack for #PF (vector 14).
    // Prevents deep HandlePageFault call chains from corrupting the
    // IRET frame on the shared RSP0 stack.
    void* pf_ist_phys = buddy_alloc_pages(2);  // 16KB
    if (!pf_ist_phys) KPANIC("TSS: failed to allocate #PF IST stack");
    tss->ist[0] = DIRECT_MAP_BASE + reinterpret_cast<uint64_t>(pf_ist_phys) + PAGE_SIZE * 4;

    // IST2 (tss->ist[1]): dedicated stack for #DF (vector 8).
    void* df_ist_phys = buddy_alloc_pages(2);  // 16KB
    if (!df_ist_phys) KPANIC("TSS: failed to allocate #DF IST stack");
    tss->ist[1] = DIRECT_MAP_BASE + reinterpret_cast<uint64_t>(df_ist_phys) + PAGE_SIZE * 4;

    uint8_t* bitmap = &g_tss_data[sizeof(TSS)];
    for (int i = 0; i < 8192; i++) bitmap[i] = 0xFF;
    g_tss_data[sizeof(g_tss_data) - 1] = 0xFF;

    uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);
    uint64_t limit = sizeof(g_tss_data) - 1;

    set_entry(5, tss_addr & 0xFFFFFFFF, limit & 0xFFFFF, 0x89, 0);
    *reinterpret_cast<uint64_t*>(&gdt[6]) = (tss_addr >> 32) & 0xFFFFFFFF;

    asm volatile(
        "ltr %%ax\n"
        :
        : "a"(0x28)
        : "memory"
    );

    klog("TSS: loaded, RSP0=");
    klog_hex(tss->rsp0);
    klog(" IST1=");
    klog_hex(tss->ist[0]);
    klog(" IST2=");
    klog_hex(tss->ist[1]);
    klog(" (I/O bitmap: all ports denied from ring 3)\n");
}

auto tss_set_rsp0(uint64_t rsp0) -> void {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    tss->rsp0 = rsp0;
}
