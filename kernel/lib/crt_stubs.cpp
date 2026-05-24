#include "kernel/lib/crt_stubs.hpp"

extern "C" {

auto memcpy(void* dst, const void* src, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

auto memmove(void* dst, const void* src, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

auto memset(void* dst, int c, size_t n) -> void* {
    auto* d = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; i++) d[i] = static_cast<unsigned char>(c);
    return dst;
}

auto memcmp(const void* a, const void* b, size_t n) -> int {
    auto* pa = static_cast<const unsigned char*>(a);
    auto* pb = static_cast<const unsigned char*>(b);
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
    }
    return 0;
}

}
