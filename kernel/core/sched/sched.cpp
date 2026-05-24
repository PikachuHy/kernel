// kernel/core/sched/sched.cpp
// Scheduler -- thread lifecycle, run-queue, context switch dispatch
//
// Implements Tasks 4-5 of Phase 5:
//   Task 4: thread_create, thread_start, thread_exit, scheduler_schedule,
//           current_thread, thread_yield
//   Task 5: idle_entry, create_idle_thread, scheduler_init, scheduler_start,
//           scheduler_tick

#include "kernel/core/sched/sched.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/core/mm/slab.hpp"
#include "kernel/core/mm/bitmap_alloc.hpp"
#include "kernel/core/mm/buddy.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/core/object/process.hpp"
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"

// ── Per-CPU scheduler state ──────────────────────────────────────────
// Using static arrays avoids adding scheduler fields to the PerCpu struct
// in smp.hpp, which would create a circular dependency with sched.hpp.
// This is simpler and avoids opaque void* casts.
static RunQueue s_run_queues[MAX_CPUS];
static Thread*  s_current_threads[MAX_CPUS];
static Thread*  s_idle_threads[MAX_CPUS];
static bool     g_sched_initialized = false;
static uint32_t g_next_tid = 1;

// Kernel stack top for syscall_entry. Mirrors TSS RSP0, but is a
// global variable accessible from assembly. Updated on every thread
// switch so syscall_entry always has the correct per-thread stack.
// Must be extern "C" and defined here (not in syscall_entry.S) so
// the linker resolves it for both C++ and asm code.
extern "C" uint64_t g_syscall_kstack_top = 0;
static Process* s_kernel_process = nullptr;

// ── Idle thread ──────────────────────────────────────────────────────
// The idle thread runs when no other thread is ready on this CPU.
// It halts the CPU to save power until the next interrupt.

static void idle_entry(void) {
    while (true) {
        asm volatile("sti; hlt; cli");
    }
}

