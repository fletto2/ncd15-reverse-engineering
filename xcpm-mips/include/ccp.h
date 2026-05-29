/*============================================================================
 * ccp.h -- Console Command Processor (§16)
 *
 * Phase-1 surface: prompt + line edit + dispatch + a handful of built-ins.
 * No vocabularies, no wildcards, no history -- those are later phases.
 *==========================================================================*/
#ifndef XCPM_CCP_H
#define XCPM_CCP_H

#include "xcpm.h"

void ccp_run(void);

/* Helpers used by built-ins (also useful for kernel diagnostics). */
int  parse_hex(const char *s, u32 *out);
int  parse_dec(const char *s, u32 *out);

#endif
