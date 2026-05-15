// kernel/init/init.cpp
// User-space init process — first ring-3 program

using uint64_t = unsigned long long;
using int64_t  = signed long long;
using uint32_t = unsigned int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

// Syscall numbers (must match kernel/arch/x86_64/syscall.hpp)
constexpr int SYS_DEBUG_PRINT    = 0;
constexpr int SYS_HANDLE_CLOSE   = 1;
constexpr int SYS_CHANNEL_WRITE  = 11;
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

    // Test 1: Open /dev/console and write a greeting
    {
        uint64_t h = syscall6(SYS_OPEN,
            (uint64_t)"/dev/console", 0x2, 0, 0, 0);  // O_WRONLY
        print("  sys_open(/dev/console): ");
        print_hex(h);
        print("\n");

        if ((int64_t)h >= 0) {
            const char* msg = "Hello from init via VFS!\n";
            struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
            WA wa = { msg, 24, nullptr, 0 };
            syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&wa, 0, 0, 0);
            print("  write: OK\n");
            syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
        }
    }

    // Test 2: Create a file in tmpfs
    {
        uint64_t h = syscall6(SYS_OPEN,
            (uint64_t)"/hello.txt", 0x8, 0, 0, 0);  // O_CREAT
        print("  sys_open(/hello.txt, O_CREAT): ");
        print_hex(h);
        print("\n");

        if ((int64_t)h >= 0) {
            const char* data = "Hello tmpfs!";
            struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
            WA wa = { data, 11, nullptr, 0 };
            syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&wa, 0, 0, 0);
            print("  write: OK\n");
            syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
        }
    }

    print("=== init: done ===\n");
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
