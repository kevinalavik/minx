/* minx kernel linker script
 *
 * The kernel is linked into the higher half of the address space at -2 GiB
 * (0xffffffff80000000) and entered in long mode with paging already set up by
 * the bootloader.  The lower half is left free for user space and the future
 * direct map window at 0xffff800000000000.
 */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* r-x */
    rodata PT_LOAD FLAGS(4);   /* r-- */
    data   PT_LOAD FLAGS(6);   /* rw- */
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .text : ALIGN(4K)
    {
        *(.text.boot)
        *(.text .text.*)
    } :text

    . = ALIGN(4K);
    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
        /* The bootloader's request block must stay readable, and the two
         * markers must bracket the requests in this exact order: the bootloader
         * derives the block's extent from them and walks it looking for
         * request IDs. */
        KEEP(*(.limine_requests_start_marker))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end_marker))
    } :rodata

    . = ALIGN(4K);
    .data : ALIGN(4K)
    {
        *(.data .data.*)
        *(.got .got.plt)
        *(.got)
    } :data

    . = ALIGN(4K);
    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
        /* __kernel_end is the exclusive end of the mapped kernel image.  It is
         * deliberately *not* followed by a page rounding: the byte at this
         * address is outside the PT_LOAD segment the bootloader maps, so the
         * symbol is only ever used for address comparisons, never dereferenced. */
        __kernel_end = .;
    } :data

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note .note.*)
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
    }
}