// kernel/init/shell/shell.cpp
// Simple demo shell — lists files, reads content, exits.

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using int32_t  = int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

constexpr int SYS_DEBUG_PRINT   = 0;
constexpr int SYS_HANDLE_CLOSE  = 1;
constexpr int SYS_CHANNEL_WRITE = 11;
constexpr int SYS_CHANNEL_READ  = 12;
constexpr int SYS_OPEN          = 50;
constexpr int SYS_PROCESS_EXIT  = 31;

constexpr uint32_t O_RDONLY = 1 << 0;
constexpr uint32_t O_WRONLY = 1 << 1;

struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op       op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};
struct FileResponse { int32_t result; uint64_t size; };
struct Stat { uint64_t size; uint32_t type; uint32_t padding; };
struct Dirent { char name[256]; uint32_t type; uint64_t size; };

#define OUTB(val) asm volatile("movw $0x3F8, %%dx; outb %%al, %%dx" : : "a"((uint8_t)(val)))

static uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    asm volatile(
        "movq %1, %%rax\n" "movq %2, %%rdi\n" "movq %3, %%rsi\n"
        "movq %4, %%rdx\n" "movq %5, %%r10\n" "movq %6, %%r8\n"
        "syscall\n" "movq %%rax, %0\n"
        : "=r"(ret) : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory");
    return ret;
}

static void print(const char* m) { syscall6(SYS_DEBUG_PRINT, (uint64_t)m, 0, 0, 0, 0); }
static void print_hex(uint64_t n) {
    char b[20] = "0x0000000000000000\n";
    for (int i = 17; i > 1; i--) { uint8_t d = n & 0xF; b[i] = d < 10 ? '0' + d : 'A' + d - 10; n >>= 4; }
    print(b);
}
__attribute__((unused)) static void print_dec(uint64_t n) {
    if (n == 0) { print("0"); return; }
    char b[21]; int i = 20; b[i--] = '\0';
    while (n > 0 && i >= 0) { b[i--] = '0' + (n % 10); n /= 10; }
    print(&b[i + 1]);
}

static struct { const void* d; size_t sz; const uint32_t* hnd; size_t n; } s_wa;
static int ch_write(uint32_t h, const void* d, size_t n) {
    s_wa.d = d; s_wa.sz = n; s_wa.hnd = nullptr; s_wa.n = 0;
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&s_wa, 0, 0, 0);
}
static int ch_read(uint32_t h, void* b, size_t sz) {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)b, sz, 0, 0);
}
static void ch_close(uint32_t h) { syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0); }

// Read a file and print first `len` bytes
static void read_file(uint32_t fh, uint32_t len) {
    FileMsg msg = {FileMsg::Read, 0, 0, len};
    if (ch_write(fh, &msg, sizeof(msg)) != 0) { print("    ch_write fail\n"); return; }
    uint8_t rbuf[sizeof(FileResponse) + 64];
    int rc = ch_read(fh, rbuf, sizeof(rbuf));
    if (rc < (int)sizeof(FileResponse)) { print("    ch_read fail\n"); return; }
    FileResponse* resp = (FileResponse*)rbuf;
    if (resp->result != 0) { print("    read error\n"); return; }
    for (uint64_t i = 0; i < resp->size && i < 64; i++) {
        uint8_t byte = rbuf[sizeof(FileResponse) + i];
        if (byte >= 32 && byte < 127) { char c[2] = {(char)byte, 0}; print(c); }
        else { print("."); }
    }
    print("\n");
}

static void list_dir(uint32_t fh) {
    FileMsg msg = {FileMsg::Readdir, 0, 0, 16};
    if (ch_write(fh, &msg, sizeof(msg)) != 0) { print("    readdir fail\n"); return; }
    uint8_t rbuf[sizeof(FileResponse) + 16 * sizeof(Dirent)];
    int rc = ch_read(fh, rbuf, sizeof(rbuf));
    if (rc < (int)sizeof(FileResponse)) return;
    FileResponse* resp = (FileResponse*)rbuf;
    if (resp->result != 0) return;
    uint32_t count = resp->size / sizeof(Dirent);
    Dirent* dirs = (Dirent*)(rbuf + sizeof(FileResponse));
    for (uint32_t i = 0; i < count; i++) {
        print("  "); print(dirs[i].type ? "[DIR ]" : "[FILE]"); print(" ");
        print(dirs[i].name);
        for (int s = 0; dirs[i].name[s]; s++) {}
        // Right-align size numbers
        uint64_t sz = dirs[i].size;
        char szb[16]; int si = 0;
        if (sz == 0) { szb[0] = '0'; szb[1] = '\0'; }
        else { int ti = 15; szb[ti--] = '\0';
            do { szb[ti--] = '0' + (sz % 10); sz /= 10; } while (sz > 0); si = ti + 1; }
        int spaces = 20 - (int)si; for (int s = 0; s < spaces; s++) print(" ");
        print("("); print(&szb[si]); print(" bytes)\n");
    }
}

extern "C" void _start() {
    print("\n--- Shell Demo ---\n");

    // Open console for output
    uint32_t con = (uint32_t)syscall6(SYS_OPEN, (uint64_t)"/dev/console", O_WRONLY, 0, 0, 0);
    print("Console handle: "); print_hex(con); print("\n");

    // Open root directory and list
    uint32_t root = (uint32_t)syscall6(SYS_OPEN, (uint64_t)"/", O_RDONLY, 0, 0, 0);
    print("Root handle: "); print_hex(root); print("\n");

    if (root != 0 && root != 0xFFFFFFFF) {
        print("Root directory:\n");
        list_dir(root);

        // Stat root
        FileMsg sm = {FileMsg::Stat, 0, 0, 0};
        if (ch_write(root, &sm, sizeof(sm)) == 0) {
            uint8_t sb[sizeof(FileResponse) + sizeof(Stat)];
            if (ch_read(root, sb, sizeof(sb)) >= (int)sizeof(FileResponse)) {
                FileResponse* sr = (FileResponse*)sb;
                if (sr->result == 0) {
                    Stat* st = (Stat*)(sb + sizeof(FileResponse));
                    print("  Type: "); print(st->type ? "Directory" : "File"); print("\n");
                }
            }
        }
        ch_close(root);
    }

    // Open /kernel.elf and read ELF magic
    print("\nReading /kernel.elf...\n");
    uint32_t kf = (uint32_t)syscall6(SYS_OPEN, (uint64_t)"/kernel.elf", O_RDONLY, 0, 0, 0);
    if (kf != 0 && kf != 0xFFFFFFFF) {
        read_file(kf, 4);
        ch_close(kf);
    } else { print("  not found\n"); }

    // Open /limine.conf and read header
    print("\nReading /limine.conf...\n");
    uint32_t lf = (uint32_t)syscall6(SYS_OPEN, (uint64_t)"/limine.conf", O_RDONLY, 0, 0, 0);
    if (lf != 0 && lf != 0xFFFFFFFF) {
        read_file(lf, 64);
        ch_close(lf);
    } else { print("  not found\n"); }

    print("\n--- Shell Demo Complete ---\n");

    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
