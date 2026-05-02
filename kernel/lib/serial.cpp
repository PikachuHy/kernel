#include "kernel/lib/serial.hpp"
#include <stdint.h>

namespace {

constexpr uint16_t COM1 = 0x3F8;

void outb(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

} // namespace

void serial_init() {
    outb(COM1 + 1, 0x00); // disable interrupts
    outb(COM1 + 3, 0x80); // enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x01); // divisor low  (115200 / 1)
    outb(COM1 + 1, 0x00); // divisor high (115200 / 0)
    outb(COM1 + 3, 0x03); // 8 bits, no parity, 1 stop bit
    outb(COM1 + 2, 0xC7); // enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B); // IRQs enabled, RTS/DSR set
}

void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
        asm volatile("pause");
    }
    outb(COM1, c);
}
