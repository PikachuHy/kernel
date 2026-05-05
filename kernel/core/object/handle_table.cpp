#include "kernel/core/object/handle_table.hpp"
#include "kernel/lib/spinlock.hpp"

namespace {
    HandleEntry g_table[MAX_HANDLES];
    handle_t g_free_head = 1;
    SpinLock g_lock;

    void init_free_list() {
        for (handle_t i = 1; i < MAX_HANDLES - 1; i++) {
            g_table[i].obj = reinterpret_cast<KernelObject*>(static_cast<uintptr_t>(i + 1));
        }
    }
}

void handle_table_init() {
    init_free_list();
}

handle_t handle_alloc(KernelObject* obj, Rights rights) {
    g_lock.lock();
    if (g_free_head == 0 || g_free_head >= MAX_HANDLES) {
        g_lock.unlock();
        return INVALID_HANDLE;
    }
    handle_t h = g_free_head;
    g_free_head = static_cast<handle_t>(
        reinterpret_cast<uintptr_t>(g_table[h].obj));
    g_table[h].obj = obj;
    g_table[h].rights = rights;
    obj->AddRef();
    g_lock.unlock();
    return h;
}

void handle_free(handle_t h) {
    if (h == 0 || h >= MAX_HANDLES) return;
    g_lock.lock();
    KernelObject* obj = g_table[h].obj;
    if (obj) {
        g_table[h].obj = reinterpret_cast<KernelObject*>(
            static_cast<uintptr_t>(g_free_head));
        g_table[h].rights = Rights{};
        g_free_head = h;
        obj->Release();
    }
    g_lock.unlock();
}

KernelObject* handle_lookup(handle_t h, Rights needed, Rights* out_rights) {
    if (h == 0 || h >= MAX_HANDLES) return nullptr;
    g_lock.lock();
    KernelObject* obj = g_table[h].obj;
    Rights rights = g_table[h].rights;
    g_lock.unlock();
    if (!obj) return nullptr;
    if (needed.mask != 0 && !rights.has(needed)) return nullptr;
    if (out_rights) *out_rights = rights;
    return obj;
}
