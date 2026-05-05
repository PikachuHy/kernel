# Phase 5: Preemptive Scheduler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a preemptive, priority-based scheduler with SMP-aware per-CPU run queues, context switching, and timer-driven time-slice preemption.

**Architecture:** Threads are represented by `Thread` objects allocated from the slab allocator. Each CPU has a `RunQueue` with 8 priority levels (bitmap + linked list for O(1) select). Context switch is an assembly routine (`switch_to`) that saves callee-saved registers and swaps stacks. The LAPIC timer drives preemption via `scheduler_tick()`. An idle thread per CPU runs `hlt` when no threads are ready.

**Tech Stack:** C++26 (freestanding, no-exceptions), x86-64 assembly (GAS), Limine page tables/HHDM, kmalloc/kfree via slab, LAPIC timer, SMP per-CPU arrays

---

## File Structure

```
kernel/core/sched/           (NEW directory)
├── sched.hpp                (NEW) — Thread, RunQueue, scheduler API
├── sched.cpp                (NEW) — scheduler implementation
├── switch.S                 (NEW) — context switch assembly
└── BUILD.bazel              (NEW)

kernel/arch/x86_64/
├── boot.cpp                 (MODIFY) — wire scheduler into boot, replace serial-echo loop
└── smp.hpp                  (MODIFY) — add scheduler fields to PerCpu

kernel/
└── BUILD.bazel              (MODIFY) — add sched dependency

test/sched/                  (NEW directory)
├── sched_test.cpp           (NEW) — host-side scheduler tests
└── BUILD.bazel              (NEW)
```

---

### Task 1: Thread Control Block and RunQueue (header-only)

**Files:**
- Create: `kernel/core/sched/sched.hpp`
- Modify: `kernel/arch/x86_64/smp.hpp` — add sched fields to PerCpu

- [ ] **Step 1: Write sched.hpp**

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr int SCHED_PRIORITIES = 8;
constexpr int SCHED_DEFAULT_TIMESLICE_MS = 10;
constexpr int MAX_THREAD_NAME = 16;

enum class ThreadState : uint8_t {
    Ready     = 0,
    Running   = 1,
    Blocked   = 2,
    Sleeping  = 3,
    Dead      = 4,
};

struct Thread {
    // Saved register state (callee-saved: rbx, rbp, r12-r15)
    // rsp is stored first — switch_to saves rsp into Thread::rsp,
    // then loads new rsp from the incoming Thread, and pops regs from there.
    uint64_t rsp;       // saved stack pointer (offset 0)
    uint64_t rflags;    // saved RFLAGS (offset 8, pushed by pushfq in switch_to)

    ThreadState state;
    uint8_t   priority;         // 0=highest, 7=lowest
    int32_t   time_slice_ms;    // remaining time slice
    uint32_t  tid;
    char      name[MAX_THREAD_NAME];

    // Run-queue linkage: intrusive linked list
    Thread*   next;

    // Stack management
    void*     stack_bottom;  // low address (for freeing)
    size_t    stack_size;    // total stack allocation size
};

static_assert(offsetof(Thread, rsp) == 0,
    "rsp must be at offset 0 for switch_to assembly");

// ── RunQueue ────────────────────────────────────────────────

struct RunQueue {
    Thread*  heads[SCHED_PRIORITIES];  // linked list per priority
    uint8_t  bitmap;                    // bit i set if heads[i] != nullptr
    uint32_t count;                     // total threads in queue

    void init() {
        for (int i = 0; i < SCHED_PRIORITIES; i++) heads[i] = nullptr;
        bitmap = 0; count = 0;
    }

    void push(Thread* t) {
        t->next = heads[t->priority];
        heads[t->priority] = t;
        bitmap |= (1u << t->priority);
        count++;
    }

    Thread* pop() {
        if (bitmap == 0) return nullptr;
        // Find highest-priority non-empty queue
        int prio = __builtin_ctz(bitmap);
        Thread* t = heads[prio];
        heads[prio] = t->next;
        if (!heads[prio]) bitmap &= ~(1u << prio);
        count--;
        return t;
    }

