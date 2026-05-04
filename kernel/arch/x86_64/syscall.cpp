#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/lib/klog.hpp"

extern "C" void syscall_entry();

constexpr uint32_t IA32_EFER   = 0xC0000080;
constexpr uint32_t IA32_STAR   = 0xC0000081;
constexpr uint32_t IA32_LSTAR  = 0xC0000082;
constexpr uint32_t IA32_CSTAR  = 0xC0000083;
constexpr uint32_t IA32_SFMASK = 0xC0000084;

namespace {
syscall_handler_t g_handler = nullptr;

extern "C" uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4) {
    if (g_handler) return g_handler(num, a1, a2, a3, a4);
    klog("Syscall #"); klog_hex(num); klog("\n");
    return 0;
}
} // namespace

void syscall_init() {
    uint64_t efer = x86::rdmsr(IA32_EFER);
    x86::wrmsr(IA32_EFER, efer | 1);  // SCE bit

    x86::wrmsr(IA32_STAR, (0x1BULL << 48) | (0x08ULL << 32));

    x86::wrmsr(IA32_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    x86::wrmsr(IA32_CSTAR, 0);
    x86::wrmsr(IA32_SFMASK, (1 << 9) | (1 << 10));

    klog("Syscall: LSTAR=");
    klog_hex(reinterpret_cast<uint64_t>(&syscall_entry));
    klog(" SCE=enabled\n");
}

void syscall_set_handler(syscall_handler_t h) { g_handler = h; }
