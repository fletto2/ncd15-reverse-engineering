/*============================================================================
 * ramdisk.c -- 4 KB RAM-disk for drive B:
 *
 * 32 × 128-byte CP/M sectors, all in BSS RAM, zeroed at boot.
 * Available before any block device or FAT32 layer is up.
 * Useful as a scratch area for temp files, pipe buffers, ASM scratch,
 * and bring-up testing.
 *
 * Exposes:
 *   ramdisk_read (sec, buf)   -- 128-byte sector read
 *   ramdisk_write(sec, buf)   -- 128-byte sector write
 *   RAMDISK_NSEC              -- total sectors (32)
 *==========================================================================*/
#include "xcpm.h"

#ifndef RAMDISK_BYTES
#define RAMDISK_BYTES   4096
#endif
#define RAMDISK_SECSZ   128
#if RAMDISK_BYTES > 0
#define RAMDISK_NSEC    (RAMDISK_BYTES / RAMDISK_SECSZ)
#else
#define RAMDISK_NSEC    0
#endif

#if RAMDISK_BYTES > 0
static u8 ramdisk_storage[RAMDISK_BYTES];   /* BSS, zeroed by startup */
#else
/* Disabled: provide a 1-byte sentinel so &ramdisk_storage exists. */
static u8 ramdisk_storage[1];
#endif

static void copy_bytes(u8 *dst, const u8 *src, u32 n)
{
    while (n--) *dst++ = *src++;
}

int ramdisk_read(u32 sec, void *buf)
{
    if (sec >= RAMDISK_NSEC) return 1;
    copy_bytes((u8 *)buf,
               &ramdisk_storage[sec * RAMDISK_SECSZ],
               RAMDISK_SECSZ);
    return 0;
}

int ramdisk_write(u32 sec, const void *buf)
{
    if (sec >= RAMDISK_NSEC) return 1;
    copy_bytes(&ramdisk_storage[sec * RAMDISK_SECSZ],
               (const u8 *)buf,
               RAMDISK_SECSZ);
    return 0;
}

u32 ramdisk_capacity(void)
{
    return RAMDISK_NSEC;
}

/* Test / diagnostic: dump first 16 bytes via UART. */
void ramdisk_dump_head(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        xputhex(ramdisk_storage[i], 2);
        uart_putc(' ');
    }
}
