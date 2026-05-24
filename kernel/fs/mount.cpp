#include "kernel/fs/mount.hpp"
#include "kernel/core/object/channel.hpp"
#include "kernel/core/object/process.hpp"

static MountEntry g_mounts[MAX_MOUNTS];
static size_t     g_mount_count = 0;

auto mount_init() -> void {
    for (size_t i = 0; i < MAX_MOUNTS; i++) {
        g_mounts[i].active = false;
        g_mounts[i].fs_channel = nullptr;
        g_mounts[i].fs_process = nullptr;
        g_mounts[i].path[0] = '\0';
    }
    g_mount_count = 0;
}

auto mount_add(const char* path, Channel* fs_channel, Process* fs_process) -> int {
    if (!path) return -1;

    // Replace existing entry if present
    for (size_t i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].active) {
            const char* a = g_mounts[i].path;
            const char* b = path;
            bool same = true;
            while (*a && *b) { if (*a != *b) { same = false; break; } a++; b++; }
            if (same && *a == '\0' && *b == '\0') {
                g_mounts[i].fs_channel = fs_channel;
                g_mounts[i].fs_process = fs_process;
                return 0;
            }
        }
    }

    // Add new entry
    if (g_mount_count >= MAX_MOUNTS) return -1;
    size_t i = g_mount_count;
    size_t j = 0;
    while (path[j] && j < 255) { g_mounts[i].path[j] = path[j]; j++; }
    g_mounts[i].path[j] = '\0';
    g_mounts[i].fs_channel = fs_channel;
    g_mounts[i].fs_process = fs_process;
    g_mounts[i].active = true;
    g_mount_count++;
    return 0;
}

auto mount_remove(const char* path) -> int {
    for (size_t i = 0; i < MAX_MOUNTS; i++) {
        if (!g_mounts[i].active) continue;
        const char* a = g_mounts[i].path;
        const char* b = path;
        bool same = true;
        while (*a && *b) { if (*a != *b) { same = false; break; } a++; b++; }
        if (same && *a == '\0' && *b == '\0') {
            g_mounts[i].active = false;
            g_mounts[i].fs_channel = nullptr;
            g_mounts[i].fs_process = nullptr;
            g_mount_count--;
            return 0;
        }
    }
    return -1;
}

auto mount_resolve(const char* path) -> MountEntry* {
    MountEntry* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < MAX_MOUNTS; i++) {
        if (!g_mounts[i].active) continue;
        const char* prefix = g_mounts[i].path;
        // Check if prefix is a path-prefix of path
        size_t j = 0;
        while (prefix[j] && path[j] && prefix[j] == path[j]) j++;
        if (prefix[j] == '\0' && (path[j] == '/' || path[j] == '\0' || j == 1)) {
            if (j > best_len) {
                best = &g_mounts[i];
                best_len = j;
            }
        }
    }
    return best;
}
