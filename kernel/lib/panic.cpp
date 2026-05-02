#include "kernel/lib/panic.hpp"
#include "kernel/lib/klog.hpp"

[[noreturn]] void kpanic(const char* msg) {
    klog("\n\n=== KERNEL PANIC ===\n");
    klog(msg);
    klog("\n");
    while (1) {
        asm volatile("cli; hlt");
    }
}

[[noreturn]] void kpanic(const char* msg, const char* file, int line) {
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
