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
        int p = t->priority & 7;
        t->next = heads[p];
        heads[p] = t;
        bitmap |= (1u << p);
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
void scheduler_init(uint64_t hhdm);
void scheduler_start();       // enables interrupts, never returns
void scheduler_tick();        // called from timer ISR
void scheduler_schedule();    // pick next, context switch

// Current thread accessor
Thread* current_thread();

// Idle thread (per-CPU)
Thread* get_idle_thread();

// ── Assembly: context switch ────────────────────────────────
// Saves callee-saved regs to prev->rsp chain, loads next->rsp chain.
extern "C" void switch_to(Thread* prev, Thread* next);
