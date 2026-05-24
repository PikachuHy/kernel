.section .bss
.align 16
saved_rcx:
    .space 8

.section .text
.globl syscall_entry
syscall_entry:
    # ── Registers on entry ───────────────────────────────────────
    # RSP = user stack pointer
    # RCX = user RIP (saved by syscall)
    # R11 = user RFLAGS (saved by syscall, SFMASK applied)
    # RAX = syscall number
    #
    # syscall preserves: RBP, RBX, R8-R15
    # syscall clobbers:  RCX (=user RIP), R11 (=user RFLAGS)
    #
    # IMPORTANT: Kernel C code (System V ABI) may use R8, R9, etc.
    # as scratch (caller-saved) registers.  Since syscall preserves
    # them, user code may rely on them being unchanged after sysretq.
    # We must save/restore them here to prevent kernel leakage.

    # Save user RIP (need RCX to hold user RSP)
    movq %rcx, saved_rcx(%rip)

    # Switch to per-thread kernel stack
    movq %rsp, %rcx
    leaq g_syscall_kstack_top(%rip), %rsp
    movq (%rsp), %rsp

    # ── Push user context for dispatcher ─────────────────────────
    pushq %rcx              # user RSP
    movq saved_rcx(%rip), %rcx
    pushq %rcx              # user RIP
    pushq %r11              # user RFLAGS
    pushq %r10              # arg4
    pushq %rdx              # arg3
    pushq %rsi              # arg2
    pushq %rdi              # arg1
    pushq %rax              # syscall number

    # ── Save registers (pushed last = popped first on return) ────
    # These are regs that syscall preserves but kernel C may clobber.
    pushq %r9
    pushq %r8
    pushq %rbx
    pushq %rbp

    # ── Dispatch ──────────────────────────────────────────────────
    # Stack layout at this point:
    #   [rsp]    = rbp     (saved reg, 4th)
    #   [rsp+8]  = rbx     (saved reg, 3rd)
    #   [rsp+16] = r8      (saved reg, 2nd)
    #   [rsp+24] = r9      (saved reg, 1st)
    #   [rsp+32] = rax     (num)
    #   [rsp+40] = rdi     (a1)
    #   [rsp+48] = rsi     (a2)
    #   [rsp+56] = rdx     (a3)
    #   [rsp+64] = r10     (a4)
    #   [rsp+72] = r11     (user RFLAGS)
    #   [rsp+80] = user RIP
    #   [rsp+88] = user RSP
    movq 32(%rsp), %rdi     # num
    movq 40(%rsp), %rsi     # a1 (saved rdi)
    movq 48(%rsp), %rdx     # a2 (saved rsi)
    movq 56(%rsp), %rcx     # a3 (saved rdx)
    movq 64(%rsp), %r8      # a4 (saved r10)
    callq syscall_dispatcher

    # ── Return: restore saved regs ───────────────────────────────
    popq %rbp
    popq %rbx
    popq %r8
    popq %r9

    # Skip rax, rdi, rsi, rdx, r10 (5 × 8 = 40)
    addq $40, %rsp
    popq %r11               # user RFLAGS → r11
    cli
    popq %rcx               # user RIP → rcx
    movq (%rsp), %rsp       # restore user RSP
    sysretq
