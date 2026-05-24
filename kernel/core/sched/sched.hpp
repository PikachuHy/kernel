#pragma once
#include <stdint.h>
#include <stddef.h>

class Process;

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

    // Kernel interrupt stack (for ring-3 → ring-0 transitions)
    uint64_t  kernel_stack;  // physical address of kernel stack (16KB)

    Process*   process;      // owning process (nullptr = idle thread / no process)
    Thread*    proc_next;    // next in process->threads list
};

static_assert(offsetof(Thread, rsp) == 0,
    "rsp must be at offset 0 for switch_to assembly");

// ── RunQueue ────────────────────────────────────────────────

struct RunQueue {
    Thread*  heads[SCHED_PRIORITIES];  // linked list per priority
    uint8_t  bitmap;                    // bit i set if heads[i] != nullptr
    uint32_t count;                     // total threads in queue

    auto init() noexcept -> void {
        for (int i = 0; i < SCHED_PRIORITIES; i++) heads[i] = nullptr;
        bitmap = 0; count = 0;
    }

    // Append to tail: FIFO per priority for round-robin fairness.
    auto push(Thread* t) noexcept -> void {
        int p = t->priority & 7;
        t->next = nullptr;
        if (!heads[p]) {
            heads[p] = t;
        } else {
            Thread* cur = heads[p];
            while (cur->next) cur = cur->next;
            cur->next = t;
        }
        bitmap |= (1u << p);
        count++;
    }

    auto pop() noexcept -> Thread* {
        if (bitmap == 0) return nullptr;
        // Find highest-priority non-empty queue
        int prio = __builtin_ctz(bitmap);
        Thread* t = heads[prio];
        heads[prio] = t->next;
        if (!heads[prio]) bitmap &= ~(1u << prio);
        count--;
        return t;
    }

    auto remove(Thread* target) noexcept -> void {
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
    auto steal_one() noexcept -> Thread* {
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
auto thread_create(void (*entry)(), const char* name, uint8_t priority,
                   Process* process = nullptr) -> Thread*;
auto thread_start(Thread* t) -> void;
auto thread_yield() -> void;
[[noreturn]] auto thread_exit() -> void;

// Scheduler lifecycle
auto scheduler_init(uint64_t hhdm) -> void;
auto scheduler_start() -> void;       // enables interrupts, never returns
auto scheduler_tick() -> void;        // called from timer ISR
auto scheduler_schedule() -> void;    // pick next, context switch

// Current thread accessor
auto current_thread() -> Thread*;

// Idle thread (per-CPU)
auto get_idle_thread() -> Thread*;

// ── Assembly: context switch ────────────────────────────────
// Saves callee-saved regs to prev->rsp chain, loads next->rsp chain.
extern "C" auto switch_to(Thread* prev, Thread* next) -> void;
