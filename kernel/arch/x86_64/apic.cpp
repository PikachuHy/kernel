#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/klog.hpp"

namespace {
uint64_t g_lapic_phys  = LAPIC_BASE_PHYS;
uint64_t g_ioapic_phys = IOAPIC_BASE_PHYS;
bool     g_apic_ready = false;

void ioapic_write(uint8_t reg, uint32_t value) {
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOREGSEL, reg);
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOWIN, value);
}
uint32_t ioapic_read(uint8_t reg) {
    x86::mmio_write32(g_hhdm, g_ioapic_phys + IOAPIC_IOREGSEL, reg);
    return x86::mmio_read32(g_hhdm, g_ioapic_phys + IOAPIC_IOWIN);
}
} // namespace

// --- LAPIC ------------------------------------------------------

void lapic_init(uint64_t hhdm) {
    g_hhdm = hhdm;
    klog("LAPIC: base="); klog_hex(g_lapic_phys); klog("\n");

    uint32_t ver = lapic_read(LAPIC_VERSION);
    klog("LAPIC: version="); klog_hex(ver); klog("\n");

    // Mask all LVT entries (bit 16 = mask)
    lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    lapic_write(LAPIC_LVT_LINT0, 1 << 16);
    lapic_write(LAPIC_LVT_LINT1, 1 << 16);
    lapic_write(LAPIC_LVT_ERROR, 1 << 16);

    lapic_write(LAPIC_TPR, 0);  // accept all interrupts
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE);
    g_apic_ready = true;
    klog("LAPIC: enabled\n");
}

void lapic_eoi() { lapic_write(LAPIC_EOI, 0); }

bool apic_is_ready() { return g_apic_ready; }

uint32_t lapic_read(uint16_t offset) {
    return x86::mmio_read32(g_hhdm, g_lapic_phys + offset);
}
void lapic_write(uint16_t offset, uint32_t value) {
    x86::mmio_write32(g_hhdm, g_lapic_phys + offset, value);
}

// --- PIC and I/O APIC -------------------------------------------

void pic_disable() {
    // ICW1: init + edge + cascade
    x86::outb(PIC1_CMD, 0x11);
    x86::outb(PIC2_CMD, 0x11);
    // ICW2: vector offsets
    x86::outb(PIC1_DATA, 0x20);
    x86::outb(PIC2_DATA, 0x28);
    // ICW3: cascade identity
    x86::outb(PIC1_DATA, 0x04);
    x86::outb(PIC2_DATA, 0x02);
    // ICW4: x86 mode
    x86::outb(PIC1_DATA, 0x01);
    x86::outb(PIC2_DATA, 0x01);
    // Mask all
    x86::outb(PIC1_DATA, 0xFF);
    x86::outb(PIC2_DATA, 0xFF);
    klog("PIC: disabled\n");
}

void ioapic_init(uint64_t hhdm) {
    g_hhdm = hhdm;

    klog("IOAPIC: base="); klog_hex(g_ioapic_phys); klog("\n");

    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint8_t max_redir = ((ver >> 16) & 0xFF) + 1;
    klog("IOAPIC: max_redir="); klog_hex(max_redir); klog("\n");

    // Mask all entries
    for (uint8_t i = 0; i < max_redir; i++) {
        uint32_t lo = ioapic_read(IOAPIC_REDTBL + 2 * i);
        ioapic_write(IOAPIC_REDTBL + 2 * i, lo | (1 << 16));
    }

    // Route IRQ1 (keyboard) -> vector 33
    // Note: vector 32 is reserved for the LAPIC timer (internal interrupt)
    ioapic_route_irq(1, IRQ_BASE_VECTOR + 1, 0);
    klog("IOAPIC: IRQ1->v33 routed\n");
}

void lapic_send_ipi(uint32_t lapic_id, uint32_t icr_lo) {
    lapic_write(LAPIC_ICR_HI, lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, icr_lo);
    // Wait for delivery with timeout (~10ms)
    for (int i = 0; i < 10000000; i++) {
        if (!(lapic_read(LAPIC_ICR_LO) & (1 << 12))) break;
        asm volatile("pause" ::: "memory");
    }
}

void lapic_send_init(uint32_t lapic_id) {
    lapic_send_ipi(lapic_id, ICR_INIT | ICR_ASSERT | ICR_LEVEL);
    for (int i = 0; i < 10000000; i++) {
        asm volatile("pause" ::: "memory");
    }
    lapic_send_ipi(lapic_id, ICR_INIT | ICR_LEVEL);
    for (int i = 0; i < 10000000; i++) {
        asm volatile("pause" ::: "memory");
    }
}

void lapic_send_sipi(uint32_t lapic_id, uint8_t vector) {
    lapic_send_ipi(lapic_id, ICR_STARTUP | vector);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id) {
    uint8_t lo = IOAPIC_REDTBL + 2 * irq;
    ioapic_write(lo, vector);  // fixed delivery, physical dest, unmasked
    ioapic_write(lo + 1, static_cast<uint32_t>(lapic_id) << 24);
}
