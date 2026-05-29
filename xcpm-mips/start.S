/*============================================================================
 * start.S -- MIPS-I entry for the NCD15 port of XCP/M.
 *
 * The NCD15 boot monitor loads our ECOFF image into DRAM at the linked
 * paddr and enters at _start. The monitor's local-boot/ECOFF loader first
 * validates a vendor signature + CRC stash at the head of .text (see
 * xncd15r-mini/start.S and monitor.dis 0x0EC10A2C–0x0EC10AD8):
 *
 *   +0x00: b real_start          (skip the signature block)
 *   +0x04: nop                   (delay slot)
 *   +0x08: reserved (0)
 *   +0x0C: version word (3 -> bit0 set triggers the "19r" suffix check)
 *   +0x10: "Xncd19r\0"           (the signature the loader compares)
 *   +0x18: 0xFFFF                (CRC stash; matches the loader's initial
 *                                 streaming-CRC value, so the compare passes
 *                                 when the IRQ-driven CRC never runs)
 *   +0x1C: reserved
 *   +0x20: real_start            (stack setup, BSS clear, call main)
 *
 * We run linked bare-physical at 0x0ED00000; the monitor enters us via the
 * KSEG0/KSEG1 alias and the address-mask makes both views resolve to the
 * same DRAM.
 *============================================================================*/
	.set    noreorder
	.set    noat
	.set    mips1
	.section .text.start, "ax"
	.globl  _start
	.ent    _start
_start:
	b       real_start              /* +0x00 */
	nop                             /* +0x04 */
	.word   0                       /* +0x08 reserved */
	.word   3                       /* +0x0C version (bit0 -> check "19r") */
	.ascii  "Xncd19r"               /* +0x10..+0x16 signature */
	.byte   0                       /* +0x17 NUL */
	.hword  0xFFFF                  /* +0x18 CRC stash (= loader's seed) */
	.hword  0                       /* +0x1A reserved */
	.word   0                       /* +0x1C reserved */

real_start:                             /* +0x20 */
	/* Stack pointer = top of our region (from the linker script). */
	la      $sp, _stack_top
	move    $gp, $zero               /* -G0: no small-data; park gp */

	/* Zero BSS: [_bss_start, _bss_end). */
	la      $t0, _bss_start
	la      $t1, _bss_end
1:
	sltu    $t2, $t0, $t1
	beq     $t2, $zero, 2f
	nop
	sw      $zero, 0($t0)
	addiu   $t0, $t0, 4
	b       1b
	nop
2:
	jal     main                    /* never returns (warmboot loops) */
	nop
3:
	b       3b                       /* safety net */
	nop
	.end    _start
