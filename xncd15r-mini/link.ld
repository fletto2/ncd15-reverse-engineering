OUTPUT_FORMAT("elf32-bigmips")
OUTPUT_ARCH(mips)
ENTRY(_start)
MEMORY {
    RAM (rwx) : ORIGIN = 0x0ED00000, LENGTH = 0x400000
}
SECTIONS {
    .text 0x0ED00000 : { *(.text.start) *(.text*) } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data   : { *(.data*)   } > RAM
    .bss    : { *(.bss*) *(COMMON) } > RAM
    _end = .;
}
