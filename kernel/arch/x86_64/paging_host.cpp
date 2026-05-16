// Host test stub: provides the global g_hhdm symbol.
// In the kernel build, g_hhdm is defined in paging.cpp.
#include "kernel/arch/x86_64/paging.hpp"

uint64_t g_hhdm = 0;
