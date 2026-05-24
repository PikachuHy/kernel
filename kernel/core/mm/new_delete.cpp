#include <stddef.h>
#include "kernel/core/mm/slab.hpp"

auto operator new(size_t size) -> void* {
    if (size == 0) size = 1;
    void* p = kmalloc(size);
    if (!p) {
        // OOM in kernel: halt
        while (1) asm volatile("cli; hlt");
    }
    return p;
}

auto operator new[](size_t size) -> void* {
    return operator new(size);
}

auto operator delete(void* ptr) noexcept -> void {
    if (ptr) kfree(ptr);
}

auto operator delete[](void* ptr) noexcept -> void {
    if (ptr) kfree(ptr);
}

auto operator delete(void* ptr, size_t) noexcept -> void {
    if (ptr) kfree(ptr);
}

auto operator delete[](void* ptr, size_t) noexcept -> void {
    if (ptr) kfree(ptr);
}


