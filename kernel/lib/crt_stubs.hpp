#pragma once
#include <stddef.h>

extern "C" {

auto memcpy(void* dst, const void* src, size_t n) -> void*;
auto memmove(void* dst, const void* src, size_t n) -> void*;
auto memset(void* dst, int c, size_t n) -> void*;
auto memcmp(const void* a, const void* b, size_t n) -> int;

}
