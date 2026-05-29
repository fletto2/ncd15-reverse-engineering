/*============================================================================
 * app/app.ld -- link script for an XCP/M-MIPS user app.
 *
 * Apps are linked at the TPA base (0x0ED40000) and emitted as flat MIPS
 * binaries (objcopy -O binary). The kernel loader memcpy's the bytes to
 * that address and jalrs to offset 0, where _app_start (a `j app_main`
 * trampoline) lives. Absolute refs in the compiled code resolve because
 * the placement matches the link address.
 *==========================================================================*/
OUTPUT_FORMAT("elf32-bigmips")
OUTPUT_ARCH(mips)
ENTRY(_app_start)

MEMORY
{
    TPA (rwx) : ORIGIN = 0x0ED40000, LENGTH = 0x00080000   /* 512 KB */
}

SECTIONS
{
    .text : ALIGN(4) {
        KEEP(*(.text.entry))   /* _app_start must be at offset 0 */
        *(.text*)
    } > TPA

    .rodata : ALIGN(4) {
        *(.rodata*)
        *(.srodata*)
    } > TPA

    .data : ALIGN(4) {
        *(.data*)
        *(.sdata*)
    } > TPA

    .bss : ALIGN(4) {
        _app_bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        _app_bss_end = .;
    } > TPA

    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.note*) *(.comment) }
}
