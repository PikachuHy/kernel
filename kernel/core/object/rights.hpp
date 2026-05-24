#pragma once
#include <stdint.h>

struct Rights {
    enum Bit : uint32_t {
        Read      = 1 << 0,
        Write     = 1 << 1,
        Duplicate = 1 << 2,
        Transfer  = 1 << 3,
        // Non-permission flag: marks a Channel handle as the "B" endpoint.
        // Used internally by sys_channel_read/write to route messages to
        // the correct internal queue.  Not checked by Lookup for access.
        ChannelEndpointB = 1 << 4,
    };
    uint32_t mask = 0;

    auto has(Rights needed) const noexcept -> bool {
        return (mask & needed.mask) == needed.mask;
    }
};
