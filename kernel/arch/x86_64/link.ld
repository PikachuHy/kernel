OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

PHDRS
{
    rx  PT_LOAD;
    rw  PT_LOAD;
}

SECTIONS
{
    /* Limine protocol requires the kernel in the higher half.
       Limine sets up paging so this virtual address maps to the
       physical load address.
       Limine 12.x requires segments with different permissions
       to not share the same page. So we merge .limine_reqs,
       .text, and .rodata into one RX segment. */
    . = 0xFFFFFFFF80000000 + SIZEOF_HEADERS;

    .limine_reqs : {
        KEEP(*(.limine_reqs))
    } :rx

    .text : {
        *(.text .text.*)
    } :rx

    .rodata : {
        *(.rodata .rodata.*)
    } :rx

    .data : {
        *(.data .data.*)
    } :rw

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :rw

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
        *(.comment.*)
    }
}
