// kernel/fs/fat32/fat32.cpp
// FAT32 filesystem server — ring-3 process at 0x800000
// Opens /dev/ahci0 to read raw sectors, parses FAT32 BPB/FAT/directory,
// and presents files as Channels.

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using int32_t  = int;
using uint8_t  = unsigned char;
using uint16_t = unsigned short;
using size_t = decltype(sizeof(0));

// ── Syscall numbers ──────────────────────────────────────────────
constexpr int SYS_DEBUG_PRINT     = 0;
constexpr int SYS_HANDLE_CLOSE    = 1;
constexpr int SYS_CHANNEL_WRITE   = 11;
constexpr int SYS_CHANNEL_READ    = 12;
constexpr int SYS_OPEN            = 50;
constexpr int SYS_PROCESS_EXIT    = 31;

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
struct Dirent { char name[256]; uint32_t type; uint64_t size; };
struct OpenPayload { char path[256]; uint32_t file_handle; uint32_t flags; };

constexpr uint32_t O_RDONLY = 1 << 0;

// ── Syscall wrappers ─────────────────────────────────────────────
static uint64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2,
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

static void debug(const char* msg) {
    syscall6(SYS_DEBUG_PRINT, (uint64_t)msg, 0, 0, 0, 0);
}

static void debug_hex(uint64_t n) {
    char buf[20] = "0x0000000000000000\n";
    for (int i = 17; i > 1; i--) {
        uint8_t d = n & 0xF;
        buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
        n >>= 4;
    }
    debug(buf);
}

static void debug_dec(uint64_t n) {
    if (n == 0) { debug("0"); return; }
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    while (n > 0 && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    debug(&buf[i + 1]);
}

static int channel_write(uint32_t h, const void* data, size_t len) {
    struct WA { const void* d; size_t sz; const uint32_t* hnd; size_t n; };
    WA a = {data, len, nullptr, 0};
    return (int)syscall6(SYS_CHANNEL_WRITE, h, (uint64_t)&a, 0, 0, 0);
}

static int channel_read(uint32_t h, void* buf, size_t buf_size) {
    return (int)syscall6(SYS_CHANNEL_READ, h, (uint64_t)buf, buf_size, 0, 0);
}

static void handle_close(uint32_t h) {
    syscall6(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
}

// ── FAT32 BPB (BIOS Parameter Block) ─────────────────────────────

struct __attribute__((packed)) Bpb {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;        // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t  media;
    uint16_t sectors_per_fat_16;  // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB
    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;      // 0x29
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];          // "FAT32   "
};

// ── Directory entry (8.3 format, 32 bytes) ───────────────────────

struct __attribute__((packed)) DirEntry {
    char     name[8];       // space-padded
    char     ext[3];        // space-padded
    uint8_t  attr;          // 0x10 = directory, 0x0F = LFN
    uint8_t  reserved;
    uint8_t  create_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_low;
    uint32_t file_size;
};

// ── LFN entry (Long File Name) ────────────────────────────────────

struct __attribute__((packed)) LfnEntry {
    uint8_t  ord;           // 1..N with bit 6 set on last
    uint16_t chars1[5];     // UTF-16 characters
    uint8_t  attr;          // 0x0F
    uint8_t  type;          // 0
    uint8_t  checksum;
    uint16_t chars2[6];
    uint16_t zero;
    uint16_t chars3[2];
};

// ── Server state ──────────────────────────────────────────────────

static uint32_t s_bdev = 0;             // block device handle (/dev/ahci0)
static uint64_t s_partition_base = 0;   // LBA offset to FAT32 partition
static uint32_t s_bytes_per_sector = 512;
static uint32_t s_sectors_per_cluster = 1;
static uint32_t s_reserved_sectors = 0;
static uint32_t s_sectors_per_fat = 0;
static uint32_t s_root_cluster = 0;
static uint64_t s_fat_start_lba = 0;
static uint64_t s_data_start_lba = 0;

// ── Sector I/O ────────────────────────────────────────────────────

static int read_sectors(uint64_t lba, uint8_t* buf, uint32_t count) {
    // Offset all reads by the partition base LBA.
    uint64_t disk_lba = lba + s_partition_base;
    for (uint32_t i = 0; i < count; i++) {
        FileMsg msg;
        msg.op = FileMsg::Read;
        msg.flags = 0;
        msg.offset = disk_lba + i;
        msg.length = 1;  // one sector at a time

        int rc = channel_write(s_bdev, &msg, sizeof(FileMsg));
        if (rc != 0) return -1;

        // Response: FileResponse header + 512 bytes of sector data
        uint8_t resp[sizeof(FileResponse) + 512];
        rc = channel_read(s_bdev, resp, sizeof(resp));
        if (rc < 0 || (size_t)rc < sizeof(FileResponse)) return -1;

        FileResponse* r = reinterpret_cast<FileResponse*>(resp);
        if (r->result != 0) return -1;

        for (size_t j = 0; j < 512; j++)
            buf[i * 512 + j] = resp[sizeof(FileResponse) + j];
    }

    return 0;
}

// ── FAT table access ──────────────────────────────────────────────

static uint32_t read_fat_entry(uint32_t cluster) {
    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t lba = s_fat_start_lba + fat_offset / s_bytes_per_sector;
    uint32_t off = fat_offset % s_bytes_per_sector;

    uint8_t sector[512];
    if (read_sectors(lba, sector, 1) != 0) return 0x0FFFFFFF;

    uint32_t val = sector[off] | ((uint32_t)sector[off + 1] << 8) |
                   ((uint32_t)sector[off + 2] << 16) | ((uint32_t)sector[off + 3] << 24);
    return val & 0x0FFFFFFF;
}

// ── Cluster chain traversal ───────────────────────────────────────

struct ClusterChain {
    uint32_t clusters[1024];
    uint32_t count;
};

static void build_cluster_chain(ClusterChain* chain, uint32_t start_cluster) {
    chain->count = 0;
    uint32_t cluster = start_cluster;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && chain->count < 1024) {
        chain->clusters[chain->count++] = cluster;
        cluster = read_fat_entry(cluster);
    }
}

