/*============================================================================
 * xcpm.h -- common types, build-config constants, and module entry points
 *==========================================================================*/
#ifndef XCPM_H
#define XCPM_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned long       u32;
typedef signed char         i8;
typedef signed short        i16;
typedef signed long         i32;
typedef unsigned long       size_t;

/* Provided by the linker script */
extern u32 _stack_top;
extern u32 _tpa_base;
extern u32 _ram_end;
extern u32 _bss_start, _bss_end;
extern u32 _data_start, _data_end, _data_lma;

/* RAM vector table (256 longs at RAM_VEC_ADDR) */
#define RAM_VEC_TABLE   ((volatile u32 *)RAM_VEC_ADDR)

/* UART driver -- console I/O abstraction */
void uart_init(void);
void uart_putc(int c);
int  uart_getc(void);          /* blocking */
int  uart_poll(void);          /* nonblocking: -1 if no byte */
void uart_puts(const char *s);
void uart_panic(void);
void uart_panic_at(u32 sr_pc_addr);

/* B: ramdisk (4 KB, always present) */
int  ramdisk_read (u32 sec, void *buf);
int  ramdisk_write(u32 sec, const void *buf);
u32  ramdisk_capacity(void);
void ramdisk_dump_head(void);

/* LZSS decompressor (matches tools/lzss-compress.c) */
u32  lzss_uncompressed_size(const u8 *blob);
u32  lzss_decompress(const u8 *blob, u32 blob_size, u8 *out, u32 out_max);
u32  lzss_decompress_stream(const u8 *blob, u32 blob_size, void (*emit)(u8 c));
u8  *lzss_scratch_buf(void);
u32  lzss_scratch_size(void);

/* Disk driver (§18b) */
int  blk_init(void);
int  blk_read (u32 lba, u32 count, void *buf);
int  blk_write(u32 lba, u32 count, const void *buf);
int  blk_flush(void);
int  blk_status(void);
u32  blk_capacity(void);

/* Tiny utilities (no libc) */
size_t xstrlen(const char *s);
void   xputhex(u32 v, int digits);
void   xputdec(u32 v);

#endif /* XCPM_H */
