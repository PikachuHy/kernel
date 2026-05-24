// kernel/fs/fat32/fat32.cpp
// FAT32 filesystem server — ring-3 process at 0x800000
// Opens /dev/ahci0 to read raw sectors, parses MBR/BPB/FAT/directory.

#include "kernel/lib/user_types.hpp"

// ── Syscall numbers ──────────────────────────────────────────────
constexpr int SYS_HANDLE_CLOSE    = 1;
constexpr int SYS_CHANNEL_WRITE   = 11;
constexpr int SYS_CHANNEL_READ    = 12;
constexpr int SYS_OPEN            = 50;
constexpr int SYS_PROCESS_EXIT    = 31;
constexpr uint32_t O_RDONLY = 1 << 0;

// ── Protocol types ───────────────────────────────────────────────
struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op       op;
    uint32_t flags;
    uint64_t offset;
    uint64_t length;
};
struct FileResponse { int32_t result; uint64_t size; };
struct Stat { uint64_t size; uint32_t type; uint32_t padding; };
struct OpenPayload { char path[256]; uint32_t file_handle; uint32_t flags; };

// ── Syscall wrappers ─────────────────────────────────────────────
static auto syscall6(uint64_t num, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5) -> uint64_t {
    uint64_t ret;
    asm volatile(
        "movq %1, %%rax\n" "movq %2, %%rdi\n" "movq %3, %%rsi\n"
        "movq %4, %%rdx\n" "movq %5, %%r10\n" "movq %6, %%r8\n"
        "syscall\n" "movq %%rax, %0\n"
        : "=r"(ret) : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory");
    return ret;
}
// Static WA struct — avoids stack corruption in deep call chains.
static struct { const void* d; size_t sz; const uint32_t* hnd; size_t n; } s_wa;
static auto ch_write(uint32_t h, const void* d, size_t n) -> int {
    s_wa.d = d; s_wa.sz = n; s_wa.hnd = nullptr; s_wa.n = 0;
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&s_wa, 0, 0, 0);
}
static auto ch_read(uint32_t h, void* b, size_t sz) -> int {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)b, sz, 0, 0);
}
static auto ch_close(uint32_t h) -> void { syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0); }

// ── FAT32 structures ──────────────────────────────────────────────
struct __attribute__((packed)) Bpb {
    uint8_t  jmp[3]; char oem[8]; uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster; uint16_t reserved_sectors;
    uint8_t  num_fats; uint16_t root_entries, total_sectors_16;
    uint8_t  media; uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track, num_heads;
    uint32_t hidden_sectors, total_sectors_32;
    uint32_t sectors_per_fat_32; uint16_t flags, version;
    uint32_t root_cluster; uint16_t fsinfo_sector, backup_boot_sector;
    uint8_t  reserved[12]; uint8_t drive_number, reserved1;
    uint8_t  boot_signature; uint32_t volume_id;
    char     volume_label[11], fs_type[8];
};
struct __attribute__((packed)) DirEntry {
    char name[8], ext[3]; uint8_t attr, reserved;
    uint8_t create_tenth; uint16_t create_time, create_date;
    uint16_t access_date, cluster_high;
    uint16_t modify_time, modify_date, cluster_low;
    uint32_t file_size;
};
struct __attribute__((packed)) LfnEntry {
    uint8_t ord; uint16_t chars1[5]; uint8_t attr, type, checksum;
    uint16_t chars2[6], zero, chars3[2];
};

// ── Server state ──────────────────────────────────────────────────
static uint32_t s_bdev = 0;
static uint64_t s_part_base = 0;
static uint32_t s_bps = 512, s_spc = 1, s_rsvd = 0, s_spf = 0, s_root = 0;
static uint64_t s_fat_lba = 0, s_data_lba = 0;

// ── Sector I/O (one sector at a time, small buffers) ─────────────
static auto read_sectors(uint64_t lba, uint8_t* buf, uint32_t count) -> int {
    uint64_t dlba = lba + s_part_base;
    for (uint32_t i = 0; i < count; i++) {
        FileMsg msg = {FileMsg::Read, 0, dlba + i, 1};
        if (ch_write(s_bdev, &msg, sizeof(msg)) != 0) return -1;
        uint8_t resp[sizeof(FileResponse) + 512];
        int rc = ch_read(s_bdev, resp, sizeof(resp));
        if (rc < 0 || (size_t)rc < sizeof(FileResponse)) return -1;
        FileResponse* r = (FileResponse*)resp;
        if (r->result != 0) return -1;
        for (size_t j = 0; j < 512; j++) buf[i * 512 + j] = resp[sizeof(FileResponse) + j];
    }
    return 0;
}

