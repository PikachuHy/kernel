// Host-compatible IRQ dispatch — recompiles the kernel's irq.cpp logic
// but with stubbed dependencies. Used for host-side unit testing.

// Stub out kernel headers
#define KLOG_HPP_STUBBED
#define APIC_HPP_STUBBED

inline void klog(const char*) {}
inline void klog_hex(unsigned long long) {}
inline bool apic_is_ready() { return false; }
inline void lapic_eoi() {}

#include <stdint.h>

// Now include the real irq header and then replicate the cpp logic
#include "kernel/arch/x86_64/irq.hpp"

namespace {
constexpr int MAX_IRQ = 16;
constexpr int MAX_PER_IRQ = 4;
struct IrqLine { irq_handler_t h[MAX_PER_IRQ]; int count; };
IrqLine g_irqs[MAX_IRQ];
}

void irq_init() {
    for (auto& irq : g_irqs) { irq.count = 0; for (auto& h : irq.h) h = nullptr; }
}

int irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= MAX_IRQ || !handler) return -1;
    auto& line = g_irqs[irq];
    if (line.count >= MAX_PER_IRQ) return -2;
    line.h[line.count++] = handler;
    return 0;
}

void irq_dispatch(uint8_t vector) {
    if (vector < 32 || vector >= 48) return;
    uint8_t irq = vector - 32;
    if (irq >= MAX_IRQ) return;
    for (int i = 0; i < g_irqs[irq].count; i++) {
        if (g_irqs[irq].h[i]) g_irqs[irq].h[i](vector);
    }
    if (apic_is_ready()) lapic_eoi();
}
