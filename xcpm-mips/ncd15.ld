/*============================================================================
 * ncd15.ld -- linker script for the NCD15 port of XCP/M.
 *
 * The NCD15 monitor loads us into DRAM at phys 0x0ED00000 (the same slot
 * the Xncd15r X-server uses) and enters at _start. We link bare-physical
 * at 0x0ED00000 and let the monitor/emulator address-mask handle the
 * KSEG0/KSEG1 view, exactly like xncd15r-mini.
 *
 * Region 0x0ED00000..0x0F000000 (3 MiB) is ours:
 *   0x0ED00000  kernel .text/.rodata/.data/.bss
 *   ...         (grows up; ~256 KiB is plenty)
 *   0x0ED40000  TPA  (transient program area for loaded apps)
 *   0x0EFE0000  supervisor stack top (grows down)
 *   0x0F000000  end (VRAM is BELOW us at 0x0EC80000; cart regs at 0x0F000000)
 *============================================================================*/
OUTPUT_FORMAT("elf32-bigmips")
OUTPUT_ARCH(mips)
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x0ED00000, LENGTH = 0x00300000
}

_tpa_base   = 0x0ED40000;
_tpa_end    = 0x0EF80000;   /* shrunk to make room for the RW-FAT shadow */
_ramfat_rw  = 0x0EF80000;   /* 128 KB writeable FAT shadow (disk_ramfat.c) */
_stack_top  = 0x0EFE0000;
_ram_base   = 0x0ED00000;
_ram_end    = 0x0F000000;

SECTIONS {
    .text 0x0ED00000 : {
        *(.text.start)
        *(.text*)
    } > RAM

    .rodata : { *(.rodata*) *(.rodata1) } > RAM

    .data : {
        *(.data*)
        *(.sdata*)
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > RAM

    _end = .;

    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.comment)
        *(.note*)
        *(.pdr)
        *(.gnu.attributes)
    }
}