// ── FAT access ────────────────────────────────────────────────────
static auto read_fat_entry(uint32_t cluster) -> uint32_t {
    uint64_t off = (uint64_t)cluster * 4;
    uint64_t lba = s_fat_lba + off / s_bps;
    uint32_t boff = off % s_bps;
    uint8_t sec[512];
    if (read_sectors(lba, sec, 1) != 0) return 0x0FFFFFFF;
    return (sec[boff] | ((uint32_t)sec[boff+1]<<8) |
            ((uint32_t)sec[boff+2]<<16) | ((uint32_t)sec[boff+3]<<24)) & 0x0FFFFFFF;
}

static auto cluster_to_lba(uint32_t cluster) -> uint64_t {
    return s_data_lba + (uint64_t)(cluster - 2) * s_spc;
}

// Walk FAT to find the Nth cluster (0-based). Returns 0 if out of range.
static auto walk_fat(uint32_t start, uint32_t n) -> uint32_t {
    uint32_t c = start;
    for (uint32_t i = 0; i < n && c >= 2 && c < 0x0FFFFFF8; i++)
        c = read_fat_entry(c);
    return (c >= 2 && c < 0x0FFFFFF8) ? c : 0;
}

// ── Directory search ──────────────────────────────────────────────
static auto name_match(const char* a, const char* b) -> bool {
    int i = 0;
    while (b[i]) { char ca = a[i], cb = b[i]; if (ca>='a'&&ca<='z') ca-=32; if (cb>='a'&&cb<='z') cb-=32; if (ca!=cb) return false; i++; }
    return a[i] == '\0';
}

// Search directory starting at cluster for name. Returns first cluster (0=not found).
static auto dir_search(uint32_t dir_cluster, const char* name,
                        uint32_t* out_size, bool* out_is_dir) -> uint32_t {
    char lfn[256];
    int lfn_len = 0;
    // Walk cluster chain of the directory
    uint32_t cl = dir_cluster;
    while (cl >= 2 && cl < 0x0FFFFFF8) {
        for (uint32_t si = 0; si < s_spc; si++) {
            uint8_t sec[512];
            if (read_sectors(cluster_to_lba(cl) + si, sec, 1) != 0) continue;
            for (int e = 0; e < 16; e++) {
                uint8_t* en = sec + e * 32;
                if (en[0] == 0x00) { lfn_len = 0; continue; }
                if (en[0] == 0xE5) continue;
                uint8_t attr = en[11];
                if (attr == 0x0F) {
                    LfnEntry* le = (LfnEntry*)en;
                    int ord = le->ord & 0x3F;
                    if (ord < 1 || ord > 20) continue;
                    if (le->ord & 0x40) lfn_len = 0;
                    int base = (ord - 1) * 13;
                    for (int c = 0; c < 5 && base+c < 255; c++) lfn[base+c] = (char)le->chars1[c];
                    for (int c = 0; c < 6 && base+5+c < 255; c++) lfn[base+5+c] = (char)le->chars2[c];
                    for (int c = 0; c < 2 && base+11+c < 255; c++) lfn[base+11+c] = (char)le->chars3[c];
                    if (ord <= lfn_len || lfn_len == 0) lfn_len = ord;
                    if (le->ord & 0x40) lfn[ord * 13 < 256 ? ord * 13 : 255] = '\0';
                } else {
                    DirEntry* de = (DirEntry*)en;
                    if (de->attr & 0x08) { lfn_len = 0; continue; }
                    char resolved[256];
                    if (lfn_len > 0) {
                        int i = 0; while (i < 255 && lfn[i]) { resolved[i] = lfn[i]; i++; }
                        resolved[i] = '\0';
                    } else {
                        int ri = 0;
                        for (int i = 0; i < 8 && de->name[i] != ' '; i++) resolved[ri++] = de->name[i];
                        if (de->ext[0] != ' ') { resolved[ri++] = '.'; for (int i = 0; i < 3 && de->ext[i] != ' '; i++) resolved[ri++] = de->ext[i]; }
                        resolved[ri] = '\0';
                    }
                    lfn_len = 0;
                    if (name_match(resolved, name)) {
                        uint32_t fc = de->cluster_low | ((uint32_t)de->cluster_high << 16);
                        *out_size = de->file_size;
                        *out_is_dir = (de->attr & 0x10) != 0;
                        return fc;
                    }
                }
            }
        }
        cl = read_fat_entry(cl);
    }
    return 0;
}

