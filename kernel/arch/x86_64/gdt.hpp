#pragma once

#include <stdint.h>

struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high : 4;
    uint8_t  flags : 4;
    uint8_t  base_high;
} __attribute__((packed));

struct GDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

auto gdt_init() -> void;
auto tss_init() -> void;
auto tss_set_rsp0(uint64_t rsp0) -> void;
