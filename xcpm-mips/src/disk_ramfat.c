/*============================================================================
 * disk_ramfat.c -- read-only block device backed by an in-image FAT blob.
 *
 * The kernel image embeds `ramfat.img` (a FAT12 image generated at build
 * time with `mkfs.fat -F 12` and populated with app binaries via mcopy)
 * as a .rodata blob between `_ramfat_start` and `_ramfat_end`. blk_read
 * memcpy's sectors out of that blob; blk_write is a no-op (the image is
 * in ROM/rodata, intentionally — apps load by filename, they don't
 * mutate the filesystem at runtime). fat.c sits on top of these blk_*
 * primitives unchanged.
 *==========================================================================*/
#include "xcpm.h"

extern const unsigned char _ramfat_start[];
extern const unsigned char _ramfat_end[];

#define SECTOR_SIZE 512

/* RW shadow lives just below the stack-growth zone -- 128 KB at
 * 0x0EF80000..0x0EFA0000. TPA shrinks to end at 0x0EF80000 to make
 * room (still 2.25 MB for apps); stack top stays at 0x0EFE0000 with
 * 256 KB of headroom above the shadow. blk_init copies the rodata
 * "factory" image into here on first mount; subsequent boots restart
 * from rodata, so user edits are session-scoped. */
static unsigned char * const ramfat_rw = (unsigned char *)0x0EF80000UL;

static u32 ramfat_sectors(void)
{
    unsigned long bytes = (unsigned long)(_ramfat_end - _ramfat_start);
    return (u32)(bytes / SECTOR_SIZE);
}

static int initialized = 0;

int blk_init(void)
{
    if (initialized) return 0;
    unsigned long n = (unsigned long)(_ramfat_end - _ramfat_start);
    for (unsigned long i = 0; i < n; i++) ramfat_rw[i] = _ramfat_start[i];
    initialized = 1;
    return 0;
}

int blk_read(u32 lba, u32 count, void *buf)
{
    if (!initialized) blk_init();
    u32 last = lba + count;
    if (last < lba || last > ramfat_sectors()) return -1;

    const unsigned char *src = ramfat_rw + ((unsigned long)lba * SECTOR_SIZE);
    unsigned char *dst = (unsigned char *)buf;
    unsigned long n = (unsigned long)count * SECTOR_SIZE;
    while (n--) *dst++ = *src++;
    return 0;
}

int blk_write(u32 lba, u32 count, const void *buf)
{
    if (!initialized) blk_init();
    u32 last = lba + count;
    if (last < lba || last > ramfat_sectors()) return -1;

    unsigned char *dst = ramfat_rw + ((unsigned long)lba * SECTOR_SIZE);
    const unsigned char *src = (const unsigned char *)buf;
    unsigned long n = (unsigned long)count * SECTOR_SIZE;
    while (n--) *dst++ = *src++;
    return 0;
}

int blk_flush(void)
{
    return 0;
}

int blk_status(void)
{
    return 0;
}

u32 blk_capacity(void)
{
    return ramfat_sectors();
}
