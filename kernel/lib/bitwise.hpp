// kernel/lib/bitwise.hpp
#pragma once
#include <stddef.h>

namespace km {

template <typename T>
auto copy_bytes(T* dst, const T* src, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = src[i];
}

template <typename T>
auto zero_bytes(T* dst, size_t count) noexcept -> void {
    for (size_t i = 0; i < count; i++) dst[i] = T{};
}

} // namespace km