// Create an idle thread for a given CPU.
// Priority = 7 (lowest), tid = 0, state = Running.
// Stack is set up so that scheduler_start can enter idle_entry via
// the same register-restore sequence used by switch_to.
static Thread* create_idle_thread(uint32_t cpu_id) {
    (void)cpu_id;

    Thread* t = static_cast<Thread*>(kmalloc(sizeof(Thread)));
    if (!t) KPANIC("sched: failed to allocate idle Thread");

    // Use a 16KB stack so timer interrupts don't overwrite the initial
    // frame at the top. The ISR pushes ~200 bytes below the IRET frame;
    // with a 4KB stack the initial frame at [top-72..top] is obliterated.
    void* stack_phys = buddy_alloc_pages(2);  // 4 pages = 16KB
    if (!stack_phys) KPANIC("sched: failed to allocate idle stack");

    uint64_t stack_virt = DIRECT_MAP_BASE + reinterpret_cast<uint64_t>(stack_phys);
    uint64_t stack_top  = stack_virt + PAGE_SIZE * 4;

    // ── Initial stack frame layout ──
    // From t->rsp (r15 slot) upward:
    //   [rsp+0]   r15            (zeroed by bitmap_alloc_page)
    //   [rsp+8]   r14            (zeroed)
    //   [rsp+16]  r13            (zeroed)
    //   [rsp+24]  r12            (zeroed)
    //   [rsp+32]  rbp            (zeroed)
    //   [rsp+40]  rbx            (zeroed)
    //   [rsp+48]  RFLAGS = 0x202 (IF set, interrupts enabled)
    //   [rsp+56]  entry()        <- switch_to / scheduler_start rets here
    //   [rsp+64]  thread_exit    <- entry() rets here (idle never returns)
    //
    // Total overhead: 9 slots * 8 = 72 bytes above t->rsp.

    // Place the initial frame 4KB below the stack top. This reserves
    // the top 4KB for interrupt frames (~200 bytes ISR + IRET frame),
    // preventing the saved context from being overwritten.
    uint64_t frame_base_virt = stack_top - PAGE_SIZE;  // 12KB from bottom
    uint64_t frame_base_phys = reinterpret_cast<uint64_t>(stack_phys) + PAGE_SIZE * 3;

    // rsp stored as physical offset (scheduler_start / thread_start convert).
    // Frame layout (low to high from t->rsp):
    //   [rsp+0]   r15            (zeroed by alloc)
    //   [rsp+8]   r14            (zeroed)
    //   [rsp+16]  r13            (zeroed)
    //   [rsp+24]  r12            (zeroed)
    //   [rsp+32]  rbp            (zeroed)
    //   [rsp+40]  rbx            (zeroed)
    //   [rsp+48]  RFLAGS = 0x202 (IF=1)
    //   [rsp+56]  idle_entry     <- switch_to/scheduler_start rets here
    //   [rsp+64]  thread_exit    <- fallback return
    t->rsp = frame_base_phys;

    *reinterpret_cast<uint64_t*>(frame_base_virt + 56) =
        reinterpret_cast<uint64_t>(idle_entry);
    *reinterpret_cast<uint64_t*>(frame_base_virt + 64) =
        reinterpret_cast<uint64_t>(thread_exit);
    *reinterpret_cast<uint64_t*>(frame_base_virt + 48) = 0x202ULL;  // IF=1
    t->rflags        = 0x202;
    t->state         = ThreadState::Running;
    t->priority      = 7;          // lowest priority
    t->time_slice_ms = SCHED_DEFAULT_TIMESLICE_MS;
    t->tid           = 0;
    t->name[0]       = 'i';
    t->name[1]       = 'd';
    t->name[2]       = 'l';
    t->name[3]       = 'e';
    t->name[4]       = '\0';
    t->next          = nullptr;
    t->stack_bottom  = stack_phys;
    t->stack_size    = PAGE_SIZE;
    t->process       = nullptr;  // idle threads have no process
    t->proc_next     = nullptr;

    // Idle thread also gets a kernel stack for timer interrupts.
    void* idle_kstack = buddy_alloc_pages(2);  // 16KB
    if (!idle_kstack) KPANIC("sched: failed to allocate idle kernel stack");
    t->kernel_stack  = reinterpret_cast<uint64_t>(idle_kstack);

    return t;
}

// ── Scheduler initialization ─────────────────────────────────────────
void scheduler_init(uint64_t hhdm) {
    g_hhdm = hhdm;

    klog("scheduler: initializing");
    if (g_cpu_count > 1) {
        klog(" for ");
        klog_hex(g_cpu_count);
        klog(" CPUs");
    }
    klog("...\n");

    for (uint32_t i = 0; i < g_cpu_count && i < MAX_CPUS; i++) {
        s_run_queues[i].init();

        Thread* idle = create_idle_thread(i);
        s_idle_threads[i]   = idle;
        s_current_threads[i] = idle;
    }

    // Create kernel process -- owns all kernel threads
    s_kernel_process = Process::Create("kernel", true);
    if (!s_kernel_process) KPANIC("sched: failed to create kernel process");

    // Set as fallback for backward-compat handle_alloc/free/lookup
    handle_table_set_fallback(&s_kernel_process->handles);

    g_sched_initialized = true;
    klog("scheduler: ready\n");
}

