#ifndef LIMINE_H
#define LIMINE_H

#include <stdint.h>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

/* Request-specific IDs (3rd and 4th values of id[4]) */
#define LIMINE_BOOTLOADER_INFO_REQUEST_ID \
    0xf55038d8e2a1202f, 0x279426fcf5f59740

#define LIMINE_FRAMEBUFFER_REQUEST_ID \
    0x9d5827dcd881dd75, 0xa3148604f6fab11b

#define LIMINE_MEMMAP_REQUEST_ID \
    0x67cf3d9d378a806f, 0xe304acdfc50c3c62

struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
};

#define LIMINE_MEDIA_TYPE_GENERIC  0
#define LIMINE_MEDIA_TYPE_OPTICAL  1
#define LIMINE_MEDIA_TYPE_TFTP     2

struct limine_file {
    uint64_t revision;
    uint64_t address;
    uint64_t size;
    char*    path;
    char*    cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct limine_uuid gpt_disk_uuid;
    struct limine_uuid gpt_part_uuid;
    struct limine_uuid part_uuid;
};

struct limine_bootloader_info_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_bootloader_info_response* response;
};

struct limine_bootloader_info_response {
    uint64_t revision;
    char*    name;
    char*    version;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response* response;
    uint16_t width;
    uint16_t height;
    uint16_t bpp;
    uint16_t unused;
};

struct limine_framebuffer {
    uint64_t address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    void*    edid;
};

#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer** framebuffers;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response* response;
};

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry** entries;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response* response;
};

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_module_response* response;
    uint64_t internal_module_count;
    uint64_t* internal_modules;
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    struct limine_file** modules;
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response* response;
};

struct limine_rsdp_response {
    uint64_t revision;
    void*    address;
};

struct limine_smbios_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_smbios_response* response;
};

struct limine_smbios_response {
    uint64_t revision;
    void*    entry_32;
    void*    entry_64;
};

struct limine_smp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_smp_response* response;
    uint64_t flags;
};

struct limine_smp_info;

typedef void (*limine_goto_address)(struct limine_smp_info*);

struct limine_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    limine_goto_address goto_address;
    uint64_t extra_argument;
};

struct limine_smp_response {
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    struct limine_smp_info** cpus;
};

struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_address_response* response;
};

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_file_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_file_response* response;
};

struct limine_kernel_file_response {
    uint64_t revision;
    struct limine_file* kernel_file;
};

struct limine_efi_system_table_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_efi_system_table_response* response;
};

struct limine_efi_system_table_response {
    uint64_t revision;
    void*    address;
};

#endif
