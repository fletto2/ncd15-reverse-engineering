/*============================================================================
 * bdos.h -- BDOS dispatcher + handler prototypes (§8)
 *==========================================================================*/
#ifndef XCPM_BDOS_H
#define XCPM_BDOS_H

#include "xcpm.h"

/* Maximum BDOS function number we route. Functions above this are NOPs. */
#define BDOS_NFUNCS     128

/* Public C entry. */
void bdos_init(void);
u32  bdos_dispatch(u32 func, u32 parm1, u32 parm2);

/* Direct callable for kernel-side use (CCP, applets, etc.) */
u32  bdos_call(u32 func, u32 parm1, u32 parm2);

/* Provided by traps.s */
extern void bdos_trap_entry(void);

#endif