static uint64_t cluster_to_lba(uint32_t cluster) {
    return s_data_start_lba + (uint64_t)(cluster - 2) * s_sectors_per_cluster;
}

__attribute__((unused)) static int read_cluster(uint32_t cluster, uint8_t* buf) {
    uint64_t lba = cluster_to_lba(cluster);
    return read_sectors(lba, buf, s_sectors_per_cluster);
}

// ── Directory reading ─────────────────────────────────────────────

// Match a name extracted from LFN + 8.3 entries against a path component.
// Returns true if the resolved name matches 'target' (case-insensitive).
static bool match_name(const char* resolved, const char* target) {
    int i = 0;
    while (target[i]) {
        char a = resolved[i];
        char b = target[i];
        // ASCII case-insensitive comparison
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return false;
        i++;
    }
    return resolved[i] == '\0';
}

// Walk a directory cluster to find an entry by name.
// Returns the first cluster of the file (0 if not found), and fills size.
static uint32_t find_in_directory(uint32_t dir_cluster,
                                   const char* name,
                                   uint32_t* out_size,
                                   bool* out_is_dir) {
    uint8_t sector[512];
    char lfn_buf[256];
    int  lfn_len = 0;

    ClusterChain chain;
    build_cluster_chain(&chain, dir_cluster);

    for (uint32_t ci = 0; ci < chain.count; ci++) {
        for (uint32_t si = 0; si < s_sectors_per_cluster; si++) {
            uint64_t lba = cluster_to_lba(chain.clusters[ci]) + si;
            if (read_sectors(lba, sector, 1) != 0) continue;

            for (int e = 0; e < 16; e++) {
                uint8_t* entry = sector + e * 32;
                uint8_t first = entry[0];

                if (first == 0x00) { lfn_len = 0; continue; } // end
                if (first == 0xE5) continue; // deleted

                uint8_t attr = entry[11];
                if (attr == 0x0F) {
                    // LFN entry
                    LfnEntry* lfn = reinterpret_cast<LfnEntry*>(entry);
                    int ord = lfn->ord & 0x3F;
                    if (ord < 1 || ord > 20) continue;
                    if (lfn->ord & 0x40) lfn_len = 0; // first LFN entry

                    // Copy UTF-16 chars as ASCII (assuming ASCII filenames)
                    uint16_t* chars = lfn->chars1;
                    for (int c = 0; c < 5 && (ord - 1) * 13 + c < 255; c++) {
                        if (chars[c]) lfn_buf[(ord - 1) * 13 + c] = (char)chars[c];
                        else { lfn_buf[(ord - 1) * 13 + c] = '\0'; break; }
                    }
                    chars = lfn->chars2;
                    for (int c = 0; c < 6 && (ord - 1) * 13 + 5 + c < 255; c++) {
                        if (chars[c]) lfn_buf[(ord - 1) * 13 + 5 + c] = (char)chars[c];
                        else { lfn_buf[(ord - 1) * 13 + 5 + c] = '\0'; break; }
                    }
                    chars = lfn->chars3;
                    for (int c = 0; c < 2 && (ord - 1) * 13 + 11 + c < 255; c++) {
                        if (chars[c]) lfn_buf[(ord - 1) * 13 + 11 + c] = (char)chars[c];
                        else { lfn_buf[(ord - 1) * 13 + 11 + c] = '\0'; break; }
                    }

                    if (ord <= lfn_len || lfn_len == 0) lfn_len = ord;
                    if (lfn->ord & 0x40) {
                        // Last LFN entry — null terminate
                        lfn_buf[ord * 13 < 256 ? ord * 13 : 255] = '\0';
                    }
                } else {
                    // Regular 8.3 entry
                    DirEntry* de = reinterpret_cast<DirEntry*>(entry);
                    if ((de->attr & 0x08) != 0) { lfn_len = 0; continue; } // volume label

                    // Build the name to match
                    char resolved[256];
                    if (lfn_len > 0) {
                        // Use LFN
                        int i = 0;
                        while (i < 255 && lfn_buf[i]) { resolved[i] = lfn_buf[i]; i++; }
                        resolved[i] = '\0';
                    } else {
                        // Use 8.3 name
                        int ri = 0;
                        for (int i = 0; i < 8 && de->name[i] != ' '; i++)
                            resolved[ri++] = de->name[i];
                        if (de->ext[0] != ' ') {
                            resolved[ri++] = '.';
                            for (int i = 0; i < 3 && de->ext[i] != ' '; i++)
                                resolved[ri++] = de->ext[i];
                        }
                        resolved[ri] = '\0';
                    }
                    lfn_len = 0;

                    if (match_name(resolved, name)) {
                        uint32_t cluster = de->cluster_low |
                                          ((uint32_t)de->cluster_high << 16);
                        *out_size = de->file_size;
                        *out_is_dir = (de->attr & 0x10) != 0;
                        return cluster;
                    }
                }
            }
        }
    }

    return 0;
}

