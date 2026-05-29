/*============================================================================
 * lzss.c -- in-ROM LZSS decompressor.
 *
 * Format (matches tools/lzss-compress.c):
 *   header:  'L','Z','S','S', <4-byte BE uncompressed-size>
 *   body:    flag-byte then 8 outputs.  bit=1 -> literal (1 byte),
 *                                       bit=0 -> match pair (2 bytes)
 *   match pair: (off_lo, ((off_hi & 0xF)<<4) | (len-3))
 *   window absolute position = (off_hi << 8) | off_lo  in 0..4095.
 *
 * Decompresses in-place; the output buffer must be at least
 * `lzss_uncompressed_size(blob)` bytes.
 *==========================================================================*/
#include "xcpm.h"

#define LZSS_WINDOW_SIZE   4096
#define LZSS_WINDOW_MASK   (LZSS_WINDOW_SIZE - 1)

static u8 lzss_window[LZSS_WINDOW_SIZE];

/* Expose the window as a free 4 KB scratch buffer for the CCP after
   warm-boot.  Anyone using it must accept that the next warmboot will
   clobber it. */
u8  *lzss_scratch_buf(void)  { return lzss_window; }
u32  lzss_scratch_size(void) { return LZSS_WINDOW_SIZE; }

/* Returns the uncompressed size encoded in the LZSS header,
   or 0 if magic is invalid. */
u32 lzss_uncompressed_size(const u8 *blob)
{
    if (!blob) return 0;
    if (blob[0] != 'L' || blob[1] != 'Z' ||
        blob[2] != 'S' || blob[3] != 'S') return 0;
    return ((u32)blob[4] << 24) | ((u32)blob[5] << 16)
         | ((u32)blob[6] <<  8) |  (u32)blob[7];
}

/* Decompress `blob` (header + body) to `out`.  Returns the number of bytes
   written, or 0 on error.  Writes at most `out_max` bytes; if the
   uncompressed size exceeds `out_max`, returns 0 without writing. */
u32 lzss_decompress(const u8 *blob, u32 blob_size, u8 *out, u32 out_max)
{
    u32 expected = lzss_uncompressed_size(blob);
    if (expected == 0) return 0;
    if (expected > out_max) return 0;

    const u8 *in = blob + 8;
    const u8 *in_end = blob + blob_size;
    u32 op = 0;
    u32 wpos = 0;
    u32 flags = 0;

    /* Initialize window to zero so undefined pre-seen bytes are 0. */
    for (u32 i = 0; i < LZSS_WINDOW_SIZE; i++) lzss_window[i] = 0;

    while (op < expected) {
        if ((flags & 0x100) == 0) {
            if (in >= in_end) return 0;
            flags = (u32)(*in++) | 0xFF00u;
        }
        if (flags & 1) {
            /* literal */
            if (in >= in_end || op >= out_max) return 0;
            u8 c = *in++;
            out[op++] = c;
            lzss_window[wpos++ & LZSS_WINDOW_MASK] = c;
        } else {
            /* match pair */
            if (in + 1 >= in_end) return 0;
            u8 b1 = *in++;
            u8 b2 = *in++;
            u32 win_pos = (u32)b1 | ((u32)(b2 & 0xF0) << 4);
            u32 len     = (u32)(b2 & 0x0F) + 3;
            for (u32 i = 0; i < len; i++) {
                if (op >= out_max) return 0;
                u8 c = lzss_window[(win_pos + i) & LZSS_WINDOW_MASK];
                out[op++] = c;
                lzss_window[wpos++ & LZSS_WINDOW_MASK] = c;
            }
        }
        flags >>= 1;
    }

    return op;
}

/* Streaming variant: decompress blob directly to console via the
 * provided byte-emit callback.  No large output buffer needed — only
 * the 4 KB sliding window (already a file-scope static).  Returns the
 * number of bytes emitted, or 0 on header/format error. */
u32 lzss_decompress_stream(const u8 *blob, u32 blob_size,
                           void (*emit)(u8 c))
{
    u32 expected = lzss_uncompressed_size(blob);
    if (expected == 0) return 0;

    const u8 *in = blob + 8;
    const u8 *in_end = blob + blob_size;
    u32 op = 0;
    u32 wpos = 0;
    u32 flags = 0;

    for (u32 i = 0; i < LZSS_WINDOW_SIZE; i++) lzss_window[i] = 0;

    while (op < expected) {
        if ((flags & 0x100) == 0) {
            if (in >= in_end) return 0;
            flags = (u32)(*in++) | 0xFF00u;
        }
        if (flags & 1) {
            if (in >= in_end) return 0;
            u8 c = *in++;
            emit(c);
            lzss_window[wpos++ & LZSS_WINDOW_MASK] = c;
            op++;
        } else {
            if (in + 1 >= in_end) return 0;
            u8 b1 = *in++;
            u8 b2 = *in++;
            u32 win_pos = (u32)b1 | ((u32)(b2 & 0xF0) << 4);
            u32 len     = (u32)(b2 & 0x0F) + 3;
            for (u32 i = 0; i < len; i++) {
                u8 c = lzss_window[(win_pos + i) & LZSS_WINDOW_MASK];
                emit(c);
                lzss_window[wpos++ & LZSS_WINDOW_MASK] = c;
                op++;
            }
        }
        flags >>= 1;
    }
    return op;
}
