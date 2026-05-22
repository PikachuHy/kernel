// kernel/init/init.cpp
// User-space init process — first ring-3 program
// Tests VFS: devfs console, FAT32 file read

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using int32_t  = int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

constexpr int SYS_DEBUG_PRINT    = 0;
constexpr int SYS_PROCESS_EXIT   = 31;
constexpr int SYS_OPEN           = 50;
constexpr int SYS_CHANNEL_WRITE  = 11;
constexpr int SYS_CHANNEL_READ   = 12;

constexpr uint32_t O_WRONLY = 1 << 1;
constexpr uint32_t O_RDONLY = 1 << 0;

struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op       op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};

struct FileResponse {
    int32_t  result;
    uint64_t size;
};

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

__attribute__((unused)) static void print_dec(uint64_t n) {
    if (n == 0) { print("0"); return; }
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    while (n > 0 && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    print(&buf[i + 1]);
}

__attribute__((unused)) static int channel_write(uint32_t h, const void* data, size_t len) {
    struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
    WA a = {data, len, nullptr, 0};
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&a, 0, 0, 0);
}

__attribute__((unused)) static int channel_read(uint32_t h, void* buf, size_t buf_size) {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)buf, buf_size, 0, 0);
}

extern "C" void _start() {
    print("=== init: test ===\n");

    // Test 1: open /dev/console
    uint64_t h = syscall6(SYS_OPEN,
        (uint64_t)"/dev/console", O_WRONLY, 0, 0, 0);
    print("  [1] sys_open('/dev/console'): ");
    print_hex(h);
    print("\n");

    // Test 2: open /kernel.elf via FAT32
    uint64_t fh = syscall6(SYS_OPEN,
        (uint64_t)"/kernel.elf", O_RDONLY, 0, 0, 0);
    print("  [2] sys_open('/kernel.elf'): ");
    print_hex(fh);
    print("\n");

    if (fh != 0 && fh != 0xFFFFFFFF) {
        print("  [3] /kernel.elf opened OK\n");
        print("  [4] reading first 4 bytes...\n");
        FileMsg msg = {FileMsg::Read, 0, 0, 4};
        if (channel_write((uint32_t)fh, &msg, sizeof(msg)) == 0) {
            uint8_t rbuf[sizeof(FileResponse)+4];
            if (channel_read((uint32_t)fh, rbuf, sizeof(rbuf)) >= (int)sizeof(FileResponse)) {
                FileResponse* resp = (FileResponse*)rbuf;
                if (resp->result == 0 && resp->size >= 4) {
                    uint8_t* d = rbuf + sizeof(FileResponse);
                    print("  [5] data: "); print_hex(d[0]); print(" "); print_hex(d[1]);
                    print(" "); print_hex(d[2]); print(" "); print_hex(d[3]);
                    print(d[0]==0x7F&&d[1]=='E'&&d[2]=='L'&&d[3]=='F'?" ELF=YES\n":" ELF=NO\n");
                }
            }
        }
    } else {
        print("  [3] /kernel.elf NOT FOUND\n");
    }

    print("=== init: done ===\n");
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
