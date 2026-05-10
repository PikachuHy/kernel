# kernel/core/elf_trampoline.S
# Trampoline: kernel thread entry -> ring-3 user mode
# r15 and r14 are overloaded from the thread's saved frame:
#   r15 = user-space entry RIP
#   r14 = user-space initial RSP

.globl elf_trampoline
elf_trampoline:
    movq %r15, %rdi       # first arg: entry_rip
    movq %r14, %rsi       # second arg: user_rsp
    call enter_usermode
    # enter_usermode never returns
    ud2                   # crash if it somehow does
