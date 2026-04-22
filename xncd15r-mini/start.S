/* Entry at 0x8ED00000 (KSEG0 cached view of phys 0x0ED00000).
 *
 * The NCD15 monitor's ECOFF loader validates that the 4 bytes at
 * offset +0x10 of the .text section are 'X','n','c','d' (or 'M' at
 * byte 0), then checks another byte at +0x13 for 'd'. See the real
 * Xncd15r image: bytes at .text+0x10 are "Xncd19r\0". This is a
 * vendor signature we have to embed.
 *
 * Layout of the first 0x20 bytes of .text:
 *   +0x00: branch forward past the signature block
 *   +0x04: nop (branch delay slot)
 *   +0x08..0x0F: reserved / zero
 *   +0x10..0x17: "Xncd15r\0"   <-- the signature the loader checks
 *   +0x18..0x1F: reserved / version word
 *   +0x20: real _start code (stack setup, jal main, halt)
 */
        .set    noreorder
        .set    mips1
        .section .text.start, "ax"
        .global _start
_start:
        b       real_start              /* +0x00: skip over signature block */
        nop                             /* +0x04: branch delay slot */

        .word   0                       /* +0x08: reserved */
        .word   3                       /* +0x0C: version / flags
                                                   (real Xncd15r has 3 here) */
        .ascii  "Xncd15r"               /* +0x10..+0x16: signature */
        .byte   0                       /* +0x17: NUL terminator */
        .word   0                       /* +0x18: reserved */
        .word   0                       /* +0x1C: reserved */

real_start:                             /* +0x20 */
        lui     $sp, 0x0ed3             /* sp = 0x0ED3FF00 */
        ori     $sp, $sp, 0xff00
        lui     $gp, 0                  /* we compile -G0; keep gp zero */
        jal     main
        nop
halt:   b       halt
        nop
