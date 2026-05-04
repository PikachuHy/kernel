#include <stddef.h>
#include "kernel/core/mm/slab.hpp"

void* operator new(size_t size) {
    if (size == 0) size = 1;
    void* p = kmalloc(size);
    if (!p) {
        // OOM in kernel: halt
        while (1) asm volatile("cli; hlt");
    }
    return p;
}

void* operator new[](size_t size) {
    return operator new(size);
}

void operator delete(void* ptr) noexcept {
    if (ptr) kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    if (ptr) kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    if (ptr) kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    if (ptr) kfree(ptr);
}
