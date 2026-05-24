#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/mm/vmm.hpp"

struct Thread;
class Vmo;

class Process : public KernelObject {
public:
    static constexpr auto kType = KernelObject::Type::Process;

    // Create a process. If kernel_process is true, shares the kernel PML4
    // template. Otherwise allocates a fresh PML4 via vmm_create_user_pml4().
    static auto Create(const char* name, bool kernel_process = false) -> Process*;

    uint64_t    pml4_phys;    // CR3 value for this process
    VmRegion*   regions;      // sorted intrusive linked list of VmRegions
    HandleTable handles;      // per-process handle table (inline, ~16KB)
    Thread*     threads;      // intrusive linked list via Thread::proc_next
    Process*    parent;       // parent process (for hierarchy; may be nullptr)
    char        name[32];

    // ── VMM operations ──────────────────────────────────────────

    // Map a portion of a VMO into this process's address space.
    // Returns false if the range overlaps or allocation fails.
    auto Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
             uint64_t size, uint64_t flags) -> bool;

    // Unmap a previously mapped range, freeing PTEs and the VmRegion.
    // Returns false if no region at `va` was found.
    auto Unmap(uint64_t va, uint64_t size) -> bool;

    // Find the VmRegion containing `va`, or nullptr.
    auto FindRegion(uint64_t va) -> VmRegion*;

    // Handle a page fault at `fault_addr`. was_write indicates whether
    // the fault was caused by a write. Returns false if the fault
    // cannot be resolved (bad address, permission denied, OOM).
    auto HandlePageFault(uint64_t fault_addr, bool was_write) -> bool;

    // ── Thread management ───────────────────────────────────────

    auto AddThread(Thread* t) -> void;
    auto RemoveThread(Thread* t) -> void;

private:
    Process(const char* name, uint64_t pml4);
    ~Process() override;
};
