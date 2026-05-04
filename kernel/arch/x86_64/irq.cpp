#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/lib/klog.hpp"

namespace {
constexpr int MAX_IRQ = 16;
constexpr int MAX_PER_IRQ = 4;

struct IrqLine { irq_handler_t h[MAX_PER_IRQ]; int count; };
IrqLine g_irqs[MAX_IRQ];
} // namespace

void irq_init() {
    for (auto& irq : g_irqs) { irq.count = 0; for (auto& h : irq.h) h = nullptr; }
    klog("IRQ: table initialized\n");
}

int irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= MAX_IRQ || !handler) return -1;
    auto& line = g_irqs[irq];
    if (line.count >= MAX_PER_IRQ) return -2;
    line.h[line.count++] = handler;
    return 0;
}

extern "C" void irq_dispatch(uint8_t vector) {
    if (vector < 32 || vector >= 48) return;
    uint8_t irq = vector - 32;
    if (irq >= MAX_IRQ) return;
    for (int i = 0; i < g_irqs[irq].count; i++) {
        if (g_irqs[irq].h[i]) g_irqs[irq].h[i](vector);
    }
    if (apic_is_ready()) lapic_eoi();
}
