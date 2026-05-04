#pragma once
#include <stdint.h>

namespace x86 {

/// MMIO read/write helpers — phys address + HHDM offset
inline uint32_t mmio_read32(uint64_t hhdm, uint64_t phys) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(hhdm + phys);
    return *ptr;
}

inline void mmio_write32(uint64_t hhdm, uint64_t phys, uint32_t value) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(hhdm + phys);
    *ptr = value;
}

/// I/O port helpers
inline void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline void outw(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline uint16_t inw(uint16_t port) {
    uint16_t result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline void outl(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline uint32_t inl(uint16_t port) {
    uint32_t result;
    asm volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

} // namespace x86
