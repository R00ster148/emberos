ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text   : { *(.text.entry) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss    : { *(.bss*) *(COMMON) }
    __bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
