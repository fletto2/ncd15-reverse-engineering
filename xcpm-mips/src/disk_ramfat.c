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

static u32 ramfat_sectors(void)
{
    unsigned long bytes = (unsigned long)(_ramfat_end - _ramfat_start);
    return (u32)(bytes / SECTOR_SIZE);
}

int blk_init(void)
{
    return 0;
}

int blk_read(u32 lba, u32 count, void *buf)
{
    u32 last = lba + count;
    if (last < lba || last > ramfat_sectors()) return -1;

    const unsigned char *src = _ramfat_start + ((unsigned long)lba * SECTOR_SIZE);
    unsigned char *dst = (unsigned char *)buf;
    unsigned long n = (unsigned long)count * SECTOR_SIZE;
    while (n--) *dst++ = *src++;
    return 0;
}

int blk_write(u32 lba, u32 count, const void *buf)
{
    (void)lba; (void)count; (void)buf;
    return -1;   /* read-only image — apps load by name, they don't mutate */
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
