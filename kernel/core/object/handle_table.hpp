#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/rights.hpp"

using handle_t = uint32_t;
constexpr handle_t INVALID_HANDLE = 0;
constexpr int MAX_HANDLES = 1024;

struct HandleEntry {
    KernelObject* obj = nullptr;
    Rights rights{};
};

void    handle_table_init();
handle_t handle_alloc(KernelObject* obj, Rights rights);
void    handle_free(handle_t h);
KernelObject* handle_lookup(handle_t h, Rights needed = Rights{}, Rights* out_rights = nullptr);
