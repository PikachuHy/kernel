#include <stdint.h>
#include <stddef.h>
#include "stivale2.h"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/serial.hpp"
#include "kernel/lib/panic.hpp"

static uint8_t boot_stack[65536];

static stivale2_header_tag_framebuffer fb_request = {
    .tag = {.identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID, .next = 0},
    .framebuffer_width = 0,
    .framebuffer_height = 0,
    .framebuffer_bpp = 0,
};

__attribute__((section(".stivale2hdr"), used))
static stivale2_header header = {
    .entry_point = 0,
    .stack = (uint64_t)boot_stack + sizeof(boot_stack),
    .flags = 0,
    .tags = (uint64_t)&fb_request,
};

static stivale2_struct_tag_memmap* get_memmap(stivale2_struct* info) {
    stivale2_tag* tag = (stivale2_tag*)info->tags;
    while (tag) {
        if (tag->identifier == STIVALE2_STRUCT_TAG_MEMMAP_ID) {
            return (stivale2_struct_tag_memmap*)tag;
        }
        tag = (stivale2_tag*)tag->next;
    }
    return nullptr;
}

static void debug_outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

extern "C" void kernel_entry(stivale2_struct* info) {
    // QEMU debug port — appears on console with -debugcon stdio
    debug_outb(0xE9, 'K');
    debug_outb(0xE9, '\n');

    serial_init();
    debug_outb(0xE9, 'S');
    klog_init(info);
    debug_outb(0xE9, 'F');

    klog("\n=== C++26 Kernel ===\n");
    klog("Bootloader: ");
    klog(info->bootloader_brand);
    klog(" ");
    klog(info->bootloader_version);
    klog("\n\n");

    klog("Initializing GDT...\n");
    gdt_init();
    klog("GDT initialized.\n");

    klog("Initializing IDT...\n");
    idt_init();
    klog("IDT initialized.\n");

    auto* memmap = get_memmap(info);
    if (memmap) {
        klog("Memory map entries: ");
        klog_hex(memmap->entries);
        klog("\n");
    }

    klog("\nKernel booted successfully.\n");

    while (1) {
        asm volatile("hlt");
    }
}
