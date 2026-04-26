/* Entry at 0x8ED00000 (KSEG0 cached view of phys 0x0ED00000).
 *
 * The NCD15 monitor's ECOFF loader validates a vendor signature at
 * offset +0x10 of the first section's data. Full check chain (see
 * monitor.dis 0x0EC10A2C – 0x0EC10AD8):
 *
 *   byte[+0x10]: 'X' (0x58) or 'M' (0x4D), context-dependent
 *   byte[+0x11]: 'n'
 *   byte[+0x12]: 'c'
 *   byte[+0x13]: 'd'
 *   if byte[+0x0F] has bit 0 set:
 *     byte[+0x14]: '1'
 *     byte[+0x15]: '9'     <-- NOT '5'!
 *     byte[+0x16]: 'r'
 *
 * The real Xncd15r firmware has "Xncd19r\0" at this location despite
 * the model being 15r. Must match exactly, or set byte[+0x0F]'s low
 * bit to 0 to skip the version check — but we mimic the real image.
 *
 * Layout of the first 0x20 bytes of .text:
 *   +0x00: b real_start (skip past signature)
 *   +0x04: nop (branch delay slot)
 *   +0x08: reserved (0)
 *   +0x0C: version word (3, matches real — bit 0 set triggers the
 *                         "19r" version-suffix check)
 *   +0x10..+0x16: "Xncd19r" (the signature the loader checks)
 *   +0x17:  0 (NUL terminator)
 *   +0x18..+0x1F: reserved (0)
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
        .ascii  "Xncd19r"               /* +0x10..+0x16: signature
                                                  (must match real firmware's
                                                  string; the version here is
                                                  1.9r, not 15r) */
        .byte   0                       /* +0x17: NUL terminator */
        .hword  0xFFFF                  /* +0x18: file-stored CRC-16. The
                                                  boot loader compares this
                                                  against a streaming CRC
                                                  accumulated into a runtime
                                                  slot whose initial value is
                                                  0xFFFF. By stashing the
                                                  same 0xFFFF here, the
                                                  comparison passes for
                                                  binaries where the
                                                  IRQ-driven streaming CRC
                                                  compute never runs (e.g.
                                                  in the C emulator where we
                                                  don't dispatch interrupts).
                                                  Real hardware computes the
                                                  actual CRC via IRQ; this
                                                  field gets overwritten
                                                  before the comparison. */
        .hword  0                       /* +0x1A: reserved */
        .word   0                       /* +0x1C: reserved (entry-point
                                                  pointer or similar; the
                                                  loader reads it into
                                                  data_0x0EC00BA0). */

real_start:                             /* +0x20 */
        lui     $sp, 0x0ed3             /* sp = 0x0ED3FF00 */
        ori     $sp, $sp, 0xff00
        lui     $gp, 0                  /* we compile -G0; keep gp zero */
        jal     main
        nop
halt:   b       halt
        nop
