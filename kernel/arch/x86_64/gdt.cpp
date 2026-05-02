#include "kernel/arch/x86_64/gdt.hpp"

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
