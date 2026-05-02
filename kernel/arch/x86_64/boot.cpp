#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"

static uint8_t boot_stack[65536];

static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_FRAMEBUFFER_REQUEST_ID},
    .revision = 0,
    .response = nullptr,
};

static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_BOOTLOADER_INFO_REQUEST_ID},
    .revision = 0,
    .response = nullptr,
};

static volatile struct limine_memmap_request memmap_request = {
    .id = {LIMINE_COMMON_MAGIC, LIMINE_MEMMAP_REQUEST_ID},
    .revision = 0,
    .response = nullptr,
};

__attribute__((section(".limine_reqs"), used))
static volatile void* limine_requests[] = {
    &framebuffer_request,
    &bootloader_info_request,
    &memmap_request,
    nullptr,
};

extern "C" void kernel_entry(void) {
    // Set up our own stack
    asm volatile("movq %0, %%rsp" : : "r"(&boot_stack[sizeof(boot_stack)]));

    serial_init();

    klog_init(
        framebuffer_request.response ? framebuffer_request.response->framebuffers[0] : nullptr
    );

    klog("\n=== C++26 Kernel ===\n");

    if (bootloader_info_request.response) {
        klog("Bootloader: ");
        klog(bootloader_info_request.response->name);
        klog(" ");
        klog(bootloader_info_request.response->version);
        klog("\n\n");
    }

    klog("Initializing GDT...\n");
    gdt_init();
    klog("GDT initialized.\n");

    klog("Initializing IDT...\n");
    idt_init();
    klog("IDT initialized.\n");

    if (memmap_request.response) {
        klog("Memory map entries: ");
        klog_hex(memmap_request.response->entry_count);
        klog("\n");
    }

    klog("\nKernel booted successfully.\n");

    while (1) {
        asm volatile("hlt");
    }
}
