.globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:0
    leaq temp_kstack_top(%rip), %rsp

    pushq %rcx
    pushq %r11
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax

    movq %rax, %rdi
    popq %rax
    pushq %rax
    movq 0(%rsp), %rdi
    movq 8(%rsp), %rsi
    movq 16(%rsp), %rdx
    movq 24(%rsp), %rcx
    movq 32(%rsp), %r8
    callq syscall_dispatcher

    addq $40, %rsp
    popq %r11
    popq %rcx

    movq %gs:0, %rsp
    swapgs
    sysretq

.section .bss
.align 16
temp_kstack:
    .space 16384
temp_kstack_top:
.text
