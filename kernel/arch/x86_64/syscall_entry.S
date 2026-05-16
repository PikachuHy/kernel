.globl syscall_entry
syscall_entry:
    # On entry: RSP = user stack pointer (syscall does NOT change RSP)
    #           RCX = user RIP (saved by syscall)
    #           R11 = user RFLAGS (saved by syscall, with SFMASK applied)
    #           RAX = syscall number (set by user code)
    #
    # We must save user RSP before switching to kernel stack.
    # Use RCX as temporary storage for user RSP (after saving user RIP).

    # Save user RIP to a temporary location (we need RCX free)
    movq %rcx, saved_rcx(%rip)

    # Save user RSP into RCX
    movq %rsp, %rcx

    # Load per-thread kernel stack top (mirrors TSS.RSP0, updated on thread switch)
    leaq g_syscall_kstack_top(%rip), %rsp
    movq (%rsp), %rsp

    # Now on the kernel stack. Push user context.
    # Layout (bottom to top, growing down):
    #   user RSP       <- pushed first
    #   user RIP       <- pushed second
    #   user RFLAGS    <- pushed third
    #   r10            <- pushed fourth
    #   rdx            <- pushed fifth
    #   rsi            <- pushed sixth
    #   rdi            <- pushed seventh (arg1)
    #   rax            <- pushed eighth (num), RSP here after pushes

    pushq %rcx              # user RSP (was in rcx)
    movq saved_rcx(%rip), %rcx   # restore user RIP
    pushq %rcx              # user RIP
    pushq %r11              # user RFLAGS
    pushq %r10              # user r10 (arg4)
    pushq %rdx              # user rdx (arg3)
    pushq %rsi              # user rsi (arg2)
    pushq %rdi              # user rdi (arg1)
    pushq %rax              # syscall number

    # Call dispatcher: syscall_dispatcher(num, a1, a2, a3, a4)
    # Arguments picked from the stack frame.
    movq 0(%rsp), %rdi      # num = saved rax
    movq 8(%rsp), %rsi      # a1 = saved rdi
    movq 16(%rsp), %rdx     # a2 = saved rsi
    movq 24(%rsp), %rcx     # a3 = saved rdx
    movq 32(%rsp), %r8      # a4 = saved r10
    callq syscall_dispatcher

    # Restore context.
    addq $40, %rsp          # skip over rax, rdi, rsi, rdx, r10 (5 × 8)
    popq %r11               # user RFLAGS → r11 (for sysretq)
    popq %rcx               # user RIP → rcx (for sysretq)
    # RSP now points at user RSP slot on the kernel stack.
    # Load it: (%rsp) computes address using OLD rsp, then writes to rsp.
    movq (%rsp), %rsp       # restore user RSP
    sysretq                 # return to ring 3 (RIP=rcx, RFLAGS=r11)

.section .bss
.align 16
saved_rcx:
    .space 8
.text
