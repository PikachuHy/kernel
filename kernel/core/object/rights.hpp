#pragma once
#include <stdint.h>

struct Rights {
    enum Bit : uint32_t {
        Read      = 1 << 0,
        Write     = 1 << 1,
        Duplicate = 1 << 2,
        Transfer  = 1 << 3,
    };
    uint32_t mask = 0;

    bool has(Rights needed) const {
        return (mask & needed.mask) == needed.mask;
    }
};
