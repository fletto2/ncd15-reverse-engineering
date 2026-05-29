/*============================================================================
 * ramfat_blob.S -- embed ramfat.img (a build-time FAT12 image populated
 * with our app binaries via mcopy) as a rodata blob inside the kernel.
 *
 * disk_ramfat.c serves blk_read out of [_ramfat_start, _ramfat_end);
 * fat.c mounts on top of those primitives. The image is read-only;
 * blk_write returns -1 by design.
 *==========================================================================*/
    .section .rodata.ramfat, "a"
    .balign 4
    .globl _ramfat_start
_ramfat_start:
    .incbin "ramfat.img"
    .globl _ramfat_end
_ramfat_end:
