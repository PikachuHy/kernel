// kernel/init/init.cpp
// User-space init process — first ring-3 program

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

// Syscall numbers (must match kernel/arch/x86_64/syscall.hpp)
constexpr int SYS_DEBUG_PRINT    = 0;
constexpr int SYS_PROCESS_EXIT   = 31;
constexpr int SYS_OPEN           = 50;

// syscall6(num, a1, a2, a3, a4, a5) → return value in rax
static inline uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    asm volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return ret;
}

static void print(const char* msg) {
    syscall6(SYS_DEBUG_PRINT, reinterpret_cast<uint64_t>(msg), 0, 0, 0, 0);
}

static void print_hex(uint64_t n) {
    char buf[20] = "0x0000000000000000\n";
    for (int i = 17; i > 1; i--) {
        uint8_t d = n & 0xF;
        buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
        n >>= 4;
    }
    print(buf);
}

extern "C" void _start() {
    print("=== init: VFS test ===\n");

    // Test sys_open to /dev/console
    uint64_t h = syscall6(SYS_OPEN,
        (uint64_t)"/dev/console", 0x2, 0, 0, 0);  // O_WRONLY
    print("  sys_open: ");
    print_hex(h);
    print("\n");

    print("=== init: done ===\n");
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
