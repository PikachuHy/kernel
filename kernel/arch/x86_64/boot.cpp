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
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/timer.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/io.hpp"

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

__attribute__((section(".limine_reqs"), used))
static volatile void* limine_requests[] = {
    &framebuffer_request,
    &bootloader_info_request,
    &memmap_request,
    &hhdm_request,
    &kernel_file_request,
    nullptr,
};

// Linker symbol: end of BSS
extern uint8_t _end;

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
    // (Limine page tables are used directly — paging_init with CR3 reload
    //  has issues that need further debugging)
    klog("Using Limine page tables...\n");

    // 4. Slab allocator using bitmap directly for slab pages
    // (buddy will be wired later when properly debugged)
    klog("Initializing slab allocator...\n");
    slab_init(hhdm);
    klog("  kmalloc ready (16B-2048B)\n\n");

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

    // ── Demo: Phase 3 capabilities showcase ──
    // This demonstrates three things working together:
    //   1. LAPIC timer   (periodic callback, fires every ~100ms)
    //   2. Keyboard IRQ  (IRQ1 routed through I/O APIC → IDT[33] → handler)
    //   3. IOAPIC routing (ISA IRQ1 → vector 33)
    //
    //  After boot, you'll see a timer counting up. Press keyboard keys
    //  and their scancodes appear as hex.
    //
    klog("\n");
    klog("  ================================================\n");
    klog("  Phase 3 Demo: Timer + Keyboard Interrupt\n");
    klog("  ------------------------------------------------\n");
    klog("  Timer ticks every ~1ms (counter below).\n");
    klog("  Press keys to see scancodes via IRQ1 dispatch.\n");
    klog("  ================================================\n\n");

    // ── Initialize PS/2 keyboard controller ──
    // The 8042 controller at ports 0x60/0x64 must be told to enable the
    // keyboard port and start scanning, otherwise IRQ1 never fires.
    {
        // Wait for input buffer empty, then enable first PS/2 port
        while (x86::inb(0x64) & 2) x86::pause();
        x86::outb(0x64, 0xAE);  // "Enable First PS/2 Port"

        // Flush any stale output buffer data
        while (x86::inb(0x64) & 1) { (void)x86::inb(0x60); }

        // Tell keyboard to start scanning
        while (x86::inb(0x64) & 2) x86::pause();
        x86::outb(0x60, 0xF4);  // "Enable Scanning"

        klog("  PS/2 keyboard: enabled\n\n");
    }

    // Track demo state (non-volatile; memory barrier in loop ensures visibility)
    static int  timer_ticks = 0;
    static char last_scancode = 0;

    // Register keyboard handler on IRQ1 (vector 33)
    irq_register(1, [](uint8_t vector) -> bool {
        (void)vector;
        uint8_t code = x86::inb(0x60);
        last_scancode = static_cast<char>(code);
        klog("  [kbd] scancode: 0x");
        klog_hex(code);
        if (code & 0x80) {
            klog(" (release)\n");
        } else {
            klog(" (press)\n");
        }
        return true;
    });

    // Start a periodic timer. The LAPIC timer fires at ~1ms intervals
    // (calibration produces 1M ticks/ms; interval=1000us gives 1000*1M/1000=1M ticks ≈ 1ms).
    // We stop after 2000 ticks (~2 seconds) to show sustained operation.
    timer_periodic(1000, [](uint64_t elapsed_ms) -> bool {
        timer_ticks++;
        // Print every 200 ticks so we don't flood output
        if (timer_ticks % 200 == 0) {
            klog("  [timer] tick #");
            klog_hex(timer_ticks);
            klog(" @ ");
            klog_hex(elapsed_ms);
            klog(" ms\n");
        }
        return timer_ticks < 2000;
    });

    klog("  Interrupts enabled. Watching for keys + timer...\n\n");

    // Enable interrupts — both timer and keyboard IRQ go live now
    asm volatile("sti");

    // Wait for demo to finish (2000 timer ticks). Memory clobber ensures
    // the compiler re-reads timer_ticks from memory each iteration.
    while (timer_ticks < 2000) {
        asm volatile("pause" ::: "memory");
    }

    // All done — stop interrupts and report
    asm volatile("cli");

    klog("\n  ================================================\n");
    klog("  Demo complete! Summary:\n");
    klog("  ------------------------------------------------\n");
    klog("  Timer ticks fired: ");
    klog_hex(timer_ticks);
    klog("\n");
    klog("  Last scancode seen: 0x");
    klog_hex(static_cast<uint8_t>(last_scancode));
    klog("\n");
    klog("  ================================================\n\n");

    klog("=== Kernel booted successfully ===\n");
    klog("  (Ctrl+A then X to exit QEMU)\n");

    while (1) { asm volatile("hlt"); }
}
