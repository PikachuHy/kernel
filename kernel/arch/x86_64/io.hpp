#pragma once
#include <stdint.h>

namespace x86 {

/// MMIO read/write helpers — phys address + HHDM offset
inline auto mmio_read32(uint64_t hhdm, uint64_t phys) noexcept -> uint32_t {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(hhdm + phys);
    return *ptr;
}

inline auto mmio_write32(uint64_t hhdm, uint64_t phys, uint32_t value) noexcept -> void {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(hhdm + phys);
    *ptr = value;
}

/// I/O port helpers
inline auto outb(uint16_t port, uint8_t value) noexcept -> void {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline auto inb(uint16_t port) noexcept -> uint8_t {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline auto outw(uint16_t port, uint16_t value) noexcept -> void {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

inline auto inw(uint16_t port) noexcept -> uint16_t {
    uint16_t result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline auto outl(uint16_t port, uint32_t value) noexcept -> void {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline auto inl(uint16_t port) noexcept -> uint32_t {
    uint32_t result;
    asm volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

inline auto pause() noexcept -> void { asm volatile("pause"); }

} // namespace x86
