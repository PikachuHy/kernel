// kernel/lib/scoped_mem.hpp
#pragma once
#include <stddef.h>
#include "kernel/core/mm/slab.hpp"

namespace km {

class ScopedMem {
    void* ptr_ = nullptr;
public:
    explicit ScopedMem(void* p) noexcept : ptr_(p) {}
    ~ScopedMem() { if (ptr_) kfree(ptr_); }

    ScopedMem(ScopedMem&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    auto operator=(ScopedMem&& other) noexcept -> ScopedMem& {
        if (this != &other) { if (ptr_) kfree(ptr_); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }

    ScopedMem(const ScopedMem&) = delete;
    auto operator=(const ScopedMem&) = delete;

    auto release() noexcept -> void* { auto* p = ptr_; ptr_ = nullptr; return p; }
    auto get() const noexcept -> void* { return ptr_; }
};

} // namespace km
