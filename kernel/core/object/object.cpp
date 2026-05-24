#include "kernel/core/object/object.hpp"
#include "kernel/core/mm/slab.hpp"

auto KernelObject::Release() -> void {
    if (--ref_count_ == 0) {
        this->~KernelObject();
        kfree(this);
    }
}
