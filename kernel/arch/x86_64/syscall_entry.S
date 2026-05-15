.globl syscall_entry
syscall_entry:
    # Save user RSP and switch to a kernel temp stack.
    movq %rsp, user_rsp_save(%rip)
    leaq temp_kstack_top(%rip), %rsp

    # Save user context. Push order must match pop order for sysretq.
    # syscall saved user RIP in RCX and user RFLAGS in R11.
    pushq %rcx             # user return RIP (goes to RCX for sysretq)
    pushq %r11             # user RFLAGS (goes to R11 for sysretq)
    pushq %r10             # user r10 (arg4)
    pushq %rdx             # user rdx (arg3)
    pushq %rsi             # user rsi (arg2)
    pushq %rdi             # user rdi (arg1)
    pushq %rax             # syscall number

    # Call dispatcher: syscall_dispatcher(num, a1, a2, a3, a4)
    # Arguments in: rdi, rsi, rdx, rcx, r8
    movq 0(%rsp), %rdi     # num = saved rax
    movq 8(%rsp), %rsi     # a1 = saved rdi
    movq 16(%rsp), %rdx    # a2 = saved rsi
    movq 24(%rsp), %rcx    # a3 = saved rdx
    movq 32(%rsp), %r8     # a4 = saved r10
    callq syscall_dispatcher

    # Restore context. Skip over rax, rdi, rsi, rdx, r10 (5*8=40 bytes).
    # RSP then points at r11 (user RFLAGS), then rcx (user return RIP).
    addq $40, %rsp
    popq %r11              # user RFLAGS
    popq %rcx              # user return RIP

    # Restore user RSP and return to ring 3.
    movq user_rsp_save(%rip), %rsp
    sysretq

.section .bss
.align 16
user_rsp_save:
    .space 8
temp_kstack:
    .space 16384
temp_kstack_top:
.text
