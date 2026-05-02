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
    /* Phase 1: identity-mapped at 2MB. Higher-half (0xFFFF'8000'0000'0000)
       will be set up in Phase 2 when paging is enabled. */
    . = 0x200000 + SIZEOF_HEADERS;

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
