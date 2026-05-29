/*============================================================================
 * bios.h -- BIOS dispatcher + handler prototypes (§7)
 *==========================================================================*/
#ifndef XCPM_BIOS_H
#define XCPM_BIOS_H

#include "xcpm.h"

#define BIOS_NFUNCS     32      /* covers v1.3's 0..24 with headroom */

/* Function indices (§7) */
enum {
    BIOS_INIT       = 0,
    BIOS_WBOOT      = 1,
    BIOS_CONSTAT    = 2,
    BIOS_CONIN      = 3,
    BIOS_CONOUT     = 4,
    BIOS_LSTOUT     = 5,
    BIOS_AUXOUT     = 6,
    BIOS_AUXIN      = 7,
    BIOS_HOME       = 8,
    BIOS_SELDSK     = 9,
    BIOS_SETTRK     = 10,
    BIOS_SETSEC     = 11,
    BIOS_SETDMA     = 12,
    BIOS_READ       = 13,
    BIOS_WRITE      = 14,
    BIOS_LSTSTAT    = 15,
    BIOS_SECTRAN    = 16,
    BIOS_CONOUTSTAT = 17,
    BIOS_GETSEG     = 18,
    BIOS_GETIOB     = 19,
    BIOS_SETIOB     = 20,
    BIOS_FLUSH      = 21,
    BIOS_SETEXC     = 22,
    BIOS_AUXINSTAT  = 23,
    BIOS_AUXOUTSTAT = 24,
};

void bios_init(void);
u32  bios_dispatch(u32 func, u32 parm1, u32 parm2);

/* Direct callable for kernel-side use. */
u32  bios_call(u32 func, u32 parm1, u32 parm2);

extern void bios_trap_entry(void);

#endif