    void remove(Thread* target) {
        // Walk all queues to find and remove target
        for (int i = 0; i < SCHED_PRIORITIES; i++) {
            Thread** prev = &heads[i];
            for (Thread* t = heads[i]; t; prev = &t->next, t = t->next) {
                if (t == target) {
                    *prev = t->next;
                    if (!heads[i]) bitmap &= ~(1u << i);
                    count--;
                    return;
                }
            }
        }
    }

    // Steal one thread from the back (lowest priority) of another CPU's queue.
    // Used for SMP work stealing.
    Thread* steal_one() {
        if (count == 0) return nullptr;
        for (int i = SCHED_PRIORITIES - 1; i >= 0; i--) {
            if (heads[i]) {
                Thread* t = heads[i];
                heads[i] = t->next;
                if (!heads[i]) bitmap &= ~(1u << i);
                count--;
                return t;
            }
        }
        return nullptr;
    }
};

// ── Scheduler API ──────────────────────────────────────────

// Threads: create, start (make ready), yield (voluntary), exit
Thread* thread_create(void (*entry)(), const char* name, uint8_t priority);
void    thread_start(Thread* t);
void    thread_yield();
[[noreturn]] void thread_exit();

// Scheduler lifecycle
void scheduler_init();
void scheduler_start();       // enables interrupts, never returns
void scheduler_tick();        // called from timer ISR

// Current thread accessor
Thread* current_thread();

// Idle thread (per-CPU)
Thread* get_idle_thread();

// ── Assembly: context switch ────────────────────────────────
// Saves callee-saved regs to prev->rsp chain, loads next->rsp chain.
// Called from scheduler_schedule().
extern "C" void switch_to(Thread* prev, Thread* next);

// ── Per-CPU integration ─────────────────────────────────────
// Called by scheduler_init() — sets up per-CPU structures.
// Implemented in sched.cpp.
void sched_init_per_cpu(uint32_t cpu_id);
```

- [ ] **Step 2: Update smp.hpp — add scheduler fields to PerCpu**

Replace:
```cpp
struct PerCpu {
    uint32_t cpu_id;
    uint32_t lapic_id;
    bool online;        // set true when AP reaches idle loop
    uint64_t reserved;  // padding (future: stack ptr, idle thread ptr)
};
```

With:
```cpp
struct PerCpu {
    uint32_t cpu_id;
    uint32_t lapic_id;
    bool online;        // set true when AP reaches idle loop
    bool sched_ready;   // set true after scheduler is initialized on this CPU

