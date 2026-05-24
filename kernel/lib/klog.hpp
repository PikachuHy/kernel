#pragma once

#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

auto klog_init(struct limine_framebuffer* fb) -> void;
auto klog_reinit_fb(uint64_t limine_hhdm) -> void;
auto klog_putc(char c) -> void;
auto klog_write(const char* str, size_t len) -> void;
auto klog(const char* str) -> void;
auto klog_hex(uint64_t value) -> void;
auto klog_dec(uint64_t value) -> void;
