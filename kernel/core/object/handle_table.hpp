#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"
#include "kernel/lib/spinlock.hpp"

using handle_t = uint32_t;
constexpr handle_t INVALID_HANDLE = 0;
constexpr int MAX_HANDLES = 1024;

struct HandleEntry {
    KernelObject* obj = nullptr;
    Rights rights{};
};

class HandleTable {
public:
    auto Init() -> void;

    auto Alloc(KernelObject* obj, Rights rights) -> handle_t;
    auto Free(handle_t h) -> void;
    auto Lookup(handle_t h, Rights needed = Rights{},
                Rights* out_rights = nullptr) -> KernelObject*;

    // For iteration during process teardown. Writes up to `max` entries
    // into out_objs/out_handles. Returns actual count written.
    auto ForEach(KernelObject** out_objs, handle_t* out_handles, int max) -> int;

    // Free the backing array (called during Process teardown).
    auto Destroy() -> void;

private:
    HandleEntry* entries_;   // buddy-allocated array, 4 pages (16KB for 1024 entries)
    handle_t     free_head_;
    SpinLock     lock_;
};

// ── Temporary backward-compatible globals ──────────────────────────
// These dispatch to a fallback handle table (set to kernel process's table).
// Remove after all callers are migrated to per-process handles.

auto handle_table_set_fallback(HandleTable* ht) -> void;

auto handle_alloc(KernelObject* obj, Rights rights) -> handle_t;
auto handle_free(handle_t h) -> void;
auto handle_lookup(handle_t h, Rights needed = Rights{},
                   Rights* out_rights = nullptr) -> KernelObject*;