// ── File open state ───────────────────────────────────────────────

struct FileState {
    uint32_t      file_chan;    // handle for file I/O (Read/Close)
    ClusterChain  chain;        // clusters for this file
    uint64_t      file_size;
    uint64_t      cursor;
    bool          is_dir;
    FileState*    next;
};

static FileState* s_files = nullptr;

__attribute__((unused)) static FileState* find_file_state(uint32_t file_chan) {
    for (FileState* fs = s_files; fs; fs = fs->next)
        if (fs->file_chan == file_chan) return fs;
    return nullptr;
}

static void add_file_state(FileState* fs) {
    fs->next = s_files;
    s_files = fs;
}

static void remove_file_state(uint32_t file_chan) {
    FileState** prev = &s_files;
    for (FileState* fs = s_files; fs; fs = fs->next) {
        if (fs->file_chan == file_chan) {
            *prev = fs->next;
            return;
        }
        prev = &fs->next;
    }
}

// ── Handle file operations ────────────────────────────────────────

static void handle_file(uint32_t file_chan, uint32_t start_cluster,
                        uint32_t size, bool is_dir) {
    FileState state;
    state.file_chan = file_chan;
    state.file_size = size;
    state.cursor = 0;
    state.is_dir = is_dir;
    state.next = nullptr;
    build_cluster_chain(&state.chain, start_cluster);

    add_file_state(&state);

    // Read-ahead buffer — sized for one sector at a time.
    uint8_t rbuf[sizeof(FileMsg) + 512];

    while (true) {
        int rc = channel_read(file_chan, rbuf, sizeof(FileMsg));
        if (rc < 0) break;

        if ((size_t)rc < sizeof(FileMsg)) continue;
        FileMsg* msg = reinterpret_cast<FileMsg*>(rbuf);

        FileResponse resp = {};
        switch (msg->op) {
        case FileMsg::Read: {
            uint64_t offset = msg->offset;
            uint64_t length = msg->length;
            // Clamp to 512 bytes per read to keep stack usage low
            if (length > 512) length = 512;
            if (offset >= state.file_size) length = 0;
            else if (offset + length > state.file_size)
                length = state.file_size - offset;

            uint8_t out[sizeof(FileResponse) + 512];
            *(FileResponse*)out = {0, length};

            if (length > 0) {
                uint32_t cluster_size = s_sectors_per_cluster * s_bytes_per_sector;
                uint32_t cl_idx = offset / cluster_size;
                uint32_t cl_off = offset % cluster_size;

                if (cl_idx < state.chain.count) {
                    uint32_t cluster = state.chain.clusters[cl_idx];
                    uint64_t lba = cluster_to_lba(cluster);
                    uint32_t sector_off = cl_off / s_bytes_per_sector;
                    uint32_t byte_off = cl_off % s_bytes_per_sector;

                    uint8_t sec[512];
                    int r = read_sectors(lba + sector_off, sec, 1);
                    if (r == 0) {
                        for (uint64_t i = 0; i < length; i++)
                            out[sizeof(FileResponse) + i] = sec[byte_off + i];
                    } else {
                        *(FileResponse*)out = {-1, 0};
                    }
                }
            }

            channel_write(file_chan, out, sizeof(FileResponse) + length);
            break;
        }
        case FileMsg::Stat: {
            Stat st = {state.file_size, state.is_dir ? 1u : 0u, 0};
            uint8_t out[sizeof(FileResponse) + sizeof(Stat)];
            *(FileResponse*)out = {0, sizeof(st)};
            *(Stat*)(out + sizeof(FileResponse)) = st;
            channel_write(file_chan, out, sizeof(out));
            break;
        }
        case FileMsg::Close:
            handle_close(file_chan);
            remove_file_state(file_chan);
            return;
        default:
            resp.result = -1;
            channel_write(file_chan, &resp, sizeof(resp));
            break;
        }
    }

    remove_file_state(file_chan);
}

