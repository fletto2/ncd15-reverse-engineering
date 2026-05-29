/*============================================================================
 * disk_null.c -- null block device driver (placeholder).
 *
 * All operations fail with -1. Used during early bring-up before a real
 * board-specific driver is wired in. Pick a real driver via DISK_DRIVER
 * in the Makefile, e.g. DISK_DRIVER=sd_spi.
 *==========================================================================*/
#include "xcpm.h"

int blk_init(void)                                      { return -1; }
int blk_read (u32 l, u32 c, void *b)                    { (void)l; (void)c; (void)b; return -1; }
int blk_write(u32 l, u32 c, const void *b)              { (void)l; (void)c; (void)b; return -1; }
int blk_flush(void)                                     { return -1; }
int blk_status(void)                                    { return -1; }
u32 blk_capacity(void)                                  { return 0; }
