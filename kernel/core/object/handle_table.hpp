#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"
#include "kernel/lib/spinlock.hpp"
#include "kernel/lib/result.hpp"

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

template <typename T>
auto typed_lookup(HandleTable& table, handle_t h, Rights needed = {}) -> km::Result<T*> {
    auto* obj = table.Lookup(h, needed);
    if (!obj || obj->type() != T::kType) return km::Result<T*>::Err(-1);
    return km::Result<T*>::Ok(static_cast<T*>(obj));
}

class ScopedHandle {
    HandleTable* table_ = nullptr;
    handle_t handle_ = INVALID_HANDLE;
public:
    ScopedHandle(HandleTable& t, handle_t h) noexcept : table_(&t), handle_(h) {}
    ~ScopedHandle() { if (table_ && handle_ != INVALID_HANDLE) table_->Free(handle_); }

    ScopedHandle(ScopedHandle&& other) noexcept
        : table_(other.table_), handle_(other.handle_) {
        other.table_ = nullptr;
        other.handle_ = INVALID_HANDLE;
    }

    auto operator=(ScopedHandle&& other) noexcept -> ScopedHandle& {
        if (this != &other) {
            if (table_ && handle_ != INVALID_HANDLE) table_->Free(handle_);
            table_ = other.table_;
            handle_ = other.handle_;
            other.table_ = nullptr;
            other.handle_ = INVALID_HANDLE;
        }
        return *this;
    }

    ScopedHandle(const ScopedHandle&) = delete;
    auto operator=(const ScopedHandle&) = delete;

    auto get() const noexcept -> handle_t { return handle_; }
    auto release() noexcept -> handle_t {
        auto h = handle_; handle_ = INVALID_HANDLE; table_ = nullptr; return h;
    }
};
