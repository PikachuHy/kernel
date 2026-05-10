#pragma once
#include <stdint.h>
#include <stddef.h>
#include "kernel/core/object/object.hpp"
#include "kernel/lib/spinlock.hpp"
#include "kernel/arch/x86_64/paging.hpp"

struct CowPage {
    uint64_t phys_addr;   // 0 = not committed
    uint32_t cow_refs;    // number of VMOs sharing this page
};

class Vmo : public KernelObject {
public:
    enum Type : uint8_t { Anonymous, Physical };

    static Vmo* CreateAnonymous(uint64_t size);
    static Vmo* CreatePhysical(uint64_t size, uint64_t phys_base);

    // Get physical address for page at `offset`. Allocates a zero-filled page
    // if not committed (Anonymous). If for_write and page is COW-shared,
    // copies the page. Returns 0 on OOM or out-of-range.
    uint64_t GetPage(uint64_t offset, bool for_write);

    // Create a COW clone sharing all committed pages.
    // The child shares physical pages with the parent; cow_refs are
    // incremented. On write, the child gets its own copy.
    Vmo* CloneCoW();

    uint64_t size()      const { return size_; }
    uint64_t num_pages() const { return num_pages_; }
    Type     type()      const { return type_; }

    ~Vmo() override;

    // Override the direct-map base for phys-to-virt translation.
    // Default is DIRECT_MAP_BASE (kernel's direct map region).
    // Used by host tests to set the correct HHDM offset.
    static void SetDirectMapOffset(uint64_t offset);

private:
    Vmo(Type t, uint64_t size);

    Type       type_;
    uint64_t   size_;
    uint64_t   num_pages_;
    CowPage**  pages_;    // array[num_pages_]; nullptr entries = not committed
    SpinLock   lock_;

    static uint64_t s_direct_map_offset_;
};
