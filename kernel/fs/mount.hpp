#pragma once
#include <stdint.h>
#include <stddef.h>

class Channel;
class Process;

struct MountEntry {
    char     path[256];
    Channel* fs_channel;   // kernel-end of the mount Channel (raw pointer)
    Process* fs_process;   // the FS server process (for handle allocation)
    bool     active;
};

constexpr size_t MAX_MOUNTS = 16;

// Initialize the mount table.
auto mount_init() -> void;

// Add or replace a mount entry. The kernel takes ownership of fs_channel.
// Returns 0 on success, -1 if table is full.
auto mount_add(const char* path, Channel* fs_channel, Process* fs_process) -> int;

// Remove a mount entry by path. Returns 0 on success, -1 if not found.
auto mount_remove(const char* path) -> int;

// Find the longest-prefix matching mount entry for the given path.
// Returns nullptr if no match.
auto mount_resolve(const char* path) -> MountEntry*;
