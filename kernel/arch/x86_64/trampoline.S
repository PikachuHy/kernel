# kernel/arch/x86_64/trampoline.S
#
# AP startup trampoline: transitions from 16-bit real mode to 64-bit long
# mode.  Copied to a physical page below 1 MiB by smp_init(), after which
# APs are started via SIPI(vector).  Fully position-independent — works at
# any physical address.
#
# Flow:
#   16-bit real mode (entry after SIPI)
#     -> load GDT (base patched by smp_init)
#     -> enter protected mode (CR0.PE)
#     -> 32-bit protected mode
#        -> enable PAE (CR4.PAE)
#        -> load shared kernel page tables (CR3)
#        -> enable long mode (EFER.LME)
#        -> enable paging (CR0.PG)
#     -> 64-bit long mode
#        -> call smp_ap_entry(cpu_id | (lapic_id << 32))

.section .trampoline, "ax"

.globl trampoline_start
.globl trampoline_end
.globl tr_cr3
.globl tr_entry
.globl tr_id
.globl tr_gdtr

.code16

# ── Trampoline data ──────────────────────────────────────────────────
# These fields sit at a fixed offset from the page start so that both
# 16-bit real-mode code (base-relative via CS) and 64-bit code
# (RIP-relative) can locate them.
trampoline_start:
    jmp real_start

.align 8
tr_cr3:   .quad 0    # physical address of top-level page table
tr_entry: .quad 0    # smp_ap_entry function pointer (64-bit)
tr_id:    .quad 0    # arg: low32 = cpu_id, high32 = lapic_id

.align 16
tr_gdt:
tr_gdt_null:   .quad 0
tr_gdt_code16: .quad 0x00009a000000ffff   # 16-bit code,  base=0 limit=4G
tr_gdt_data16: .quad 0x000092000000ffff   # 16-bit data,  base=0 limit=4G
tr_gdt_code32: .quad 0x00cf9a000000ffff   # 32-bit code,  D=1
tr_gdt_data32: .quad 0x00cf92000000ffff   # 32-bit data,  D=1
tr_gdt_code64: .quad 0x00209a0000000000   # 64-bit code,  L=1 D=0
tr_gdt_data64: .quad 0x0000920000000000   # 64-bit data
tr_gdt_end:

.align 4
tr_gdtr:
    .word tr_gdt_end - tr_gdt - 1    # GDT limit
    .long 0                           # GDT base (patched by smp_init)

# ── 16-bit real mode entry ──────────────────────────────────────────
real_start:
    cli
    cld

    # SIPI sets CS = vector * 0x100, giving physical base = vector * 0x1000.
    # Copy CS to DS/ES/SS so that data segment addressing is consistent.
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    # Compute physical base address from CS: base = CS << 4.
    movw %cs, %ax
    movzwl %ax, %eax
    shll $4, %eax           # eax = physical base of trampoline page

    # Load GDT pseudo-descriptor (base patched by smp_init).
    # Use PIC via computed base to avoid absolute 16-bit relocation.
    movl %eax, %edx
    addl $(tr_gdtr - trampoline_start), %edx
    lgdtl (%edx)

    # Enter protected mode: set CR0.PE (bit 0).
    movl %cr0, %ecx
    orl  $1, %ecx
    movl %ecx, %cr0

    # Far jump to 32-bit code using computed runtime address.
    # eax still holds the trampoline page base from above.
    addl $(tmp_protected - trampoline_start), %eax   # eax = &tmp_protected
    pushw $0x18          # 32-bit code selector (tr_gdt_code32, index 3)
    pushl %eax           # 32-bit offset
    lretl

# ── 32-bit protected mode ───────────────────────────────────────────
.code32
tmp_protected:
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Temporary stack below 1 MiB (safe; smp_init reserves this area).
    movl $0x8000, %esp

    # Enable PAE: CR4.PAE = bit 5.
    movl %cr4, %eax
    orl  $(1 << 5), %eax
    movl %eax, %cr4

    # Get the runtime base address of the trampoline page (PIC).
    call 1f
1:  popl %ebx
    subl $(1b - trampoline_start), %ebx   # ebx = &trampoline_start

    # Load shared page table root into CR3.
    movl tr_cr3 - trampoline_start(%ebx), %eax
    movl %eax, %cr3

    # Enable long mode: EFER.LME = bit 8 (MSR 0xC0000080).
    movl $0xC0000080, %ecx
    rdmsr
    orl  $(1 << 8), %eax
    wrmsr

    # Enable paging: CR0.PG = bit 31.
    # With PAE + LME already set, the processor enters IA-32e
    # compatibility mode; the far jump below transitions to 64-bit mode.
    movl %cr0, %eax
    orl  $(1 << 31), %eax
    movl %eax, %cr0

    # Compute runtime address of tmp_long and far-jump to 64-bit mode.
    leal tmp_long - trampoline_start(%ebx), %ecx   # ecx = &tmp_long
    pushw $0x28          # 64-bit code selector (tr_gdt_code64, index 5)
    pushl %ecx
    lretl

# ── 64-bit long mode ────────────────────────────────────────────────
.code64
tmp_long:
    movw $0x30, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    # Call smp_ap_entry(cpu_id | (lapic_id << 32)).
    # tr_id is filled by smp_init before SIPI.
    movq tr_entry(%rip), %rax
    movq tr_id(%rip), %rdi
    callq *%rax

    # smp_ap_entry should never return.
.Lhalt:
    hlt
    jmp .Lhalt

trampoline_end:
