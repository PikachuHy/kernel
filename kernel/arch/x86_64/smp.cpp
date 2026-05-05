#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/arch/x86_64/acpi.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/lib/klog.hpp"

PerCpu g_per_cpu[MAX_CPUS];
uint32_t g_cpu_count = 0;

// Trampoline symbols (defined in trampoline.S)
extern "C" {
    extern uint8_t trampoline_start[];
    extern uint8_t trampoline_end[];
    extern uint64_t tr_cr3;
    extern uint64_t tr_entry;
    extern uint64_t tr_id;
    extern uint8_t tr_gdt[];
    extern uint8_t tr_gdtr[];
}

void spinlock_acquire(Spinlock* lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        asm volatile("pause" ::: "memory");
    }
}

void spinlock_release(Spinlock* lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

// Per-CPU AP stacks and saved HHDM
alignas(16) static uint8_t ap_stacks[MAX_CPUS][65536];
static uint64_t g_smp_hhdm = 0;

// Compute trampoline symbol offsets and patch data fields.
static void patch_trampoline_data(uint64_t tramp_phys, uint64_t cr3) {
    uint8_t* tramp_virt = reinterpret_cast<uint8_t*>(g_smp_hhdm + tramp_phys);

    // Patch CR3 and entry point
    uint64_t off_cr3   = reinterpret_cast<uint64_t>(&tr_cr3)   - reinterpret_cast<uint64_t>(trampoline_start);
    uint64_t off_entry = reinterpret_cast<uint64_t>(&tr_entry) - reinterpret_cast<uint64_t>(trampoline_start);
    *reinterpret_cast<uint64_t*>(tramp_virt + off_cr3)   = cr3;
    *reinterpret_cast<uint64_t*>(tramp_virt + off_entry) = reinterpret_cast<uint64_t>(smp_ap_entry);

    // Patch GDTR base: the GDT is embedded in the trampoline page.
    // The GDTR needs the PHYSICAL address of the GDT.
    uint64_t gdt_phys = tramp_phys + (reinterpret_cast<uint64_t>(tr_gdt) - reinterpret_cast<uint64_t>(trampoline_start));
    uint64_t off_gdtr = reinterpret_cast<uint64_t>(tr_gdtr) - reinterpret_cast<uint64_t>(trampoline_start);
    *reinterpret_cast<uint16_t*>(tramp_virt + off_gdtr)     = 7 * 8 - 1;
    *reinterpret_cast<uint32_t*>(tramp_virt + off_gdtr + 2) = static_cast<uint32_t>(gdt_phys);
}

uint32_t smp_init(uint64_t hhdm, uint64_t rsdp_phys) {
    g_smp_hhdm = hhdm;

    // 1. Discover CPUs via ACPI MADT
    const MADT* madt = nullptr;
    if (acpi_find_madt(hhdm, rsdp_phys, &madt) != 0) {
        klog("SMP: MADT not found, single-core boot\n");
        g_cpu_count = 1;
        g_per_cpu[0].cpu_id = 0;
        g_per_cpu[0].online = true;
        return 1;
    }

    CpuInfo cpus[ACPI_MAX_CPUS];
    int total_cpus = acpi_parse_cpus(madt, cpus, ACPI_MAX_CPUS);
    if (total_cpus <= 0) {
        klog("SMP: no CPUs in MADT, single-core boot\n");
        g_cpu_count = 1;
        g_per_cpu[0].cpu_id = 0;
        g_per_cpu[0].online = true;
        return 1;
    }

    if (total_cpus == 1) {
        klog("SMP: only 1 CPU found, single-core boot\n");
        g_cpu_count = 1;
        g_per_cpu[0].cpu_id = 0;
        g_per_cpu[0].lapic_id = cpus[0].lapic_id;
        g_per_cpu[0].online = true;
        return 1;
    }

    klog("SMP: "); klog_hex(total_cpus); klog(" CPU(s) in MADT\n");

    // 2. Allocate trampoline page below 1MB
    void* tramp_phys_ptr = bitmap_alloc_page();
    if (!tramp_phys_ptr) {
        klog("SMP: failed to allocate trampoline page\n");
        g_cpu_count = 1;
        g_per_cpu[0].cpu_id = 0;
        g_per_cpu[0].online = true;
        return 1;
    }
    uint64_t tramp_phys = reinterpret_cast<uint64_t>(tramp_phys_ptr);
    if (tramp_phys >= 0x100000) {
        klog("SMP: trampoline above 1MB, single-core boot\n");
        g_cpu_count = 1;
        g_per_cpu[0].cpu_id = 0;
        g_per_cpu[0].online = true;
        return 1;
    }
    klog("SMP: trampoline at phys "); klog_hex(tramp_phys); klog("\n");

    // 3. Copy trampoline code to physical page
    size_t tramp_size = trampoline_end - trampoline_start;
    uint8_t* tramp_virt = reinterpret_cast<uint8_t*>(hhdm + tramp_phys);
    for (size_t i = 0; i < tramp_size; i++) {
        tramp_virt[i] = trampoline_start[i];
    }

    // 4. Get CR3 and patch trampoline data fields
    uint64_t cr3_val;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_val));
    patch_trampoline_data(tramp_phys, cr3_val);

    // 5. Register BSP
    g_per_cpu[0].cpu_id = 0;
    g_per_cpu[0].lapic_id = cpus[0].lapic_id;
    g_per_cpu[0].online = true;
    g_cpu_count = 1;

    // 6. Bring up APs
    for (int i = 1; i < total_cpus && i < MAX_CPUS; i++) {
        if (!cpus[i].enabled) {
            klog("SMP: CPU "); klog_hex(i); klog(" disabled, skipping\n");
            continue;
        }

        // Set AP identity
        uint64_t off_id = reinterpret_cast<uint64_t>(&tr_id) - reinterpret_cast<uint64_t>(trampoline_start);
        uint64_t ap_id = (static_cast<uint64_t>(cpus[i].lapic_id) << 32) | static_cast<uint64_t>(i);
        *reinterpret_cast<uint64_t*>(tramp_virt + off_id) = ap_id;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Send INIT-SIPI sequence
        uint8_t sipi_vector = static_cast<uint8_t>(tramp_phys >> 12);
        lapic_send_init(cpus[i].lapic_id);
        lapic_send_sipi(cpus[i].lapic_id, sipi_vector);

        // Poll for AP online
        for (int timeout = 0; timeout < 100000000; timeout++) {
            if (g_per_cpu[i].online) break;
            asm volatile("pause" ::: "memory");
        }

        if (g_per_cpu[i].online) {
            g_cpu_count++;
            klog("SMP: CPU "); klog_hex(i); klog(" online (LAPIC ID=");
            klog_hex(cpus[i].lapic_id); klog(")\n");
        } else {
            klog("SMP: CPU "); klog_hex(i); klog(" FAILED to come online\n");
        }
    }

    klog("SMP: "); klog_hex(g_cpu_count); klog(" CPU(s) online\n");
    return g_cpu_count;
}

extern "C" void smp_ap_entry(uint64_t id) {
    uint32_t cpu_id   = static_cast<uint32_t>(id & 0xFFFFFFFF);
    uint32_t lapic_id = static_cast<uint32_t>(id >> 32);

    // Switch to this AP's own stack
    uint8_t* stack_top = ap_stacks[cpu_id] + sizeof(ap_stacks[cpu_id]);
    asm volatile("movq %0, %%rsp" : : "r"(stack_top));

    // Load AP's own GDTR (GDT table shared, but GDTR is per-CPU register)
    gdt_init();

    // Enable this AP's local APIC
    lapic_init(g_smp_hhdm);

    // Mark online
    g_per_cpu[cpu_id].cpu_id   = cpu_id;
    g_per_cpu[cpu_id].lapic_id = lapic_id;
    g_per_cpu[cpu_id].online   = true;

    klog("AP "); klog_hex(cpu_id); klog(" online\n");

    // Park in idle loop
    asm volatile("sti");
    while (1) {
        asm volatile("hlt");
    }
}

uint32_t smp_cpu_count() {
    return g_cpu_count;
}
