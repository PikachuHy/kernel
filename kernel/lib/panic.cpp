#include "kernel/lib/panic.hpp"
#include "kernel/lib/klog.hpp"

[[noreturn]] auto kpanic(const char* msg) -> void {
    klog("\n\n=== KERNEL PANIC ===\n");
    klog(msg);
    klog("\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}

[[noreturn]] auto kpanic(const char* msg, const char* file, int line) -> void {
    klog("\n\n=== KERNEL PANIC ===\n");
    klog("File: ");
    klog(file);
    klog("\nLine: ");
    klog_dec(line);
    klog("\nMessage: ");
    klog(msg);
    klog("\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}
