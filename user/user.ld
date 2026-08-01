ENTRY(_start)

/* User programs link at 1GB, well above the kernel and its heap. elf_exec()
   maps these pages on load. */
SECTIONS
{
    . = 0x40000000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
