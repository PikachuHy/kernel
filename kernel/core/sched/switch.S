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
