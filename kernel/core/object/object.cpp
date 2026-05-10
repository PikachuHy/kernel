#include "kernel/core/object/object.hpp"
#include "kernel/core/mm/slab.hpp"

void KernelObject::Release() {
    if (--ref_count_ == 0) {
        this->~KernelObject();
        kfree(this);
    }
}
