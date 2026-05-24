#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/core/sched/sched.hpp"
#include "kernel/core/object/process.hpp"
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

} // namespace

auto idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t type_attr) -> void {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector = 0x08;
    idt[vector].ist = ist;
    idt[vector].type_attr = type_attr;
    idt[vector].reserved = 0;
}

namespace {

extern "C" auto exception_handler(InterruptFrame* frame) -> void {
    // #PF: try to handle via demand paging / COW
    if (frame->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));

        // Save critical IRET frame fields BEFORE calling HandlePageFault,
        // whose deep call chain on the IST stack can overwrite the frame
        // (CS, SS, RIP, RFLAGS).  We read the raw values now and restore
        // known-good selectors afterward.
        // IRET frame (after 15 regs + int_no + err_code):
        //   raw[17] = RIP, raw[18] = CS, raw[19] = RFLAGS
        //   raw[20] = RSP (ring-3 only), raw[21] = SS (ring-3 only)
        uint64_t* raw          = reinterpret_cast<uint64_t*>(frame);
        uint64_t  pf_rip       = raw[17];
        uint64_t  pf_cs        = raw[18];
        uint64_t  pf_rflags    = raw[19];
        bool      pf_ring3     = (pf_cs & 3) == 3;
        uint64_t  pf_rsp       = pf_ring3 ? raw[20] : 0;

        Thread* cur = current_thread();
        if (cur && cur->process) {
            bool was_write = (frame->err_code & 0x2) != 0;
            bool handled = cur->process->HandlePageFault(cr2, was_write);
            if (handled) {
                // IST stack may have been overwritten by the deep call chain.
                // Restore the IRET frame using the values saved *before* the call.
                // Re-read raw pointer in case stack grew (same address though).
                raw = reinterpret_cast<uint64_t*>(frame);
                raw[17] = pf_rip;
                raw[18] = pf_ring3 ? 0x1B : 0x08;
                raw[19] = pf_rflags;
                if (pf_ring3) {
                    raw[20] = pf_rsp;
                    raw[21] = 0x13;          // SS = user data (DPL=3, entry 2)
                }
                return;
            }
        }

        // Couldn't handle — fall through to panic
        klog("\n=== PAGE FAULT (unhandled) ===\n");
        klog(" in process ");
        klog(cur && cur->process ? cur->process->name : "(none)");
        klog("\nfault addr: ");
        klog_hex(cr2);
        klog("\nRIP: ");
        klog_hex(frame->rip);
        klog("\nerror code: ");
        klog_hex(frame->err_code);
        klog("\n");
        while (1) {
            asm volatile("cli; hlt");
        }
    }

    // All other exceptions: print and halt
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
    // Force-correct CS/SS in iretq frame. IST #PF handling may overwrite
    // frame fields below the save area on the IST stack.
    movq 8(%rsp), %rdi       // saved CS
    testb $3, %dil
    jz 1f                     // ring-0: CS=0x08 is fine
    movq $0x1B, 8(%rsp)       // force ring-3 CS
    movq $0x13, 32(%rsp)      // force ring-3 SS
1:  iretq
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

extern "C" {
    void irq_stub_32(); void irq_stub_33(); void irq_stub_34(); void irq_stub_35();
    void irq_stub_36(); void irq_stub_37(); void irq_stub_38(); void irq_stub_39();
    void irq_stub_40(); void irq_stub_41(); void irq_stub_42(); void irq_stub_43();
    void irq_stub_44(); void irq_stub_45(); void irq_stub_46(); void irq_stub_47();
}

auto idt_init() -> void {
    idt_set_gate(0, (uint64_t)&isr_exc0, 0, 0x8E);
    idt_set_gate(1, (uint64_t)&isr_exc1, 0, 0x8E);
    idt_set_gate(2, (uint64_t)&isr_exc2, 0, 0x8E);
    idt_set_gate(3, (uint64_t)&isr_exc3, 0, 0x8E);
    idt_set_gate(4, (uint64_t)&isr_exc4, 0, 0x8E);
    idt_set_gate(5, (uint64_t)&isr_exc5, 0, 0x8E);
    idt_set_gate(6, (uint64_t)&isr_exc6, 0, 0x8E);
    idt_set_gate(7, (uint64_t)&isr_exc7, 0, 0x8E);
    idt_set_gate(8, (uint64_t)&isr_exc8, 2, 0x8E);   // IST2 for #DF
    idt_set_gate(9, (uint64_t)&isr_exc9, 0, 0x8E);
    idt_set_gate(10, (uint64_t)&isr_exc10, 0, 0x8E);
    idt_set_gate(11, (uint64_t)&isr_exc11, 0, 0x8E);
    idt_set_gate(12, (uint64_t)&isr_exc12, 0, 0x8E);
    idt_set_gate(13, (uint64_t)&isr_exc13, 0, 0x8E);
    idt_set_gate(14, (uint64_t)&isr_exc14, 1, 0x8E);   // IST1 for #PF
    idt_set_gate(15, (uint64_t)&isr_exc15, 0, 0x8E);
    idt_set_gate(16, (uint64_t)&isr_exc16, 0, 0x8E);
    idt_set_gate(17, (uint64_t)&isr_exc17, 0, 0x8E);
    idt_set_gate(18, (uint64_t)&isr_exc18, 0, 0x8E);
    idt_set_gate(19, (uint64_t)&isr_exc19, 0, 0x8E);
    idt_set_gate(20, (uint64_t)&isr_exc20, 0, 0x8E);
    idt_set_gate(21, (uint64_t)&isr_exc21, 0, 0x8E);
    idt_set_gate(22, (uint64_t)&isr_exc22, 0, 0x8E);
    idt_set_gate(23, (uint64_t)&isr_exc23, 0, 0x8E);
    idt_set_gate(24, (uint64_t)&isr_exc24, 0, 0x8E);
    idt_set_gate(25, (uint64_t)&isr_exc25, 0, 0x8E);
    idt_set_gate(26, (uint64_t)&isr_exc26, 0, 0x8E);
    idt_set_gate(27, (uint64_t)&isr_exc27, 0, 0x8E);
    idt_set_gate(28, (uint64_t)&isr_exc28, 0, 0x8E);
    idt_set_gate(29, (uint64_t)&isr_exc29, 0, 0x8E);
    idt_set_gate(30, (uint64_t)&isr_exc30, 0, 0x8E);
    idt_set_gate(31, (uint64_t)&isr_exc31, 0, 0x8E);

    // IRQ gates: vectors 32-47, interrupt gate (type_attr = 0x8E)
    uint64_t stubs[] = {
        (uint64_t)&irq_stub_32, (uint64_t)&irq_stub_33, (uint64_t)&irq_stub_34, (uint64_t)&irq_stub_35,
        (uint64_t)&irq_stub_36, (uint64_t)&irq_stub_37, (uint64_t)&irq_stub_38, (uint64_t)&irq_stub_39,
        (uint64_t)&irq_stub_40, (uint64_t)&irq_stub_41, (uint64_t)&irq_stub_42, (uint64_t)&irq_stub_43,
        (uint64_t)&irq_stub_44, (uint64_t)&irq_stub_45, (uint64_t)&irq_stub_46, (uint64_t)&irq_stub_47,
    };
    for (int i = 0; i < 16; i++) idt_set_gate(32 + i, stubs[i], 0, 0x8E);

    idtr.limit = sizeof(IDTEntry) * IDT_ENTRIES - 1;
    idtr.base = (uint64_t)&idt[0];

    asm volatile("lidt (%0)" : : "r"(&idtr) : "memory");
}