// ── Scheduler start ──────────────────────────────────────────────────
// Transitions from the boot-time single thread of execution into the
// scheduler's context: pops the first thread from the run queue and
// jumps to its entry function via the same register-restore sequence
// used by switch_to (pop r15..rbx, popfq, ret).
// Never returns.
void scheduler_start() {
    klog("scheduler: starting...\n");

    // Pick the first thread from CPU 0's run queue
    Thread* first = s_run_queues[0].pop();
    if (!first) {
        first = s_idle_threads[0];
        klog("scheduler: no threads, entering idle\n");
    }

    first->state = ThreadState::Running;
    s_current_threads[0] = first;

    klog("scheduler: running thread '");
    klog(first->name);
    klog("'\n");

    // Convert rsp from physical to virtual if not already done
    if (first->rsp < g_hhdm) {
        first->rsp += g_hhdm;
    }

    // If the first thread belongs to a process, load its page tables.
    if (first->process && first->process->pml4_phys) {
        uint64_t cr3 = first->process->pml4_phys;
        asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    }

    // If the thread has a kernel stack, set TSS RSP0.
    if (first->kernel_stack) {
        uint64_t rsp0 = DIRECT_MAP_BASE + first->kernel_stack + PAGE_SIZE * 4;
        tss_set_rsp0(rsp0);
        g_syscall_kstack_top = rsp0;
    }

    // Load the thread's saved stack pointer and restore all callee-saved
    // registers + RFLAGS, then ret into the thread's entry function.
    // This matches the switch_to restore sequence exactly.
    asm volatile(
        "movq %0, %%rsp\n"
        "popq %%r15\n"
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%rbp\n"
        "popq %%rbx\n"
        "popfq\n"
        "ret\n"
        :
        : "r"(first->rsp)
        : "memory"
    );

    __builtin_unreachable();
}

// ── Timer tick (preemption) ─────────────────────────────────────────
void scheduler_tick() {
    Thread* cur = s_current_threads[0];
    if (!cur) return;

    // Only tick non-idle threads (idle thread doesn't accumulate ticks)
    if (cur == s_idle_threads[0]) return;

    cur->time_slice_ms--;

    if (cur->time_slice_ms <= 0) {
        cur->time_slice_ms = SCHED_DEFAULT_TIMESLICE_MS;
        scheduler_schedule();
    }
}

// ── Scheduler core: pick next thread, context switch ─────────────────
void scheduler_schedule() {
    Thread* prev = s_current_threads[0];

    // Push prev back to run queue if it is still runnable and not idle.
    // Blocked and Dead threads are NOT re-enqueued — they wait off-queue
    // for a wakeup (thread_wake sets state→Ready and pushes).
    if (prev && prev != s_idle_threads[0] &&
        prev->state != ThreadState::Dead &&
        prev->state != ThreadState::Blocked &&
        prev->state != ThreadState::Sleeping) {
        if (prev->state == ThreadState::Running) {
            prev->state = ThreadState::Ready;
        }
        s_run_queues[0].push(prev);
    }

    // Pick the next ready thread from CPU 0's run queue
    Thread* next = s_run_queues[0].pop();
    if (!next) {
        // Nothing ready -- fall back to idle thread
        next = s_idle_threads[0];
    }

    // Convert rsp from physical to virtual if needed.
    if (next->rsp < g_hhdm) {
        next->rsp += g_hhdm;
    }

    if (next == prev) return;

    // Mark next as Running
    next->state = ThreadState::Running;

    // Update the current thread pointer
    s_current_threads[0] = next;

    // Reload CR3 if crossing process boundary
    if (prev && next->process != prev->process) {
        uint64_t new_cr3 = next->process
            ? next->process->pml4_phys
            : paging_kernel_pml4_template();
        if (new_cr3) {
            asm volatile("mov %0, %%cr3" :: "r"(new_cr3) : "memory");
        }
    }

    // Update TSS RSP0 if the next thread has its own kernel stack.
    if (next->kernel_stack) {
        uint64_t rsp0 = DIRECT_MAP_BASE + next->kernel_stack + PAGE_SIZE * 4;
        tss_set_rsp0(rsp0);
        g_syscall_kstack_top = rsp0;
    }

    // Perform the context switch
    switch_to(prev, next);
}

