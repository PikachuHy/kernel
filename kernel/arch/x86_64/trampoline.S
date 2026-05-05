# kernel/arch/x86_64/trampoline.S
#
# AP startup trampoline: transitions from 16-bit real mode to 64-bit long
# mode.  Copied to a physical page below 1 MiB by smp_init().  Fully
# position-independent.
#
# Flow:
#   16-bit real mode (entry after SIPI, CS = vector * 0x100)
#     -> load GDT (GDTR patched by smp_init)
#     -> enter protected mode (CR0.PE)
#     -> 32-bit protected mode
#        -> enable PAE (CR4.PAE)
#        -> load shared kernel page tables (CR3, patched by smp_init)
#        -> enable long mode + NXE (EFER.LME | EFER.NXE)
#        -> enable paging (CR0.PG)
#     -> 64-bit long mode
#        -> call smp_ap_entry(cpu_id | (lapic_id << 32))

.section .trampoline, "ax"

.globl trampoline_start
.globl trampoline_end
.globl tr_cr3
.globl tr_entry
.globl tr_id
.globl tr_gdt
.globl tr_gdtr

.code16

# ── Trampoline data (patched by smp_init at runtime) ──
trampoline_start:
    jmp real_start

.align 8
tr_cr3:   .quad 0               # CR3 value
tr_entry: .quad 0               # smp_ap_entry function pointer
tr_id:    .quad 0               # cpu_id (low 32) | lapic_id (high 32)

.align 16
tr_gdt:
tr_gdt_null:   .quad 0
tr_gdt_code16: .quad 0x00009a000000ffff
tr_gdt_data16: .quad 0x000092000000ffff
tr_gdt_code32: .quad 0x00cf9a000000ffff
tr_gdt_data32: .quad 0x00cf92000000ffff
tr_gdt_code64: .quad 0x00209a0000000000
tr_gdt_data64: .quad 0x0000920000000000
tr_gdt_end:

.align 4
tr_gdtr:
    .word tr_gdt_end - tr_gdt - 1   # limit = 55
    .long 0                          # base (patched by smp_init)

# ── 16-bit real mode entry ──────────────────────────────────────────
real_start:
    cli
    cld

    # Compute physical base from CS (CS << 4).
    movw %cs, %ax
    movzwl %ax, %eax
    shll $4, %eax                # eax = physical base
    movl %eax, %ebp              # ebp = physical base (saved)

    # Copy CS to DS/ES/SS for consistent segment addressing.
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    # Load GDT descriptor.
    # The GDTR is at trampoline_base + tr_gdtr_offset.
    # lgdtl uses DS segment, so we must zero DS to get flat physical
    # addressing (DS has base = CS base = trampoline_base set above).
    movl %ebp, %edx
    addl $(tr_gdtr - trampoline_start), %edx
    xorw %ax, %ax
    movw %ax, %ds                # DS = 0 — absolute physical addressing
    lgdtl (%edx)

    # Restore DS for stack operations below.
    movw %cs, %ax
    movw %ax, %ds

    # Set stack pointer to a safe area near top of 64K segment.
    movw $0xFFF0, %sp

    # Enter protected mode.
    movl %cr0, %ecx
    orl  $1, %ecx
    movl %ecx, %cr0

    # Far jump to 32-bit code via computed physical address.
    movl %ebp, %eax
    addl $(tmp_protected - trampoline_start), %eax
    pushw $0x18                 # 32-bit code selector (index 3)
    pushl %eax                  # 32-bit offset
    lretl

# ── 32-bit protected mode ───────────────────────────────────────────
.code32
tmp_protected:
    movw $0x20, %ax             # 32-bit data selector (index 4)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x8000, %esp          # temporary stack below 1 MiB

    # Enable PAE.
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    # Get runtime base (PIC in 32-bit mode).
    call 1f
1:  popl %ebx
    subl $(1b - trampoline_start), %ebx

    # Load shared kernel page tables.
    movl tr_cr3 - trampoline_start(%ebx), %eax
    movl %eax, %cr3

    # Enable long mode and NXE.
    # NXE is required: Limine page tables mark data pages as NX.
    movl $0xC0000080, %ecx
    rdmsr
    orl  $((1 << 8) | (1 << 11)), %eax
    wrmsr

    # Enable paging — enters IA-32e compatibility mode.
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    # Far jump to 64-bit mode.
    leal tmp_long - trampoline_start(%ebx), %ecx
    pushw $0x28                 # 64-bit code selector (index 5)
    pushl %ecx
    lretl

# ── 64-bit long mode ────────────────────────────────────────────────
.code64
tmp_long:
    movw $0x30, %ax             # 64-bit data selector (index 6)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Call smp_ap_entry(cpu_id | (lapic_id << 32)).
    movq tr_entry(%rip), %rax
    movq tr_id(%rip), %rdi
    callq *%rax

1:  hlt
    jmp 1b

trampoline_end:
