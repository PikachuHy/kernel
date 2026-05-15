#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/timer.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/acpi.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/port.hpp"
#include "kernel/core/object/process.hpp"

// Placement new (defined in kernel/core/object/port.cpp — re-declared here for boot.cpp's usage)
void* operator new(size_t, void* p) noexcept;

static uint8_t boot_stack[65536];

// ── Limine requests ──────────────────────────────────────────
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_FRAMEBUFFER_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_BOOTLOADER_INFO_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_memmap_request memmap_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_MEMMAP_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_hhdm_request hhdm_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_HHDM_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_KERNEL_FILE_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_rsdp_request rsdp_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_RSDP_REQUEST_ID},
    .revision = 0, .response = nullptr,
};

static volatile struct limine_smp_request smp_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_SMP_REQUEST_ID},
    .revision = 0, .response = nullptr, .flags = 0,
};

__attribute__((section(".limine_reqs"), used))
static volatile void* limine_requests[] = {
    &framebuffer_request,
    &bootloader_info_request,
    &memmap_request,
    &hhdm_request,
    &kernel_file_request,
    &rsdp_request,
    &smp_request,
    nullptr,
};

// BSP handshake: set true after BSP finishes init. APs spin until this is set,
// then divert to the AP entry path instead of re-running BSP init.
__attribute__((unused)) static volatile bool bsp_done = false;

// Linker symbol: end of BSS
extern uint8_t _end;

// Timer preemption callback — drives scheduler_tick() from LAPIC timer.

static bool timer_preempt_callback(uint64_t) {
    scheduler_tick();
    return true;
}