// ── Thread creation ──────────────────────────────────────────────────
Thread* thread_create(void (*entry)(), const char* name, uint8_t priority,
                      Process* process) {
    if (!g_sched_initialized) return nullptr;

    Thread* t = static_cast<Thread*>(kmalloc(sizeof(Thread)));
    if (!t) return nullptr;

    void* stack_phys = bitmap_alloc_page();
    if (!stack_phys) {
        kfree(t);
        return nullptr;
    }

    // Allocate a 16KB kernel interrupt stack for ring-3 → ring-0 transitions.
    // buddy_alloc_pages(2) = 4 pages = 16KB.
    void* kstack_phys = buddy_alloc_pages(2);
    if (!kstack_phys) {
        bitmap_free_page(stack_phys);
        kfree(t);
        return nullptr;
    }

    // Write initial stack frame at the virtual address
    uint64_t stack_virt = g_hhdm + reinterpret_cast<uint64_t>(stack_phys);
    uint64_t stack_top  = stack_virt + PAGE_SIZE;

    // thread_exit: what entry() returns to
    *reinterpret_cast<uint64_t*>(stack_top - 8) =
        reinterpret_cast<uint64_t>(thread_exit);
    // entry: what switch_to/scheduler_start rets to
    *reinterpret_cast<uint64_t*>(stack_top - 16) =
        reinterpret_cast<uint64_t>(entry);
    // RFLAGS with IF set (interrupts enabled)
    *reinterpret_cast<uint64_t*>(stack_top - 24) = 0x202ULL;

    // rsp stored as physical offset (converted to virtual in thread_start)
    t->rsp           = reinterpret_cast<uint64_t>(stack_phys) + PAGE_SIZE - 72;
    t->rflags        = 0x202;
    t->state         = ThreadState::Ready;
    t->priority      = priority & 7;
    t->time_slice_ms = SCHED_DEFAULT_TIMESLICE_MS;
    t->tid           = g_next_tid++;

    // Copy name (up to MAX_THREAD_NAME - 1 chars + null terminator)
    {
        int i = 0;
        while (i < MAX_THREAD_NAME - 1 && name[i]) {
            t->name[i] = name[i];
            i++;
        }
        t->name[i] = '\0';
    }

    t->next          = nullptr;
    t->stack_bottom  = stack_phys;
    t->stack_size    = PAGE_SIZE;

    t->kernel_stack  = reinterpret_cast<uint64_t>(kstack_phys);

    t->process   = process;
    t->proc_next = nullptr;

    if (process) {
        process->AddThread(t);
    }

    return t;
}

// ── Thread start ─────────────────────────────────────────────────────
void thread_start(Thread* t) {
    if (!t) return;

    // Convert rsp from physical offset to virtual address
    t->rsp += g_hhdm;

    // Mark as Ready (should already be Ready from thread_create)
    t->state = ThreadState::Ready;

    // Add to CPU 0's run queue
    s_run_queues[0].push(t);

    klog("scheduler: thread '");
    klog(t->name);
    klog("' tid=");
    klog_hex(t->tid);
    klog(" started\n");
}

// ── Thread yield (voluntary reschedule) ──────────────────────────────
void thread_yield() {
    // Mark current thread as Ready so scheduler_schedule will re-enqueue it
    Thread* cur = s_current_threads[0];
    if (cur && cur->state == ThreadState::Running) {
        cur->state = ThreadState::Ready;
    }

    scheduler_schedule();
}

// ── Thread exit ──────────────────────────────────────────────────────
[[noreturn]] void thread_exit() {
    Thread* cur = s_current_threads[0];
    if (cur) {
        cur->state = ThreadState::Dead;

        klog("scheduler: thread '");
        klog(cur->name);
        klog("' tid=");
        klog_hex(cur->tid);
        klog(" exited\n");
    }

    scheduler_schedule();

    // scheduler_schedule should never return -- the Dead thread is not
    // re-enqueued, and the idle thread runs instead. If we reach here
    // something is very wrong.
    KPANIC("scheduler_schedule returned in thread_exit -- dead thread "
           "still running");
}

// ── Current thread accessor ──────────────────────────────────────────
Thread* current_thread() {
    return s_current_threads[0];
}

Thread* get_idle_thread() {
    return s_idle_threads[0];
}
