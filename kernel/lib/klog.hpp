#pragma once

#include <stddef.h>
#include <stdint.h>

struct stivale2_struct;

void klog_init(struct stivale2_struct* info);
void klog_putc(char c);
void klog_write(const char* str, size_t len);
void klog(const char* str);
void klog_hex(uint64_t value);
void klog_dec(uint64_t value);
