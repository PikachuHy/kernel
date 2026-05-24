#include "kernel/arch/x86_64/timer.hpp"
#include "kernel/arch/x86_64/apic.hpp"
#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/irq.hpp"
#include "kernel/lib/klog.hpp"

namespace {

constexpr uint16_t PIT_CMD = 0x43;
constexpr uint8_t  TIMER_VEC = 32;
constexpr uint32_t PIT_HZ = 1193182;
constexpr uint32_t CAL_MS = 10;
constexpr uint8_t  LAPIC_DIV = 0b1011;  // divide by 1

uint64_t g_ticks_per_ms = 0;
uint64_t g_uptime_ms = 0;
timer_callback_t g_oneshot_cb = nullptr;
timer_callback_t g_periodic_cb = nullptr;

auto lapic_timer_unmask(uint8_t vector, bool periodic) -> void {
    uint32_t lvt = vector;
    if (periodic) lvt |= (1 << 17);  // periodic mode
    lapic_write(LAPIC_LVT_TIMER, lvt);
}

auto timer_handler(uint8_t) -> bool {
    g_uptime_ms++;
    if (g_oneshot_cb) {
        auto cb = g_oneshot_cb; g_oneshot_cb = nullptr;
        if (!cb(g_uptime_ms)) lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    }
    if (g_periodic_cb) {
        if (!g_periodic_cb(g_uptime_ms)) {
            g_periodic_cb = nullptr;
            lapic_write(LAPIC_LVT_TIMER, 1 << 16);
        }
    }
    return true;
}

} // namespace

auto timer_init(uint64_t hhdm) -> void {
    (void)hhdm;
    g_uptime_ms = 0;
    g_ticks_per_ms = 0;

    // Calibrate LAPIC timer against PIT channel 2
    // PIT channel 2 output is visible on port 0x61 bit 5

    // Start counting from max
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    // Enable PIT channel 2 gate (port 0x61 bit 0)
    uint8_t ppi = x86::inb(0x61);
    x86::outb(0x61, (ppi & ~1) | 1);  // set gate high

    // PIT channel 2, mode 0 (one-shot), lobyte/hibyte, binary
    uint16_t reload = (PIT_HZ * CAL_MS) / 1000;
    x86::outb(PIT_CMD, 0xB0);
    x86::outb(0x42, reload & 0xFF);
    x86::outb(0x42, (reload >> 8) & 0xFF);

    // Wait for channel 2 OUT (bit 5 of port 0x61) to go high
    while ((x86::inb(0x61) & 0x20) == 0) x86::pause();

    uint32_t remaining = lapic_read(LAPIC_TIMER_CURR);
    uint32_t elapsed = 0xFFFFFFFF - remaining;
    g_ticks_per_ms = elapsed / CAL_MS;
    if (g_ticks_per_ms == 0) g_ticks_per_ms = 1;

    klog("Timer: ");
    klog_hex(g_ticks_per_ms);
    klog(" ticks/ms (elapsed ");
    klog_hex(elapsed);
    klog(" over ");
    klog_hex(CAL_MS);
    klog("ms)\n");

    // Mask timer until used
    lapic_write(LAPIC_LVT_TIMER, 1 << 16);
    irq_register(0, timer_handler);
    klog("Timer: hardware init complete\n");
}

auto timer_oneshot(uint64_t delay_us, timer_callback_t cb) -> void {
    if (!g_ticks_per_ms || !cb) return;
    g_oneshot_cb = cb;
    uint64_t ticks = (delay_us * g_ticks_per_ms) / 1000;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_write(LAPIC_TIMER_INIT, static_cast<uint32_t>(ticks));
    lapic_timer_unmask(TIMER_VEC, false);
}

auto timer_periodic(uint64_t interval_us, timer_callback_t cb) -> void {
    if (!g_ticks_per_ms || !cb) return;
    g_periodic_cb = cb;
    uint64_t ticks = (interval_us * g_ticks_per_ms) / 1000;
    if (ticks == 0) ticks = 1;
    if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;
    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV);
    lapic_timer_unmask(TIMER_VEC, true);
    lapic_write(LAPIC_TIMER_INIT, static_cast<uint32_t>(ticks));
}

auto timer_uptime_ms() -> uint64_t { return g_uptime_ms; }
auto timer_ticks_per_ms() -> uint64_t { return g_ticks_per_ms; }
