# kernel/arch/x86_64/usermode.S
# Ring 0 → Ring 3 transition via iretq

.globl enter_usermode
enter_usermode:
    # rdi = entry_rip (user-space instruction pointer)
    # rsi = user_rsp  (top of user stack)

    cli                  # Block IRQs during ring transition

    # Switch to user data segments
    movw $0x13, %ax      # user data selector (GDT index 4 | RPL 3)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    # Build IRET frame on the stack
    # iretq pops: RIP, CS, RFLAGS, RSP, SS (in that order)
    pushq $0x13          # SS = user data selector
    pushq %rsi           # RSP = user stack pointer
    pushq $0x202         # RFLAGS with IF=1 (not pushfq, which has IF=0 due to cli)
    pushq $0x1B          # CS = user code selector (GDT index 3 | RPL 3)
    pushq %rdi           # RIP = entry point

    iretq
