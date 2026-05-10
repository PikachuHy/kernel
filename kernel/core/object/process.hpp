#pragma once
#include <stdint.h>
#include "kernel/core/object/object.hpp"
#include "kernel/core/object/handle_table.hpp"
#include "kernel/core/mm/vmm.hpp"

struct Thread;
class Vmo;

class Process : public KernelObject {
public:
    static Process* Create(const char* name);

    uint64_t    pml4_phys;    // CR3 value for this process
    VmRegion*   regions;      // sorted intrusive linked list of VmRegions
    HandleTable handles;      // per-process handle table (inline, ~16KB)
    Thread*     threads;      // intrusive linked list via Thread::proc_next
    Process*    parent;       // parent process (for hierarchy; may be nullptr)
    char        name[32];

    // ── VMM operations ──────────────────────────────────────────

    // Map a portion of a VMO into this process's address space.
    // Returns false if the range overlaps or allocation fails.
    bool Map(Vmo* vmo, uint64_t va, uint64_t vmo_offset,
             uint64_t size, uint64_t flags);

    // Unmap a previously mapped range, freeing PTEs and the VmRegion.
    // Returns false if no region at `va` was found.
    bool Unmap(uint64_t va, uint64_t size);

    // Find the VmRegion containing `va`, or nullptr.
    VmRegion* FindRegion(uint64_t va);

    // Handle a page fault at `fault_addr`. was_write indicates whether
    // the fault was caused by a write. Returns false if the fault
    // cannot be resolved (bad address, permission denied, OOM).
    bool HandlePageFault(uint64_t fault_addr, bool was_write);

    // ── Thread management ───────────────────────────────────────

    void AddThread(Thread* t);
    void RemoveThread(Thread* t);

private:
    Process(const char* name, uint64_t pml4);
    ~Process() override;
};
