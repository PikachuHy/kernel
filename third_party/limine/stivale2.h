#ifndef STIVALE2_H
#define STIVALE2_H

#include <stdint.h>

struct stivale2_tag {
    uint64_t identifier;
    uint64_t next;
} __attribute__((packed));

struct stivale2_header {
    uint64_t entry_point;
    uint64_t stack;
    uint64_t flags;
    uint64_t tags;
} __attribute__((packed));

#define STIVALE2_HEADER_TAG_FRAMEBUFFER_ID 0x3ecc1bc43d0f7971
#define STIVALE2_HEADER_TAG_TERMINAL_ID    0xa85d499b1823be72
#define STIVALE2_HEADER_TAG_SMP_ID         0x1ab015085f3273df
#define STIVALE2_HEADER_TAG_5LV_PAGING_ID  0x932f477032007e8f

struct stivale2_header_tag_framebuffer {
    struct stivale2_tag tag;
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_bpp;
    uint16_t unused;
} __attribute__((packed));

struct stivale2_header_tag_terminal {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t callback;
} __attribute__((packed));

struct stivale2_header_tag_smp {
    struct stivale2_tag tag;
    uint64_t flags;
} __attribute__((packed));

struct stivale2_struct {
    char bootloader_brand[64];
    char bootloader_version[64];
    uint64_t tags;
} __attribute__((packed));

#define STIVALE2_STRUCT_TAG_CMDLINE_ID      0xe5e76a1b4597a781
#define STIVALE2_STRUCT_TAG_MEMMAP_ID       0x2187f79e8612de07
#define STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID  0x506461d2950408fa
#define STIVALE2_STRUCT_TAG_TERMINAL_ID     0xc2b3f4c3233b0974
#define STIVALE2_STRUCT_TAG_MODULES_ID      0x4b6fe466aade04ce
#define STIVALE2_STRUCT_TAG_RSDP_ID         0x9e1786930a375e78
#define STIVALE2_STRUCT_TAG_EPOCH_ID        0x566a7bed888e1407
#define STIVALE2_STRUCT_TAG_SMBIOS_ID       0x2792e5efe28c4de2
#define STIVALE2_STRUCT_TAG_SMP_ID          0x34d1d96339647025

struct stivale2_struct_tag_cmdline {
    struct stivale2_tag tag;
    uint64_t cmdline;
} __attribute__((packed));

struct stivale2_struct_tag_memmap {
    struct stivale2_tag tag;
    uint64_t entries;
    uint64_t memmap;
} __attribute__((packed));

enum {
    STIVALE2_MMAP_USABLE = 1,
    STIVALE2_MMAP_RESERVED = 2,
    STIVALE2_MMAP_ACPI_RECLAIMABLE = 3,
    STIVALE2_MMAP_ACPI_NVS = 4,
    STIVALE2_MMAP_BAD_MEMORY = 5,
    STIVALE2_MMAP_BOOTLOADER_RECLAIMABLE = 0x1000,
    STIVALE2_MMAP_KERNEL_AND_MODULES = 0x1001,
    STIVALE2_MMAP_FRAMEBUFFER = 0x1002,
};

struct stivale2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t unused;
} __attribute__((packed));

struct stivale2_struct_tag_framebuffer {
    struct stivale2_tag tag;
    uint64_t framebuffer_addr;
    uint16_t framebuffer_width;
    uint16_t framebuffer_height;
    uint16_t framebuffer_pitch;
    uint16_t framebuffer_bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused;
} __attribute__((packed));

struct stivale2_struct_tag_terminal {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t cols;
    uint64_t rows;
    uint64_t term_write;
    uint64_t max_length;
} __attribute__((packed));

struct stivale2_struct_tag_modules {
    struct stivale2_tag tag;
    uint64_t module_count;
    uint64_t modules;
} __attribute__((packed));

struct stivale2_module {
    uint64_t begin;
    uint64_t end;
    char string[128];
} __attribute__((packed));

struct stivale2_struct_tag_rsdp {
    struct stivale2_tag tag;
    uint64_t rsdp;
} __attribute__((packed));

struct stivale2_struct_tag_smbios {
    struct stivale2_tag tag;
    uint64_t flags;
    uint64_t smbios_entry_32;
    uint64_t smbios_entry_64;
} __attribute__((packed));

struct stivale2_struct_tag_smp {
    struct stivale2_tag tag;
    uint64_t flags;
    uint32_t bsp_lapic_id;
    uint32_t unused;
    uint64_t cpu_count;
    uint64_t cpus;
} __attribute__((packed));

struct stivale2_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t target_stack;
    uint64_t goto_address;
    uint64_t extra_argument;
} __attribute__((packed));

#endif
