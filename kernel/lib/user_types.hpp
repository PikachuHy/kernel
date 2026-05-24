// kernel/lib/user_types.hpp
// Shared type aliases for ring-3 user-space programs (freestanding — no stdlib)
#pragma once

using int32_t   = int;
using uint8_t   = unsigned char;
using uint16_t  = unsigned short;
using uint32_t  = unsigned int;
using uint64_t  = unsigned long long;
using size_t    = decltype(sizeof(0));
using nullptr_t = decltype(nullptr);
