#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/lib/klog.hpp"
#include "kernel/lib/panic.hpp"
#include <stdint.h>

struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IDTR {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

namespace {

constexpr int IDT_ENTRIES = 256;
IDTEntry idt[IDT_ENTRIES];
IDTR idtr;

struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags;
};

const char* exception_names[] = {
    "#DE Division Error",
    "#DB Debug",
    "#NMI Non-maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 Floating-Point Exception",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point Exception",
    "#VE Virtualization Exception",
    "#CP Control Protection Exception",
};

void set_entry(int idx, uint64_t handler, uint8_t ist, uint8_t type_attr) {
    idt[idx].offset_low = handler & 0xFFFF;
    idt[idx].offset_mid = (handler >> 16) & 0xFFFF;
    idt[idx].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[idx].selector = 0x08;
    idt[idx].ist = ist;
    idt[idx].type_attr = type_attr;
    idt[idx].reserved = 0;
}

extern "C" void exception_handler(InterruptFrame* frame) {
    klog("\n=== EXCEPTION ===\n");
    if (frame->int_no < 22) {
        klog(exception_names[frame->int_no]);
    } else {
        klog("Unknown Exception #");
        klog_hex(frame->int_no);
    }
    klog("\nError code: ");
    klog_hex(frame->err_code);
    klog("\nRIP: ");
    klog_hex(frame->rip);
    klog("\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}

// Exceptions WITHOUT error codes — push a dummy 0 for consistent frame layout
#define EXCEPTION(n) \
    extern "C" void isr_exc##n(); \
    asm( \
        ".globl isr_exc" #n "\n" \
        "isr_exc" #n ":\n" \
        "pushq $0\n" \
        "pushq $" #n "\n" \
        "jmp isr_common\n" \
    );

// Exceptions WITH error codes — the CPU already pushed the code
#define EXCEPTION_ERR(n) \
    extern "C" void isr_exc##n(); \
    asm( \
        ".globl isr_exc" #n "\n" \
        "isr_exc" #n ":\n" \
        "pushq $" #n "\n" \
        "jmp isr_common\n" \
    );

// Common handler: save registers, call exception_handler, restore, iretq
asm(R"(
.globl isr_common
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    callq exception_handler

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp   // skip int_no and err_code
    iretq
)");

// Exception 0-7: no error code
EXCEPTION(0)
EXCEPTION(1)
EXCEPTION(2)
EXCEPTION(3)
EXCEPTION(4)
EXCEPTION(5)
EXCEPTION(6)
EXCEPTION(7)
// Exception 8: error code (#DF)
EXCEPTION_ERR(8)
// Exception 9: no error code (reserved)
EXCEPTION(9)
// Exception 10-14: error code
EXCEPTION_ERR(10)
EXCEPTION_ERR(11)
EXCEPTION_ERR(12)
EXCEPTION_ERR(13)
EXCEPTION_ERR(14)
// Exception 15-16: no error code
EXCEPTION(15)
EXCEPTION(16)
// Exception 17: error code
EXCEPTION_ERR(17)
// Exception 18-20: no error code
EXCEPTION(18)
EXCEPTION(19)
EXCEPTION(20)
// Exception 21: error code
EXCEPTION_ERR(21)
// 22-31: reserved, no error code
EXCEPTION(22)
EXCEPTION(23)
EXCEPTION(24)
EXCEPTION(25)
EXCEPTION(26)
EXCEPTION(27)
EXCEPTION(28)
EXCEPTION(29)
EXCEPTION(30)
EXCEPTION(31)

} // namespace

void idt_init() {
    set_entry(0, (uint64_t)&isr_exc0, 0, 0x8E);
    set_entry(1, (uint64_t)&isr_exc1, 0, 0x8E);
    set_entry(2, (uint64_t)&isr_exc2, 0, 0x8E);
    set_entry(3, (uint64_t)&isr_exc3, 0, 0x8E);
    set_entry(4, (uint64_t)&isr_exc4, 0, 0x8E);
    set_entry(5, (uint64_t)&isr_exc5, 0, 0x8E);
    set_entry(6, (uint64_t)&isr_exc6, 0, 0x8E);
    set_entry(7, (uint64_t)&isr_exc7, 0, 0x8E);
    set_entry(8, (uint64_t)&isr_exc8, 0, 0x8E);
    set_entry(9, (uint64_t)&isr_exc9, 0, 0x8E);
    set_entry(10, (uint64_t)&isr_exc10, 0, 0x8E);
    set_entry(11, (uint64_t)&isr_exc11, 0, 0x8E);
    set_entry(12, (uint64_t)&isr_exc12, 0, 0x8E);
    set_entry(13, (uint64_t)&isr_exc13, 0, 0x8E);
    set_entry(14, (uint64_t)&isr_exc14, 0, 0x8E);
    set_entry(15, (uint64_t)&isr_exc15, 0, 0x8E);
    set_entry(16, (uint64_t)&isr_exc16, 0, 0x8E);
    set_entry(17, (uint64_t)&isr_exc17, 0, 0x8E);
    set_entry(18, (uint64_t)&isr_exc18, 0, 0x8E);
    set_entry(19, (uint64_t)&isr_exc19, 0, 0x8E);
    set_entry(20, (uint64_t)&isr_exc20, 0, 0x8E);
    set_entry(21, (uint64_t)&isr_exc21, 0, 0x8E);
    set_entry(22, (uint64_t)&isr_exc22, 0, 0x8E);
    set_entry(23, (uint64_t)&isr_exc23, 0, 0x8E);
    set_entry(24, (uint64_t)&isr_exc24, 0, 0x8E);
    set_entry(25, (uint64_t)&isr_exc25, 0, 0x8E);
    set_entry(26, (uint64_t)&isr_exc26, 0, 0x8E);
    set_entry(27, (uint64_t)&isr_exc27, 0, 0x8E);
    set_entry(28, (uint64_t)&isr_exc28, 0, 0x8E);
    set_entry(29, (uint64_t)&isr_exc29, 0, 0x8E);
    set_entry(30, (uint64_t)&isr_exc30, 0, 0x8E);
    set_entry(31, (uint64_t)&isr_exc31, 0, 0x8E);

    idtr.limit = sizeof(IDTEntry) * IDT_ENTRIES - 1;
    idtr.base = (uint64_t)&idt[0];

    asm volatile("lidt (%0)" : : "r"(&idtr) : "memory");
}