    // Scheduler fields
    Thread*   current_thread;  // currently running thread on this CPU
    RunQueue  run_queue;       // per-CPU run queue
    Thread*   idle_thread;     // this CPU's idle thread
};
```

- [ ] **Step 3: Build and verify compilation**

Run: `bazel build //kernel:kernel`
Expected: compilation succeeds (linker may complain about missing symbols — that's fine, they come in later tasks)

---

### Task 2: Context Switch Assembly

**Files:**
- Create: `kernel/core/sched/switch.S`

- [ ] **Step 1: Write switch.S**

```asm
# kernel/core/sched/switch.S
# Context switch: switch_to(Thread* prev, Thread* next)
#
# Calling convention: System V AMD64 — prev in %rdi, next in %rsi
#
# Layout saved on the stack (top to bottom, growing down):
#   [rsp]     = return address (pushed by call instruction)
#   [rsp-8]   = pushfq (rflags, 8 bytes)
#   [rsp-16]  = rbx
#   [rsp-24]  = rbp
#   [rsp-32]  = r12
#   [rsp-40]  = r13
#   [rsp-48]  = r14
#   [rsp-56]  = r15
#
# Thread::rsp points to the bottom of this frame (where rflags is saved).
# switch_to() is called by scheduler_schedule(), which already saved its own
# callee-save context.  The return from switch_to() pops %rip from the
# NEXT thread's stack, resuming where that thread last called switch_to().

.section .text
.globl switch_to

switch_to:
    # Save callee-saved registers + RFLAGS onto current stack
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    # Save current RSP into prev->rsp
    movq %rsp, (%rdi)       # Thread::rsp = current stack pointer

    # Load next->rsp (callee-save chain of the incoming thread)
    movq (%rsi), %rsp       # switch to next thread's stack

    # Restore callee-saved registers from new stack
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq

    # Return to where next thread was preempted (or to thread startup stub)
    ret
```

- [ ] **Step 2: Verify switch.S assembles**

Run: `bazel build //kernel/core/sched:switch`
Expected: no errors

---

### Task 3: BUILD.bazel for sched library

**Files:**
- Create: `kernel/core/sched/BUILD.bazel`
- Modify: `kernel/BUILD.bazel` — add sched dependency

- [ ] **Step 1: Create kernel/core/sched/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

exports_files(
    ["sched.cpp", "sched.hpp", "switch.S"],
    visibility = ["//test:__subpackages__"],
)

cc_library(
    name = "sched",
    srcs = [
        "sched.cpp",
        "switch.S",
    ],
    hdrs = ["sched.hpp"],
    deps = [
        "//kernel/arch/x86_64:arch",
        "//kernel/core/mm:mm",
        "//kernel/lib:klib",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 2: Update kernel/BUILD.bazel — add sched to kernel binary deps**

Replace the kernel cc_binary to add `"//kernel/core/sched:sched"`:

```python
cc_binary(
    name = "kernel",
    deps = [
        "//kernel/arch/x86_64:arch",
        "//kernel/lib:klib",
        "//kernel/core/mm:mm",
        "//kernel/core/sched:sched",
    ],
    additional_linker_inputs = ["//kernel/arch/x86_64:linker_script"],
    linkopts = ["-Wl,-T,$(location //kernel/arch/x86_64:linker_script)"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Verify build compiles all stubs**

Run: `bazel build //kernel:kernel`
Expected: linker errors for undefined `sched_init_per_cpu`, `scheduler_init`, `thread_create`, etc. — confirms everything is wired

---

### Task 4: Thread lifecycle and stack allocation

**Files:**
- Create: `kernel/core/sched/sched.cpp`

- [ ] **Step 1: Write sched.cpp — thread lifecycle (no context switch yet)**

```cpp
#include "kernel/core/sched/sched.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

namespace {

constexpr size_t DEFAULT_STACK_PAGES = 2;  // 8KB per thread
uint32_t g_next_tid = 1;

// Per-CPU current thread pointer stored in g_per_cpu[].current_thread
// — set by scheduler_schedule() on each context switch.

} // namespace

Thread* thread_create(void (*entry)(), const char* name, uint8_t priority) {
    if (priority >= SCHED_PRIORITIES) return nullptr;
    if (!entry) return nullptr;

    // Allocate Thread control block from slab
    Thread* t = static_cast<Thread*>(kmalloc(sizeof(Thread)));
    if (!t) return nullptr;

    // Allocate stack
    size_t stack_size = DEFAULT_STACK_PAGES * PAGE_SIZE;
    void* stack_phys = bitmap_alloc_page();
    if (!stack_phys) { kfree(t); return nullptr; }
    // For simplicity, single-page stacks.  bitmap_alloc_page returns phys addr.
    // We access via HHDM.

    // Initialize the Thread
    t->state     = ThreadState::Ready;
    t->priority  = priority;
    t->time_slice_ms = SCHED_DEFAULT_TIMESLICE_MS;
    t->tid       = g_next_tid++;
    t->next      = nullptr;
    t->stack_bottom = stack_phys;
    t->stack_size   = PAGE_SIZE;

    // Copy name
    for (int i = 0; i < MAX_THREAD_NAME - 1 && name[i]; i++) {
        t->name[i] = name[i];
    }
    t->name[MAX_THREAD_NAME - 1] = '\0';

    // Set up initial stack frame so that when switch_to "returns" to this
    // thread, it pops the entry function address into RIP.
    //
    // Initial stack (grows down):
    //   [top-8]   = entry function pointer (where switch_to's "ret" jumps)
    //   rsp       = top - 8
    //
    // When switch_to loads this rsp, it pops r15..rflags, then "ret" pops
    // the entry address.
    uint64_t hhdm = 0; // will be set at init
    // HHDM offset is stored globally — we hold it in a static.
    // Actually, due to how we access stack_bottom, we need the hhdm offset.
    // It's stored in a file-static during scheduler_init.

    // We defer final rsp computation to thread_start(), since we need the
    // HHDM offset to convert phys→virt.  Just store the phys addr for now.
    t->rsp = reinterpret_cast<uint64_t>(stack_phys) + PAGE_SIZE - 8;
    // ^ This is a physical address.  During thread_start we translate it.
    // Wait — we can compute virt if we have hhdm.  thread_create doesn't
    // have hhdm, so we store phys in a temporary field and fix up in
    // thread_start.

    return t;
}

// Store HHDM offset globally for stack phys→virt translation
static uint64_t g_hhdm = 0;

static inline void* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<void*>(g_hhdm + phys);
}

void thread_start(Thread* t) {
    if (!t || t->state != ThreadState::Ready) return;

    // Fix up rsp: convert physical to virtual
    t->rsp = g_hhdm + t->rsp;

    // Write entry function address at top of stack
    uint64_t* stack_top = reinterpret_cast<uint64_t*>(t->rsp);
    *stack_top = reinterpret_cast<uint64_t>(thread_exit);
    // Push a fake "return address" that thread_exit is called when entry returns.
    // Actually: we want entry() to run, and if it returns, call thread_exit.
    //
    // Better approach: the initial stack frame should look like:
    //   [rsp]  = address of a trampoline that calls entry then thread_exit
    //
    // Even simpler: entry is void(*)(), so we set up:
    //   [top of stack] = entry address (for ret)
    // When switch_to "returns", it pops this into RIP and runs entry.
    // But if entry returns normally, RIP goes to whatever was next on stack
    // which is undefined.  So we push thread_exit first, then entry on top.
    // "Entry returns into thread_exit."

    uint64_t* sp = stack_top;
    sp--;  // grow down
    *sp = reinterpret_cast<uint64_t>(thread_exit);  // "return address" after entry
    sp--;  // grow down more
    *sp = reinterpret_cast<uint64_t>(entry);  // actual "return address" from switch_to
    t->rsp = reinterpret_cast<uint64_t>(sp);

    // If this is a brand-new thread, its callee-saved regs were never
    // pushed (they don't exist yet).  switch_to's pop sequence will
    // pop garbage into r15..rbx and popfq, then ret into entry().
    // This is fine because a new thread doesn't need preserved register values.

    // Add to current CPU's run queue
    // We need to know which CPU we're on.  For now, put on CPU 0.
    // In SMP, we'd use the caller's CPU.  thread_start is always called
    // from the calling thread's context, so we can determine CPU via
    // current_thread or a per-CPU variable.
    uint32_t cpu = 0; // FIXME: determine current CPU

    g_per_cpu[cpu].run_queue.push(t);
}

[[noreturn]] void thread_exit() {
    Thread* cur = current_thread();
    if (cur) {
        cur->state = ThreadState::Dead;
        klog("Thread "); klog(cur->name); klog(" exited\n");
    }
    // Schedule next thread — never returns
    scheduler_schedule();
    __builtin_unreachable();
}
```

- [ ] **Step 2: Add scheduler_schedule() stub**

Append to sched.cpp:

```cpp
// Forward declaration for scheduler_schedule which needs switch_to
// (switch_to is in switch.S, declared in sched.hpp)

void scheduler_schedule() {
    uint32_t cpu_id = 0; // FIXME: get current CPU

    Thread* prev = g_per_cpu[cpu_id].current_thread;
    Thread* next = g_per_cpu[cpu_id].run_queue.pop();

    if (!next) {
        // Nothing ready — switch to idle
        next = g_per_cpu[cpu_id].idle_thread;
    }

    if (next == prev) return;  // nothing to switch

    if (prev) {
        prev->state = ThreadState::Ready;
        if (prev != g_per_cpu[cpu_id].idle_thread) {
            g_per_cpu[cpu_id].run_queue.push(prev);
        }
    }

    next->state = ThreadState::Running;
    g_per_cpu[cpu_id].current_thread = next;

    switch_to(prev, next);
}
```

- [ ] **Step 3: Verify compilation**

Run: `bazel build //kernel:kernel`
Expected: compiles, links (but won't boot correctly yet)

---

### Task 5: Idle thread per CPU

**Files:**
- Modify: `kernel/core/sched/sched.cpp` — add idle thread creation

- [ ] **Step 1: Write idle thread entry and init function**

Replace the stubs in sched.cpp with complete implementation. Add before `thread_create`:

```cpp
// ── Idle thread ─────────────────────────────────────────────

static void idle_entry() {
    while (1) {
        asm volatile("sti; hlt; cli");
    }
}

static Thread* create_idle_thread(uint32_t cpu_id) {
    // Allocate a dedicated stack for the idle thread
    void* stack_phys = bitmap_alloc_page();
    if (!stack_phys) return nullptr;

    Thread* idle = static_cast<Thread*>(kmalloc(sizeof(Thread)));
    if (!idle) return nullptr;

    idle->state      = ThreadState::Running;  // idle is always "running"
    idle->priority   = SCHED_PRIORITIES - 1;  // lowest priority
    idle->time_slice_ms = 0;                   // never preempted by timer
    idle->tid        = 0;                      // tid 0 = idle
    idle->next       = nullptr;
    idle->stack_bottom = stack_phys;
    idle->stack_size   = PAGE_SIZE;

    // Set name
    const char* name = "\0idle\0";
    for (int i = 0; i < MAX_THREAD_NAME - 1 && name[i]; i++) {
        idle->name[i] = name[i];
    }
    idle->name[0] = 'i'; idle->name[1] = 'd'; idle->name[2] = 'l';
    idle->name[3] = 'e'; idle->name[4] = '\0';

    // Set up idle stack: idle_entry's "return address"
    uint64_t virt_stack_top = g_hhdm + reinterpret_cast<uint64_t>(stack_phys) + PAGE_SIZE;
    uint64_t* sp = reinterpret_cast<uint64_t*>(virt_stack_top);
    sp--;
    *sp = reinterpret_cast<uint64_t>(idle_entry);
    sp--;
    *sp = reinterpret_cast<uint64_t>(thread_exit);  // safety: if idle_entry returns
    idle->rsp = reinterpret_cast<uint64_t>(sp);

    return idle;
}

// ── Per-CPU Scheduler Init ──────────────────────────────────

static bool g_sched_initialized = false;

void scheduler_init() {
    if (g_sched_initialized) return;

    // The BSP calls this.  Per-CPU setup for all online CPUs.
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        g_per_cpu[i].run_queue.init();
        g_per_cpu[i].idle_thread = create_idle_thread(i);
        if (!g_per_cpu[i].idle_thread) {
            KPANIC("scheduler_init: failed to create idle thread");
        }
        g_per_cpu[i].current_thread = g_per_cpu[i].idle_thread;
        g_per_cpu[i].sched_ready = true;
    }

    g_sched_initialized = true;
    klog("Scheduler: initialized with ");
    klog_hex(g_cpu_count);
    klog(" CPU(s)\n");
}
```

Note: sched.cpp needs `#include "kernel/lib/panic.hpp"` if not already.

- [ ] **Step 2: Add HHDM initialization to scheduler_init**

Add `extern uint64_t g_hhdm;` declaration, or pass hhdm to scheduler_init:

Change `scheduler_init()` signature to `scheduler_init(uint64_t hhdm)` and set `g_hhdm = hhdm;` at the top.

- [ ] **Step 3: Verify complete build**

Run: `bazel build //kernel:kernel`
Expected: successful build

---

### Task 6: Timer preemption (scheduler_tick)

**Files:**
- Modify: `kernel/core/sched/sched.cpp` — add scheduler_tick
- Modify: `kernel/arch/x86_64/boot.cpp` — hook timer to scheduler

- [ ] **Step 1: Add scheduler_tick implementation**

```cpp
void scheduler_tick() {
    if (!g_sched_initialized) return;

    Thread* cur = current_thread();
    if (!cur || cur == g_per_cpu[0].idle_thread) return;

    cur->time_slice_ms--;
    if (cur->time_slice_ms <= 0) {
        cur->time_slice_ms = SCHED_DEFAULT_TIMESLICE_MS;
        scheduler_schedule();
    }
}
```

- [ ] **Step 2: Add current_thread() helper**

```cpp
Thread* current_thread() {
    // Determine current CPU: for now, CPU 0 (BSP).
    // In the future, this reads the GS segment base or LAPIC ID.
    return g_per_cpu[0].current_thread;
}
```

- [ ] **Step 3: Modify boot.cpp — replace demo timer with scheduler tick**

Replace the periodic timer registration (the demo lambda with `timer_periodic(1000, ...)` and the serial polling loop) with:

```cpp
// ── Phase 5: Scheduler ──
klog("=== Phase 5: Scheduler ===\n\n");

klog("Initializing scheduler...\n");
scheduler_init(hhdm);
klog("Scheduler ready.\n\n");

// Create demo threads
Thread* t1 = thread_create([](){
    while (1) {
        klog("[thread-A] tick\n");
        for (volatile int i = 0; i < 10000000; i++) asm volatile("pause");
    }
}, "thread-A", 1);

Thread* t2 = thread_create([](){
    while (1) {
        klog("[thread-B] tick\n");
        for (volatile int i = 0; i < 10000000; i++) asm volatile("pause");
    }
}, "thread-B", 1);

if (t1) { thread_start(t1); klog("Started thread-A\n"); }
if (t2) { thread_start(t2); klog("Started thread-B\n"); }

// Hook timer to scheduler
timer_periodic(10000, [](uint64_t) -> bool {  // 10ms tick for preemption
    scheduler_tick();
    return true;
});

klog("\nScheduler starting...\n");
asm volatile("sti");
scheduler_start();
```

- [ ] **Step 4: Add scheduler_start() implementation**

```cpp
void scheduler_start() {
    // Pick the first real thread (not idle) to run
    Thread* first = g_per_cpu[0].run_queue.pop();
    if (!first) {
        // No threads — idle
        first = g_per_cpu[0].idle_thread;
    } else {
        first->state = ThreadState::Running;
        g_per_cpu[0].current_thread = first;
    }

    // Load this thread's stack and "return" to it.
    // We bypass switch_to here because there's no prev thread to save.
    uint64_t new_rsp = first->rsp;
    asm volatile(
        "movq %0, %%rsp\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbp\n\t"
        "popq %%rbx\n\t"
        "popfq\n\t"
        "ret"
        : : "r"(new_rsp) : "memory"
    );
    __builtin_unreachable();
}
```

- [ ] **Step 5: Wire RSDP and SMP, then scheduler in boot sequence**

Modify boot.cpp to call `smp_init()` before `scheduler_init()` (so g_cpu_count is correct), then call `scheduler_init(hhdm)`.

The boot sequence should be:
```
Phase 1: GDT, IDT
Phase 2: Memory management (PMM, bitmap, slab)
Phase 3: APIC, Timer, IRQ, syscall
Phase 4: SMP bringup → smp_init(hhdm, rsdp_phys)
Phase 5: Scheduler → scheduler_init(hhdm) → create demo threads → register timer → scheduler_start()
```

---

### Task 7: Host-side unit tests

**Files:**
- Create: `test/sched/sched_test.cpp`
- Create: `test/sched/BUILD.bazel`
- Modify: `kernel/core/sched/sched.hpp` — ensure RunQueue is testable

- [ ] **Step 1: Write host-side tests**

```cpp
#include <gtest/gtest.h>
#include "kernel/core/sched/sched.hpp"

// RunQueue unit tests — these test the data structure directly,
// no kernel dependencies needed.

TEST(RunQueueTest, Empty) {
    RunQueue rq;
    rq.init();
    EXPECT_EQ(rq.pop(), nullptr);
    EXPECT_EQ(rq.count, 0u);
    EXPECT_EQ(rq.bitmap, 0u);
}

TEST(RunQueueTest, PushPop) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{};
    t1.priority = 1; t2.priority = 1;

    rq.push(&t1);
    rq.push(&t2);
    EXPECT_EQ(rq.count, 2u);

    Thread* a = rq.pop();
    Thread* b = rq.pop();
    EXPECT_TRUE((a == &t1 && b == &t2) || (a == &t2 && b == &t1));
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, PriorityOrder) {
    RunQueue rq;
    rq.init();
    Thread t_low{}, t_mid{}, t_high{};
    t_low.priority = 5; t_mid.priority = 3; t_high.priority = 0;

    rq.push(&t_low);
    rq.push(&t_mid);
    rq.push(&t_high);

    EXPECT_EQ(rq.pop(), &t_high);  // highest priority first
    EXPECT_EQ(rq.pop(), &t_mid);
    EXPECT_EQ(rq.pop(), &t_low);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, Remove) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{}, t3{};
    t1.priority = 2; t2.priority = 2; t3.priority = 2;

    rq.push(&t1);
    rq.push(&t2);
    rq.push(&t3);
    EXPECT_EQ(rq.count, 3u);

    rq.remove(&t2);
    EXPECT_EQ(rq.count, 2u);

    Thread* a = rq.pop();
    Thread* b = rq.pop();
    EXPECT_NE(a, &t2);
    EXPECT_NE(b, &t2);
    EXPECT_EQ(rq.pop(), nullptr);
}

TEST(RunQueueTest, Steal) {
    RunQueue rq;
    rq.init();
    Thread t1{}, t2{};
    t1.priority = 7; t2.priority = 7;

    rq.push(&t1);
    rq.push(&t2);

    Thread* stolen = rq.steal_one();
    EXPECT_NE(stolen, nullptr);
    EXPECT_EQ(rq.count, 1u);
    EXPECT_EQ(rq.pop(), &t1);  // remaining one
}

TEST(ThreadTest, ThreadSize) {
    // Thread must not be too large — we allocate from slab (max 2048 bytes)
    EXPECT_LE(sizeof(Thread), 2048u);
}
```

- [ ] **Step 2: Create test/sched/BUILD.bazel**

```python
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "sched_test",
    size = "small",
    srcs = ["sched_test.cpp"],
    deps = [
        "//kernel/core/sched:sched_hdrs",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
```

- [ ] **Step 3: Add sched_hdrs target in kernel/core/sched/BUILD.bazel**

Add to the existing BUILD.bazel:
```python
cc_library(
    name = "sched_hdrs",
    hdrs = ["sched.hpp"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Run tests**

Run: `bazel test //test/sched:sched_test`
Expected: all 6 tests pass

---

### Task 8: Integration — boot demo and cleanup

**Files:**
- Modify: `kernel/arch/x86_64/boot.cpp` — clean boot sequence, add scheduler demo
- Modify: `kernel/core/sched/sched.cpp` — final polish
- Modify: `CLAUDE.md` — update phase status

- [ ] **Step 1: Update CLAUDE.md**

Change the Phase Plans table entry:
```
| 5: Scheduler | `docs/superpowers/plans/2026-05-05-phase-5-scheduler.md` | ✅ Done |
```

- [ ] **Step 2: Clean boot.cpp — remove old demo code**

Remove the old Phase 3 demo (timer_periodic lambda, serial polling loop, char_count, timer_ticks variables). Replace with clean scheduler boot as described in Task 6.

- [ ] **Step 3: Build and verify**

Run: `bazel build //kernel:kernel && bazel test //test/sched:sched_test //test/mm:all //test/irq:all`
Expected: kernel builds, all tests pass (PMM, buddy, slab, IRQ, sched)

- [ ] **Step 4: QEMU boot verification**

Run: `bash scripts/run.sh`
Expected: kernel boots, scheduler starts, thread-A and thread-B interleave output, system continues running indefinitely

- [ ] **Step 5: Commit**

```bash
git add kernel/core/sched/ test/sched/ kernel/arch/x86_64/smp.hpp kernel/BUILD.bazel kernel/arch/x86_64/boot.cpp CLAUDE.md
git commit -m "Phase 5: Preemptive Scheduler"
```