// ── Path resolution ───────────────────────────────────────────────
static auto fat32_open(const char* path, uint32_t* out_size, bool* out_is_dir) -> uint32_t {
    uint32_t dir = s_root;
    if (path[0] == '/') path++;
    // Empty path means root directory
    if (path[0] == '\0') {
        // Root directory has size=0 and is_dir=true
        *out_size = 0; *out_is_dir = true;
        return s_root;
    }
    while (*path) {
        char comp[256]; int ci = 0;
        while (*path && *path != '/') { if (ci < 255) comp[ci++] = *path; path++; }
        comp[ci] = '\0';
        if (*path == '/') path++;
        uint32_t sz; bool isd;
        uint32_t child = dir_search(dir, comp, &sz, &isd);
        if (!child) return 0;
        if (*path == '\0') { *out_size = sz; *out_is_dir = isd; return child; }
        if (!isd) return 0;
        dir = child;
    }
    return 0;
}

// ── File handle ───────────────────────────────────────────────────
struct OpenFile { uint32_t chan, start_cluster; uint64_t size; bool is_dir; };
struct Dirent { char name[256]; uint32_t type; uint64_t size; };
static OpenFile s_of;

// List directory entries starting at given cluster.
// Returns count of entries read, or -1 on error.
static auto list_entries(uint32_t dir_cl, uint32_t cookie, Dirent* out, uint32_t max) -> int {
    char lfn[256]; int lfn_len = 0;
    uint32_t cl = dir_cl;
    uint32_t found = 0;

    while (cl >= 2 && cl < 0x0FFFFFF8) {
        for (uint32_t si = 0; si < s_spc; si++) {
            uint8_t sec[512];
            if (read_sectors(cluster_to_lba(cl) + si, sec, 1) != 0) continue;
            for (int e = 0; e < 16; e++) {
                uint8_t* en = sec + e * 32;
                if (en[0] == 0x00) { lfn_len = 0; continue; }
                if (en[0] == 0xE5) continue;
                if (en[11] == 0x0F) {
                    LfnEntry* le = (LfnEntry*)en;
                    int ord = le->ord & 0x3F;
                    if (ord >= 1 && ord <= 20) {
                        if (le->ord & 0x40) lfn_len = 0;
                        int base = (ord - 1) * 13;
                        for (int c = 0; c < 5 && base+c < 255; c++) lfn[base+c] = (char)le->chars1[c];
                        for (int c = 0; c < 6 && base+5+c < 255; c++) lfn[base+5+c] = (char)le->chars2[c];
                        for (int c = 0; c < 2 && base+11+c < 255; c++) lfn[base+11+c] = (char)le->chars3[c];
                        if (ord <= lfn_len || lfn_len == 0) lfn_len = ord;
                        if (le->ord & 0x40) lfn[ord * 13 < 256 ? ord * 13 : 255] = '\0';
                    }
                    continue;
                }
                if (en[11] & 0x08) { lfn_len = 0; continue; }
                DirEntry* de = (DirEntry*)en;
                char resolved[256];
                if (lfn_len > 0) {
                    int i = 0; while (i < 255 && lfn[i]) { resolved[i] = lfn[i]; i++; }
                    resolved[i] = '\0';
                } else {
                    int ri = 0;
                    for (int i = 0; i < 8 && de->name[i] != ' '; i++) resolved[ri++] = de->name[i];
                    if (de->ext[0] != ' ') { resolved[ri++] = '.';
                        for (int i = 0; i < 3 && de->ext[i] != ' '; i++) resolved[ri++] = de->ext[i]; }
                    resolved[ri] = '\0';
                }
                lfn_len = 0;
                if (cookie > 0) { cookie--; continue; }
                if (found >= max) return (int)found;
                int ni = 0; while (resolved[ni] && ni < 255) { out[found].name[ni] = resolved[ni]; ni++; }
                out[found].name[ni] = '\0';
                out[found].type = (de->attr & 0x10) ? 1u : 0u;
                out[found].size = de->file_size;
                found++;
            }
        }
        cl = read_fat_entry(cl);
    }
    return (int)found;
}