// ── FAT32 open: resolve path, build cluster chain ─────────────────

static uint32_t fat32_open(const char* path, uint32_t* out_size, bool* out_is_dir) {
    // Start from root directory
    uint32_t dir_cluster = s_root_cluster;

    // Skip leading slash
    if (path[0] == '/') path++;

    while (*path) {
        // Extract next component
        char comp[256];
        int ci = 0;
        while (*path && *path != '/') {
            if (ci < 255) comp[ci++] = *path;
            path++;
        }
        comp[ci] = '\0';
        if (*path == '/') path++;

        uint32_t child_size;
        bool child_is_dir;
        uint32_t child_cluster = find_in_directory(dir_cluster, comp,
                                                    &child_size, &child_is_dir);
        if (child_cluster == 0) return 0;

        if (*path == '\0') {
            // This is the file
            *out_size = child_size;
            *out_is_dir = child_is_dir;
            return child_cluster;
        }

        // Must be a directory to continue
        if (!child_is_dir) return 0;
        dir_cluster = child_cluster;
    }

    return 0; // root directory not supported as a file
}

// ── Entry point ───────────────────────────────────────────────────

extern "C" void _start() {
    const uint32_t MOUNT_CHAN = 1;

    debug("FAT32: starting...\n");

    // Open block device
    uint32_t bdev_handle = (uint32_t)syscall6(SYS_OPEN,
        (uint64_t)"/dev/ahci0", O_RDONLY, 0, 0, 0);
    if (bdev_handle == 0xFFFFFFFF || bdev_handle == 0) {
        debug("FAT32: failed to open /dev/ahci0\n");
        syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
        while (1) { asm volatile("hlt"); }
    }
    s_bdev = bdev_handle;

    // Read MBR (sector 0) to find the FAT32 partition.
    // The FAT32 BPB is at the start of the partition, not at disk LBA 0.
    {
        uint8_t mbr[512];
        // Temporarily set partition_base to 0 to read the raw MBR
        s_partition_base = 0;
        if (read_sectors(0, mbr, 1) != 0) {
            debug("FAT32: failed to read MBR\n");
            handle_close(s_bdev);
            syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
            while (1) { asm volatile("hlt"); }
        }

        // Parse partition table at offset 446 (0x1BE).
        // Each entry is 16 bytes; find FAT32 type (0x0B or 0x0C).
        s_partition_base = 0;
        for (int e = 0; e < 4; e++) {
            uint8_t* entry = mbr + 446 + e * 16;
            uint8_t ptype = entry[4];
            if (ptype == 0x0B || ptype == 0x0C) {
                s_partition_base = entry[8] | ((uint64_t)entry[9] << 8) |
                                   ((uint64_t)entry[10] << 16) | ((uint64_t)entry[11] << 24);
                break;
            }
        }
        if (s_partition_base == 0) {
            // Fallback: assume partition starts at LBA 63 (common for MBR)
            debug("FAT32: no FAT32 partition found, defaulting to LBA 63\n");
            s_partition_base = 63;
        }
    }

    // Read BPB from the start of the FAT32 partition
    uint8_t bpb_sector[512];
    if (read_sectors(0, bpb_sector, 1) != 0) {
        debug("FAT32: failed to read BPB\n");
        handle_close(s_bdev);
        syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
        while (1) { asm volatile("hlt"); }
    }

    Bpb* bpb = reinterpret_cast<Bpb*>(bpb_sector);

    s_bytes_per_sector  = bpb->bytes_per_sector;
    s_sectors_per_cluster = bpb->sectors_per_cluster;
    s_reserved_sectors  = bpb->reserved_sectors;
    s_sectors_per_fat   = bpb->sectors_per_fat_32;
    s_root_cluster      = bpb->root_cluster;

    debug("FAT32: BPB parsed\n");
    debug("  bytes/sector: "); debug_dec(s_bytes_per_sector); debug("\n");
    debug("  sectors/cluster: "); debug_dec(s_sectors_per_cluster); debug("\n");
    debug("  reserved sectors: "); debug_dec(s_reserved_sectors); debug("\n");
    debug("  sectors/FAT: "); debug_dec(s_sectors_per_fat); debug("\n");
    debug("  root cluster: "); debug_hex(s_root_cluster); debug("\n");

    // Compute layout
    s_fat_start_lba   = s_reserved_sectors;
    s_data_start_lba  = s_fat_start_lba + (uint64_t)bpb->num_fats * s_sectors_per_fat;

    uint64_t total_sectors = bpb->total_sectors_16 != 0
                             ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint64_t data_sectors = total_sectors - s_data_start_lba;
    uint64_t total_clusters = data_sectors / s_sectors_per_cluster;

    debug("  total sectors: "); debug_dec(total_sectors); debug("\n");
    debug("  data start LBA: "); debug_hex(s_data_start_lba); debug("\n");
    debug("  total clusters: "); debug_hex(total_clusters); debug("\n");

    debug("FAT32: ready\n");

    // Event loop: handle OpenPayload from mount Channel
    while (true) {
        uint8_t buf[264];
        int rc = channel_read(MOUNT_CHAN, buf, sizeof(buf));
        if (rc < 0) break;

        OpenPayload* payload = reinterpret_cast<OpenPayload*>(buf);
        uint32_t ack_handle = payload->file_handle;
        const char* path = payload->path;

        debug("FAT32: open '"); debug(path); debug("'\n");

        uint32_t file_size;
        bool is_dir;
        uint32_t start_cluster = fat32_open(path, &file_size, &is_dir);

        if (start_cluster == 0) {
            debug("  not found\n");
            FileResponse err = {-1, 0};
            channel_write(ack_handle, &err, sizeof(err));
            handle_close(ack_handle);
            continue;
        }

        debug("  found, size="); debug_dec(file_size);
        debug(is_dir ? " dir" : " file"); debug("\n");

        // Respond with success on ack_handle
        FileResponse ok = {0, file_size};
        channel_write(ack_handle, &ok, sizeof(ok));

        // Handle file operations
        handle_file(ack_handle, start_cluster, file_size, is_dir);
    }

    debug("FAT32: exiting\n");
    handle_close(s_bdev);
    syscall6(SYS_PROCESS_EXIT, 0, 0, 0, 0, 0);
    while (1) { asm volatile("hlt"); }
}
