#pragma once
#include <stdint.h>

// Well-known x86 APIC addresses (QEMU defaults)
constexpr uint64_t LAPIC_BASE_PHYS  = 0xFEE00000;
constexpr uint64_t IOAPIC_BASE_PHYS = 0xFEC00000;

// LAPIC register offsets (all 32-bit)
constexpr uint16_t LAPIC_ID          = 0x020;
constexpr uint16_t LAPIC_VERSION     = 0x030;
constexpr uint16_t LAPIC_TPR         = 0x080;
constexpr uint16_t LAPIC_EOI         = 0x0B0;
constexpr uint16_t LAPIC_SVR         = 0x0F0;
constexpr uint16_t LAPIC_ICR_LO      = 0x300;
constexpr uint16_t LAPIC_ICR_HI      = 0x310;

constexpr uint32_t ICR_INIT     = 0x00004500;
constexpr uint32_t ICR_STARTUP  = 0x00004600;
constexpr uint32_t ICR_ASSERT   = 0x00004000;
constexpr uint32_t ICR_LEVEL    = 0x00008000;

constexpr uint16_t LAPIC_LVT_TIMER   = 0x320;
constexpr uint16_t LAPIC_LVT_LINT0   = 0x350;
constexpr uint16_t LAPIC_LVT_LINT1   = 0x360;
constexpr uint16_t LAPIC_LVT_ERROR   = 0x370;
constexpr uint16_t LAPIC_TIMER_INIT  = 0x380;
constexpr uint16_t LAPIC_TIMER_CURR  = 0x390;
constexpr uint16_t LAPIC_TIMER_DIV   = 0x3E0;

constexpr uint32_t LAPIC_SVR_ENABLE = 0x1FF;  // spurious vector 0xFF, APIC enabled

// IOAPIC registers (accessed via IOREGSEL + IOWIN)
constexpr uint16_t IOAPIC_IOREGSEL = 0x00;
constexpr uint16_t IOAPIC_IOWIN    = 0x10;
constexpr uint8_t  IOAPIC_ID       = 0x00;
constexpr uint8_t  IOAPIC_VER      = 0x01;
constexpr uint8_t  IOAPIC_REDTBL   = 0x10;

// Legacy PIC ports
constexpr uint16_t PIC1_CMD  = 0x20;
constexpr uint16_t PIC1_DATA = 0x21;
constexpr uint16_t PIC2_CMD  = 0xA0;
constexpr uint16_t PIC2_DATA = 0xA1;

// IRQ->vector mapping: ISA IRQs start at vector 32
constexpr uint8_t IRQ_BASE_VECTOR = 32;

void lapic_init(uint64_t hhdm);
void lapic_eoi();
uint32_t lapic_read(uint16_t offset);
void lapic_write(uint16_t offset, uint32_t value);
void lapic_send_ipi(uint32_t lapic_id, uint32_t icr_lo);
void lapic_send_init(uint32_t lapic_id);
void lapic_send_sipi(uint32_t lapic_id, uint8_t vector);
bool apic_is_ready();

void ioapic_init(uint64_t hhdm);
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id);
void pic_disable();
