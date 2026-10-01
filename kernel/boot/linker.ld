ENTRY(_start)

SECTIONS {
    . = 0xffffffff80000000;
    _kernel_start = .;

    .limine_requests : ALIGN(8) {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    }

    .text : ALIGN(0x1000) { *(.text .text.*) }
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .bss : ALIGN(0x1000) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = .;
    /DISCARD/ : { *(.eh_frame .note .comment) }
}
