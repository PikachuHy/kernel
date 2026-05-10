#include "kernel/arch/x86_64/gdt.hpp"
#include <stddef.h>
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

namespace {

constexpr int GDT_ENTRIES = 7;
GDTEntry gdt[GDT_ENTRIES];
GDTR gdtr;

void set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
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

void gdt_init() {
    // 0: null
    set_entry(0, 0, 0, 0, 0);
    // 1: kernel code (64-bit, ring 0)
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA);
    // 2: kernel data (ring 0)
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC);
    // 3: user code (64-bit, ring 3) -- placeholder for Phase 5
    set_entry(3, 0, 0xFFFFF, 0xFA, 0xA);
    // 4: user data (ring 3) -- placeholder for Phase 5
    set_entry(4, 0, 0xFFFFF, 0xF2, 0xC);
    // 5: TSS low (placeholder)
    // 6: TSS high (placeholder)

    gdtr.limit = sizeof(GDTEntry) * GDT_ENTRIES - 1;
    gdtr.base = (uint64_t)&gdt[0];

    asm volatile(
        "lgdt (%0)\n"
        "movw $0x10, %%ax\n"
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

void tss_init() {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    for (size_t i = 0; i < sizeof(TSS); i++) {
        reinterpret_cast<uint8_t*>(tss)[i] = 0;
    }
    tss->io_bitmap_offset = sizeof(TSS);

    // Allocate a kernel interrupt stack for ring-3 -> ring-0 transitions.
    // When the CPU traps from ring 3 to ring 0, it loads RSP from tss->rsp0.
    void* kstack_phys = bitmap_alloc_page();
    if (!kstack_phys) KPANIC("TSS: failed to allocate kernel interrupt stack");
    // Use 2 pages for the kernel stack (8KB)
    void* kstack2_phys = bitmap_alloc_page();
    if (!kstack2_phys) KPANIC("TSS: failed to allocate 2nd kstack page");

    uint64_t kstack_base = reinterpret_cast<uint64_t>(kstack_phys);
    // Stack grows down from the top of the 8KB region
    tss->rsp0 = DIRECT_MAP_BASE + kstack_base + PAGE_SIZE * 2;

    uint8_t* bitmap = &g_tss_data[sizeof(TSS)];
    for (int i = 0; i < 8192; i++) bitmap[i] = 0xFF;
    g_tss_data[sizeof(g_tss_data) - 1] = 0xFF;

    uint64_t tss_addr = reinterpret_cast<uint64_t>(tss);
    uint64_t limit = sizeof(g_tss_data) - 1;

    set_entry(5, tss_addr & 0xFFFFFFFF, limit & 0xFFFFF, 0x89, 0);
    // Entry 6 for a 64-bit TSS is a raw 8-byte value:
    //   bits 0:31  = BASE[63:32]
    //   bits 32:63 = reserved (must be 0)
    *reinterpret_cast<uint64_t*>(&gdt[6]) = (tss_addr >> 32) & 0xFFFFFFFF;

    asm volatile(
        "ltr %%ax\n"
        :
        : "a"(0x28)
        : "memory"
    );

    klog("TSS: loaded, RSP0=");
    klog_hex(tss->rsp0);
    klog(" (I/O bitmap: all ports denied from ring 3)\n");
}

void tss_set_rsp0(uint64_t rsp0) {
    TSS* tss = reinterpret_cast<TSS*>(&g_tss_data[0]);
    tss->rsp0 = rsp0;
}