static auto handle_file(uint32_t ch, uint32_t start, uint32_t size, bool is_dir) -> void {
    s_of.chan = ch; s_of.start_cluster = start; s_of.size = size; s_of.is_dir = is_dir;

    uint8_t rbuf[sizeof(FileMsg) + 64];
    while (true) {
        int rc = ch_read(ch, rbuf, sizeof(FileMsg));
        if (rc < 0) break;
        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = (FileMsg*)rbuf;
        FileResponse resp = {};

        switch (msg->op) {
        case FileMsg::Read: {
            uint64_t off = msg->offset, len = msg->length;
            if (len > 512) len = 512;
            if (off >= s_of.size) len = 0;
            else if (off + len > s_of.size) len = s_of.size - off;

            uint8_t out[sizeof(FileResponse) + 512];
            *(FileResponse*)out = {0, (uint64_t)len};

            if (len > 0) {
                uint32_t csize = s_spc * s_bps;
                uint32_t idx = off / csize, coff = off % csize;
                uint32_t cl = walk_fat(start, idx);
                if (cl >= 2) {
                    uint64_t lba = cluster_to_lba(cl);
                    uint32_t so = coff / s_bps, bo = coff % s_bps;
                    uint8_t sec[512];
                    if (read_sectors(lba + so, sec, 1) == 0)
                        for (uint64_t i = 0; i < len; i++) out[sizeof(FileResponse)+i] = sec[bo+i];
                    else *(FileResponse*)out = {-1, 0};
                } else *(FileResponse*)out = {-1, 0};
            }
            ch_write(ch, out, sizeof(FileResponse) + len);
            break;
        }
        case FileMsg::Stat: {
            Stat st = {s_of.size, s_of.is_dir ? 1u : 0u, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            ch_write(ch, out, sizeof(out));
            break;
        }
        case FileMsg::Readdir: {
            if (!s_of.is_dir) { resp.result = -1; ch_write(ch, &resp, sizeof(resp)); break; }
            uint32_t cookie = (uint32_t)msg->offset;
            Dirent dirs[16];
            int n = list_entries(s_of.start_cluster, cookie, dirs, 16);
            if (n < 0) { resp.result = -1; ch_write(ch, &resp, sizeof(resp)); break; }
            uint32_t data_sz = (uint32_t)n * sizeof(Dirent);
            uint8_t out[sizeof(FileResponse) + 16 * sizeof(Dirent)];
            *(FileResponse*)out = {0, data_sz};
            uint8_t* dout = out + sizeof(FileResponse);
            for (uint32_t i = 0; i < (uint32_t)n; i++) {
                uint8_t* src = (uint8_t*)&dirs[i];
                for (size_t j = 0; j < sizeof(Dirent); j++)
                    dout[i * sizeof(Dirent) + j] = src[j];
            }
            ch_write(ch, out, sizeof(FileResponse) + data_sz);
            break;
        }
        case FileMsg::Close: ch_close(ch); return;
        default: resp.result = -1; ch_write(ch, &resp, sizeof(resp)); break;
        }
    }
}

// ── Entry point ───────────────────────────────────────────────────
extern "C" void _start() {
    const uint32_t MC = 1;

    uint32_t h = (uint32_t)syscall6(SYS_OPEN, (uint64_t)"/dev/ahci0", O_RDONLY, 0, 0, 0);
    if (h == 0 || h == 0xFFFFFFFF) { goto exit; }
    s_bdev = h;

    // MBR → find partition
    {
        uint8_t mbr[512];
        s_part_base = 0;
        if (read_sectors(0, mbr, 1) != 0) { goto exit; }
        s_part_base = 0;
        for (int e = 0; e < 4; e++) {
            uint8_t* en = mbr + 446 + e * 16;
            if (en[4] == 0x0B || en[4] == 0x0C) {
                s_part_base = en[8] | ((uint64_t)en[9]<<8) | ((uint64_t)en[10]<<16) | ((uint64_t)en[11]<<24);
                break;
            }
        }
        if (s_part_base == 0) { s_part_base = 63; }
    }

    // BPB
    uint8_t bpb_sec[512];
    if (read_sectors(0, bpb_sec, 1) != 0) { goto exit; }
    {
        Bpb* b = (Bpb*)bpb_sec;
        s_bps = b->bytes_per_sector; s_spc = b->sectors_per_cluster;
        s_rsvd = b->reserved_sectors; s_spf = b->sectors_per_fat_32;
        s_root = b->root_cluster;
        s_fat_lba = s_rsvd;
        s_data_lba = s_fat_lba + (uint64_t)b->num_fats * s_spf;
    }

    // Mount-channel event loop
    while (true) {
        uint8_t buf[264];
        int rc = ch_read(MC, buf, sizeof(buf));
        if (rc < 0) break;
        OpenPayload* p = (OpenPayload*)buf;

        uint32_t sz; bool isd;
        uint32_t cl = fat32_open(p->path, &sz, &isd);
        if (!cl) {
            FileResponse err = {-1, 0};
            ch_write(p->file_handle, &err, sizeof(err));
            ch_close(p->file_handle);
            continue;
        }
        FileResponse ok = {0, sz};
        ch_write(p->file_handle, &ok, sizeof(ok));
        handle_file(p->file_handle, cl, sz, isd);
    }

exit:
    ch_close(s_bdev);
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
