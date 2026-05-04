#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/mm/pmm.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"

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

    // 3. Higher-half paging takeover
    klog("Setting up higher-half paging...\n");
    paging_init(hhdm, kernel_phys, kernel_virt, kernel_size);
    klog("  Paging active\n");

    // 4. Buddy allocator (WIP — crashes, needs debug)
    // buddy_init(hhdm);
    // 5. Slab allocator (WIP)
    // slab_init(hhdm);
    klog("  Buddy/slab skipped (WIP)\n");

    klog("\n=== Kernel booted successfully ===\n");

    while (1) {
        asm volatile("hlt");
    }
}
