#pragma once
#include <stdint.h>
#include "kernel/core/mm/vmo.hpp"

// VmRegion flags
constexpr uint64_t VM_READ   = 1ULL << 0;
constexpr uint64_t VM_WRITE  = 1ULL << 1;
constexpr uint64_t VM_EXEC   = 1ULL << 2;
constexpr uint64_t VM_COW    = 1ULL << 3;
constexpr uint64_t VM_USER   = 1ULL << 4;

struct VmRegion {
    VmRegion* next;       // intrusive linked list, sorted by base_va ascending
    uint64_t  base_va;
    uint64_t  size;       // page-aligned
    Vmo*      vmo;
    uint64_t  vmo_offset; // offset within the VMO where this mapping starts
    uint64_t  flags;      // VM_READ | VM_WRITE | VM_EXEC | VM_COW | VM_USER
};

// Address space bounds for user processes
constexpr uint64_t USER_SPACE_START = 0x0;
constexpr uint64_t USER_SPACE_END   = 0x00007FFFFFFFFFFFULL;

// ── PML4 management ─────────────────────────────────────────────

// Create a new PML4 for a user process. Copies kernel-half entries
// (indices 256-511) from the kernel template saved by paging_init.
// User-half entries (0-255) are zeroed (via bitmap_alloc_page).
// Returns physical address of the new PML4, or 0 on allocation failure.
uint64_t vmm_create_user_pml4();

// Free a user PML4 and all intermediate page tables in the user half
// (PML4 indices 0-255). Does NOT touch kernel-half entries.
void vmm_destroy_user_pml4(uint64_t pml4_phys);

// ── VmRegion list operations ─────────────────────────────────────

// Insert a region into the sorted (by base_va) linked list.
// Returns false if the range overlaps an existing region.
// The region must be kmalloc'd; ownership transfers to the list.
bool vmm_insert_region(VmRegion** head, VmRegion* region);

// Remove the region containing `va` and spanning `size` bytes from the
// list. Unmaps all pages in [va, va+size) from the given PML4.
// Frees the VmRegion struct. Returns the removed region (already freed!)
// or nullptr if not found. Caller should NOT use the returned pointer.
VmRegion* vmm_remove_region(VmRegion** head, uint64_t va, uint64_t size,
                             uint64_t pml4_phys);

// Find the VmRegion containing `va`, or nullptr.
VmRegion* vmm_find_region(VmRegion* head, uint64_t va);