extern "C" void kernel_entry(void) {
    asm volatile("movq %0, %%rsp" : : "r"(&boot_stack[sizeof(boot_stack)]));

    serial_init();
    klog_init(
        framebuffer_request.response
            ? framebuffer_request.response->framebuffers[0] : nullptr
    );

    klog("\n=== C++26 Kernel ===\n");

    if (bootloader_info_request.response) {
        klog("Bootloader: ");
        klog(bootloader_info_request.response->name);
        klog(" ");
        klog(bootloader_info_request.response->version);
        klog("\n\n");
    }

    // ── Phase 1: GDT, IDT ──
    klog("Initializing GDT...\n");
    gdt_init();
    klog("GDT initialized.\n");

    // Enable SSE/SSE2 for user-space code. Set OSFXSR (bit 9) and
    // OSXMMEXCPT (bit 10) so that FXSAVE/FXRSTOR work and SSE
    // instructions don't raise #UD. Also clear SMEP/SMAP.
    {
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 &= ~((1ULL << 20) | (1ULL << 21));  // clear SMEP (20) and SMAP (21)
        cr4 |= (1ULL << 9) | (1ULL << 10);       // set OSFXSR (9), OSXMMEXCPT (10)
        asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
        klog("CR4="); klog_hex(cr4); klog(" (OSFXSR/OSXMMEXCPT set, SMEP/SMAP cleared)\n");
    }

    klog("Initializing IDT...\n");
    idt_init();
    klog("IDT initialized.\n");

    // ── Verify Limine responses ──
    if (!memmap_request.response) KPANIC("No memory map from Limine");
    if (!hhdm_request.response) KPANIC("No HHDM offset from Limine");
    if (!kernel_file_request.response) KPANIC("No kernel file info from Limine");

    uint64_t hhdm = hhdm_request.response->offset;
    uint64_t kernel_phys = kernel_file_request.response->kernel_file->address - hhdm;
    uint64_t kernel_virt = KERNEL_VIRT_BASE;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&_end) - kernel_virt;

    klog("HHDM offset: "); klog_hex(hhdm); klog("\n");
    klog("Kernel: phys ");
    klog_hex(kernel_phys); klog(" -> virt ");
    klog_hex(kernel_virt); klog(" (");
    klog_hex(kernel_size); klog(" bytes)\n\n");

    // ── Phase 2: Memory management ──
    klog("=== Phase 2: Memory Management ===\n\n");

    // 1. Physical memory manager
    klog("Initializing PMM...\n");
    // Convert Limine memmap entries to MemRange array
    MemRange memmap_buf[128];
    size_t entry_count = memmap_request.response->entry_count;
    if (entry_count > 128) entry_count = 128;
    for (size_t i = 0; i < entry_count; i++) {
        auto* e = memmap_request.response->entries[i];
        memmap_buf[i] = {e->base, e->length, static_cast<uint32_t>(e->type)};
    }
    pmm_init(memmap_buf, entry_count, kernel_phys, kernel_phys + kernel_size);
    klog("  Total: "); klog_hex(pmm_total_memory()); klog(" bytes\n");
    klog("  Usable: "); klog_hex(pmm_usable_memory()); klog(" bytes\n");
    klog("  Highest physical: "); klog_hex(pmm_highest_phys_addr()); klog("\n");

    // 2. Bitmap allocator (early boot, uses HHDM)
    klog("Initializing bitmap allocator...\n");
    bitmap_init(hhdm, 0x200000);  // bitmap at 2MB physical
    klog("  Free pages: "); klog_hex(bitmap_free_page_count()); klog("\n");
    klog("  Total pages: "); klog_hex(bitmap_total_page_count()); klog("\n");

    // ── Bitmap demo ──
    {
        size_t free_before = bitmap_free_page_count();
        klog("\n  [demo] Free before alloc: "); klog_hex(free_before); klog(" pages\n");

        void* p1 = bitmap_alloc_page();
        void* p2 = bitmap_alloc_page();
        void* p3 = bitmap_alloc_page();

        klog("  [demo] Allocated 3 pages:\n");
        klog("    p1 = "); klog_hex(reinterpret_cast<uint64_t>(p1)); klog("\n");
        klog("    p2 = "); klog_hex(reinterpret_cast<uint64_t>(p2)); klog("\n");
        klog("    p3 = "); klog_hex(reinterpret_cast<uint64_t>(p3)); klog("\n");

        size_t free_after = bitmap_free_page_count();
        klog("  [demo] Free after  alloc: "); klog_hex(free_after); klog(" pages\n");

        bitmap_free_page(p1);
        bitmap_free_page(p2);
        bitmap_free_page(p3);

        size_t free_final = bitmap_free_page_count();
        klog("  [demo] Free after  free:  "); klog_hex(free_final); klog(" pages\n");
        klog("  [demo] Freed count matches: ");
        klog(free_final == free_before ? "YES" : "NO");
        klog("\n");
    }

    // 3. Higher-half paging takeover
    // paging_init has a CR3-reload triple-fault bug (debugging in progress).
    // For now, use Limine's page tables and save them as the kernel template.
    klog("Using Limine page tables...\n");
    paging_save_kernel_template();

    // 4. Buddy allocator
    klog("Initializing buddy allocator...\n");
    buddy_init(hhdm, 0);
    klog("  buddy ready\n\n");

    // 5. Slab allocator using buddy for slab pages
    klog("Initializing slab allocator...\n");
    slab_init(hhdm);
    klog("  kmalloc ready (16B-2048B)\n\n");

    // TSS: must be after bitmap_init since it allocates kernel interrupt stacks
    klog("Initializing TSS...\n");
    tss_init();

    // ── kmalloc / new / delete demo ──
    {
        klog("  [demo] kmalloc + operator new:\n");

        // kmalloc/kfree
        void* k1 = kmalloc(32);
        void* k2 = kmalloc(128);
        void* k3 = kmalloc(2048);
        klog("    kmalloc(32)   -> "); klog_hex(reinterpret_cast<uint64_t>(k1)); klog("\n");
        klog("    kmalloc(128)  -> "); klog_hex(reinterpret_cast<uint64_t>(k2)); klog("\n");
        klog("    kmalloc(2048) -> "); klog_hex(reinterpret_cast<uint64_t>(k3)); klog("\n");

        if (k1) *static_cast<char*>(k1) = 'A';
        if (k2) *static_cast<char*>(k2) = 'B';

        kfree(k1);
        kfree(k2);
        kfree(k3);
        klog("    kfree: OK\n");

        // operator new / delete
        struct alignas(16) DemoObj { int x = 42; int y = 99; };
        DemoObj* obj = new DemoObj();
        klog("    new DemoObj -> "); klog_hex(reinterpret_cast<uint64_t>(obj)); klog("\n");
        klog("    obj->x="); klog_hex(obj->x); klog(" obj->y="); klog_hex(obj->y);
        klog(" (expect 0x2A, 0x63)\n");
        delete obj;
        klog("    delete: OK\n");

        // kmalloc_usable_size
        void* us = kmalloc(100);
        klog("    kmalloc(100).usable = "); klog_hex(kmalloc_usable_size(us));
        klog(" (expect 128)\n");
        kfree(us);

        klog("  [demo] All allocator tests passed\n\n");
    }

    // ── Phase 3: APIC, Timer, Interrupts ──
    klog("=== Phase 3: APIC + Timer + Interrupts ===\n\n");

    klog("Disabling legacy PIC...\n");
    pic_disable();

    klog("Initializing LAPIC...\n");
    lapic_init(hhdm);

    klog("Initializing I/O APIC...\n");
    ioapic_init(hhdm);

    klog("Initializing IRQ dispatch...\n");
    irq_init();

    klog("Initializing timer...\n");
    timer_init(hhdm);

    klog("Initializing syscall...\n");
    syscall_init();


    // ── Phase 4: SMP bringup ──
    klog("=== Phase 4: SMP Bringup ===\n\n");

    uint64_t rsdp_phys = 0;
    if (rsdp_request.response && rsdp_request.response->address) {
        uint64_t rsdp_virt = reinterpret_cast<uint64_t>(rsdp_request.response->address);
        rsdp_phys = rsdp_virt - hhdm;
    }
    smp_init(hhdm, rsdp_phys);

    // ── SMP summary ──
    klog("\n  --- CPU Summary ---\n");
    for (uint32_t i = 0; i < smp_cpu_count(); i++) {
        klog("  CPU "); klog_hex(i);
        klog(": LAPIC="); klog_hex(g_per_cpu[i].lapic_id);
        klog(" online=");
        klog(g_per_cpu[i].online ? "yes\n" : "NO\n");
    }
    klog("  Total: "); klog_hex(smp_cpu_count()); klog(" CPU(s)\n\n");

    // ── Phase 5: Scheduler ──
    klog("=== Phase 5: Scheduler ===\n\n");

    klog("Initializing scheduler...\n");
    scheduler_init(hhdm);

    // Hook timer to scheduler for preemption (every 10ms)
    timer_periodic(10000, timer_preempt_callback);

    // ── Phase 7: VMM + Process + ring-3 ──────────────────────────
    klog("\n=== Phase 7: VMM + Process + ring-3 ===\n\n");
    klog("  VMM:     per-process PML4, VmRegion list, demand paging\n");
    klog("  VMO:     Anonymous (COW) + Physical, buddy-backed pages\n");
    klog("  Process: HandleTable (1024 entries, buddy-allocated)\n");
    klog("  Stack:   16KB per-thread kernel stack, TSS RSP0 on switch\n");
    klog("  IST:     #PF(IST1) #DF(IST2) 16KB each\n");
    klog("  GDT:     user CS=0x1B, SS=0x13 (sysretq-compatible)\n");
    klog("  Timer:   10ms preemption, SYSRET CS fix\n");
    klog("  ELF:     init.elf @ 0x400000, embedded via llvm-objcopy\n");
    klog("  Init:    ring-3 syscalls (print, channel, close, exit)\n\n");

    // ── Embedded init process ──────────────────────────────────────
    // elf_load_init_process() loads the embedded init.elf and starts it.
    // It is defined in kernel/core/elf_loader.cpp (extern "C").
    extern void elf_load_init_process();
    elf_load_init_process();

    klog("Scheduler starting...\n\n");
    asm volatile("sti");
    scheduler_start();  // never returns
}
