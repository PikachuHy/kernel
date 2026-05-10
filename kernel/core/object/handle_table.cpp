#include "kernel/core/object/handle_table.hpp"

// ── HandleTable ─────────────────────────────────────────────────────

void HandleTable::Init() {
    for (handle_t i = 1; i < MAX_HANDLES - 1; i++) {
        entries_[i].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(i + 1));
    }
    entries_[MAX_HANDLES - 1].obj = nullptr;
    free_head_ = 1;
}

handle_t HandleTable::Alloc(KernelObject* obj, Rights rights) {
    lock_.lock();
    if (free_head_ == 0 || free_head_ >= MAX_HANDLES) {
        lock_.unlock();
        return INVALID_HANDLE;
    }
    handle_t h = free_head_;
    free_head_ = static_cast<handle_t>(
        reinterpret_cast<uintptr_t>(entries_[h].obj));
    entries_[h].obj = obj;
    entries_[h].rights = rights;
    obj->AddRef();
    lock_.unlock();
    return h;
}

void HandleTable::Free(handle_t h) {
    if (h == 0 || h >= MAX_HANDLES) return;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    if (obj) {
        entries_[h].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(free_head_));
        entries_[h].rights = Rights{};
        free_head_ = h;
        obj->Release();
    }
    lock_.unlock();
}

KernelObject* HandleTable::Lookup(handle_t h, Rights needed,
                                   Rights* out_rights) {
    if (h == 0 || h >= MAX_HANDLES) return nullptr;
    lock_.lock();
    KernelObject* obj = entries_[h].obj;
    Rights rights = entries_[h].rights;
    lock_.unlock();
    if (!obj) return nullptr;
    if (needed.mask != 0 && !rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = rights;
    return obj;
}

int HandleTable::ForEach(KernelObject** out_objs, handle_t* out_handles,
                          int max) {
    int count = 0;
    lock_.lock();
    for (handle_t h = 1; h < MAX_HANDLES && count < max; h++) {
        if (entries_[h].obj) {
            out_objs[count] = entries_[h].obj;
            out_handles[count] = h;
            count++;
        }
    }
    lock_.unlock();
    return count;
}

// ── Backward-compat globals ─────────────────────────────────────────

static HandleTable* g_fallback_ht = nullptr;

void handle_table_set_fallback(HandleTable* ht) { g_fallback_ht = ht; }

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    if (g_fallback_ht) return g_fallback_ht->Alloc(obj, rights);
    return INVALID_HANDLE;
}

void handle_free(handle_t h) {
    if (g_fallback_ht) g_fallback_ht->Free(h);
}

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    if (g_fallback_ht) return g_fallback_ht->Lookup(h, needed, out_rights);
    return nullptr;
}
