/* OSEF bare-metal x86_64 kernel linker script (Limine protocol, higher half).
 *
 * Link with: x86_64-elf-ld -nostdlib -static -z max-page-size=0x1000 -T linker.ld
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(osef_kmain)

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R X */
    rodata  PT_LOAD FLAGS(4);   /* R   */
    data    PT_LOAD FLAGS(6);   /* R W */
    stack   PT_LOAD FLAGS(6);   /* R W, no file contents: the kernel stack */
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /* The OSEF_HARDWARE(...) record. Its own output section, so osef-image
       can find it by name in the kernel ELF. */
    .osef_hardware : {
        KEEP(*(.osef_hardware))
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    /* Limine requests (written by the bootloader, hence in the data segment),
       delimited by the start/end markers. */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :data

    .data : {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :data

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    } :data

    /* .bss last: it occupies no space in the file. */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* The kernel stack (osef_kmain switches to it before anything else).
       The 64 KiB below it belong to no segment, so the bootloader never maps
       them: a stack overflow hits this guard and raises a page fault (which
       the kernel turns into a "stack overflow" process fault or panic)
       instead of silently overwriting .bss. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __osef_stack_guard_start = .;
    . += 0x10000;
    .osef_stack (NOLOAD) : ALIGN(4096) {
        __osef_stack_bottom = .;
        . += 0x40000;           /* 256 KiB */
        __osef_stack_top = .;
    } :stack

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.fini_array .fini_array.* .dtors .dtors.*)
    }
}
