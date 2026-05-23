	.macro IRQ_STUB n
	.globl irq_stub_\n
	irq_stub_\n:
	    pushq $0; pushq $\n
	    pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx
	    pushq %rsi; pushq %rdi; pushq %rbp
	    pushq %r8; pushq %r9; pushq %r10; pushq %r11
	    pushq %r12; pushq %r13; pushq %r14; pushq %r15
	    movq $\n, %rdi
	    callq irq_dispatch
	    cli                     # IF=1 after switch_to popfq; prevent nested IRQ during cleanup
	    popq %r15; popq %r14; popq %r13; popq %r12
	    popq %r11; popq %r10; popq %r9; popq %r8
	    popq %rbp; popq %rdi; popq %rsi; popq %rdx
	    popq %rcx; popq %rbx; popq %rax
	    addq $16, %rsp
	    // Force-correct CS/SS in iretq frame.
	    // Context-switch or IST frame overwrite can leave stale
	    // selectors; matching isr_common defense.
	    movq 8(%rsp), %rdi       // saved CS in frame
	    testb $3, %dil           // RPL=3 → ring-3 return
	    jz 1f
	    movq $0x1B, 8(%rsp)      // force ring-3 CS
	    movq $0x13, 32(%rsp)     // force ring-3 SS
	    jmp 2f
	1:	movq $0x08, 8(%rsp)      // force ring-0 CS
	2:	iretq
	.endm

	IRQ_STUB 32; IRQ_STUB 33; IRQ_STUB 34; IRQ_STUB 35
	IRQ_STUB 36; IRQ_STUB 37; IRQ_STUB 38; IRQ_STUB 39
	IRQ_STUB 40; IRQ_STUB 41; IRQ_STUB 42; IRQ_STUB 43
	IRQ_STUB 44; IRQ_STUB 45; IRQ_STUB 46; IRQ_STUB 47
