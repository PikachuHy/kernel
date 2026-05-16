#pragma once

#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

void klog_init(struct limine_framebuffer* fb);
void klog_reinit_fb(uint64_t limine_hhdm);
void klog_putc(char c);
void klog_write(const char* str, size_t len);
void klog(const char* str);
void klog_hex(uint64_t value);
void klog_dec(uint64_t value);
