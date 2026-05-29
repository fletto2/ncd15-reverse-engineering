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
	/* M5d: install the syscall exception handler at virtual 0x80000080.
	 * BEV is cleared by the monitor by the time we run (per CLAUDE.md),
	 * so general exceptions vector here. Copy _exc_handler_text bytes
	 * (handler stub below) to 0x80000080, then flush I-cache so the CPU
	 * fetches the new bytes instead of stale ones. */
	la      $t0, _exc_handler_text
	la      $t1, _exc_handler_text_end
	lui     $t2, 0x8000
	ori     $t2, $t2, 0x0080
3:
	lw      $t3, 0($t0)
	addiu   $t0, $t0, 4
	sw      $t3, 0($t2)
	bne     $t0, $t1, 3b
	addiu   $t2, $t2, 4

	/* I-cache invalidate over the bytes we just wrote. R3052: set CP0
	 * reg 7 bit 13 (cache-isolate, NOT R3000's Status.IsC), sw zero in
	 * 16-byte strides, clear bit again. Emulator no-ops the stores;
	 * real HW invalidates cache tags. */
	mfc0    $t4, $7
	nop
	li      $t5, 0x2000              /* bit 13 */
	or      $t6, $t4, $t5
	mtc0    $t6, $7
	nop
	nop
	lui     $t2, 0x8000
	ori     $t2, $t2, 0x0080
	addiu   $t3, $t2, 256            /* invalidate 256 bytes covering handler */
4:
	sw      $zero, 0($t2)
	bne     $t2, $t3, 4b
	addiu   $t2, $t2, 16
	mtc0    $t4, $7                  /* restore CCR */
	nop
	nop

	jal     main                    /* never returns (warmboot loops) */
	nop
5:
	b       5b                       /* safety net */
	nop
	.end    _start

/*============================================================================
 * Exception handler stub, copied verbatim to virtual 0x80000080 by the
 * boot path above. Position-independent: uses only $sp-relative loads /
 * stores and `jal bdos_dispatch` (28-bit absolute that resolves correctly
 * because the handler and bdos_dispatch both live in KSEG0, top 4 bits =
 * 0x8).
 *
 * Convention (must match libxcpm.h's bdos_sys()):
 *   on syscall: $v0 = BDOS func, $a0 = p1, $a1 = p2.
 *   on return : $v0 = result; EPC advanced by 4 so we don't re-execute
 *               the syscall instruction.
 *
 * NOTE: re-uses the caller's stack (the app's $sp), which is fine because
 * apps and the kernel share TPA-resident memory and there's plenty of
 * headroom (~256 KB of TPA, app code/data tiny).
 *==========================================================================*/
	.section .text.exc, "ax"
	.balign 4
	.globl  _exc_handler_text
_exc_handler_text:
	addiu   $sp, $sp, -56
	sw      $at, 4($sp)
	sw      $v1, 12($sp)
	sw      $a0, 16($sp)
	sw      $a1, 20($sp)
	sw      $a2, 24($sp)
	sw      $a3, 28($sp)
	sw      $t0, 32($sp)
	sw      $t1, 36($sp)
	sw      $t2, 40($sp)
	sw      $t3, 44($sp)
	sw      $t4, 48($sp)
	sw      $ra, 52($sp)

	/* Marshal: bdos_dispatch(func, p1, p2) <- (v0, a0, a1) */
	move    $a2, $a1                /* p2 */
	move    $a1, $a0                /* p1 */
	jal     bdos_dispatch
	move    $a0, $v0                /* func (delay slot) */

	/* $v0 now holds the result. */
	lw      $at, 4($sp)
	lw      $v1, 12($sp)
	lw      $a0, 16($sp)
	lw      $a1, 20($sp)
	lw      $a2, 24($sp)
	lw      $a3, 28($sp)
	lw      $t0, 32($sp)
	lw      $t1, 36($sp)
	lw      $t2, 40($sp)
	lw      $t3, 44($sp)
	lw      $t4, 48($sp)
	lw      $ra, 52($sp)
	addiu   $sp, $sp, 56

	/* Advance EPC past the syscall and RFE-return. */
	mfc0    $k0, $14                /* EPC */
	nop
	addiu   $k0, $k0, 4
	jr      $k0
	rfe
	.globl  _exc_handler_text_end
_exc_handler_text_end:
