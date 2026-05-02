OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)

PHDRS
{
    headers PT_LOAD FILEHDR PHDRS;
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    /* Limine stivale2 requires the kernel in the higher half.
       Limine sets up paging so this virtual address maps to the
       physical load address. */
    . = 0xFFFFFFFF80000000 + SIZEOF_HEADERS;

    .stivale2hdr : {
        KEEP(*(.stivale2hdr))
    } :headers

    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
        *(.comment.*)
    }
}
