/* NCD15 emulator — shared types and constants. */

#ifndef NCD15_EMU_H
#define NCD15_EMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* --- Hardware map (see FINDINGS.md / NEW_INSIGHTS.md) --- */

#define NCD15_ROM_PHYS      0x0EC00000u  /* 256 KiB ROM */
#define NCD15_ROM_SIZE      0x00040000u
#define NCD15_ROM_KSEG1     0xBFC00000u  /* uncached alias */
#define NCD15_ROM_KUSEG     0x1FC00000u  /* KUSEG alias used by cache trampoline */

#define NCD15_DRAM_PHYS     0x00000000u  /* 4 MiB DRAM at phys 0 */
#define NCD15_DRAM_SIZE     0x00400000u
#define NCD15_DRAM_ALIAS    0x03D00000u  /* wraps every this many bytes */

#define NCD15_VRAM_PHYS     0x0F000000u  /* 1 MiB, KSEG1 access at 0xAF000000 */
#define NCD15_VRAM_SIZE     0x00100000u

#define NCD15_DUART_PHYS    0x0E880000u  /* KSEG1 0xBE880000 = phys 0x0E880000 */
#define NCD15_LANCE_PHYS    0x0E482000u  /* KSEG1 0xBE482000 */
#define NCD15_CRTC_PHYS     0x0E380000u  /* KSEG1 0xBE380000 */
#define NCD15_KBD_PHYS      0x0EC80000u  /* KSEG1 0xAEC80000 — bit-banged NVRAM+kbd */
#define NCD15_MEMCTL_PHYS   0xFFFE0000u  /* KSEG2 pass-through on R3052 */
#define NCD15_VIDCTL_PHYS   0x0F000000u  /* KSEG1 0xAF000000 — same as VRAM base first 4 bytes = cart ID */

/* --- MIPS-I CPU state --- */

typedef struct mips_cpu {
    u32 pc;
    u32 next_pc;          /* for branch-delay emulation */
    u32 r[32];            /* GPRs; r[0] always 0 */
    u32 hi, lo;

    /* CP0 */
    u32 cp0_status;       /* register 12 */
    u32 cp0_cause;        /* register 13 */
    u32 cp0_epc;          /* register 14 */
    u32 cp0_badvaddr;     /* register 8 */
    u32 cp0_prid;         /* register 15 — processor ID */

    bool in_delay_slot;   /* set by preceding branch */
    bool branch_taken;
    u32  branch_target;

    struct bus *bus;      /* back-pointer to memory/MMIO bus */
    u64 cycles;
    bool halted;
} mips_cpu;

void mips_reset(mips_cpu *cpu, struct bus *bus);
void mips_step(mips_cpu *cpu);   /* execute one instruction */
void mips_run(mips_cpu *cpu, u64 max_cycles);
void mips_dump(const mips_cpu *cpu);

/* --- Memory / MMIO bus --- */

typedef struct mmio_region {
    u32 start;            /* physical start */
    u32 length;
    u32 (*read)(void *ctx, u32 offset, unsigned size);
    void (*write)(void *ctx, u32 offset, u32 value, unsigned size);
    void *ctx;
    const char *name;
} mmio_region;

#define MAX_MMIO_REGIONS 16

typedef struct bus {
    u8 *rom;              /* NCD15_ROM_SIZE bytes, big-endian order */
    u8 *dram;             /* NCD15_DRAM_SIZE bytes, big-endian order */
    u8 *vram;             /* NCD15_VRAM_SIZE bytes, big-endian order */
    mmio_region regions[MAX_MMIO_REGIONS];
    size_t nregions;
    bool trace;           /* log every access */
} bus;

void bus_init(bus *b, u8 *rom_bytes);
void bus_add_mmio(bus *b, mmio_region r);
u32  bus_read(bus *b, u32 vaddr, unsigned size);      /* size = 1, 2, or 4 */
void bus_write(bus *b, u32 vaddr, u32 value, unsigned size);

/* Translate a MIPS virtual address to physical. R3052 has no MMU; this
 * is just the standard KSEG0/KSEG1/KUSEG segmentation. */
u32  mips_va_to_pa(u32 vaddr);

/* --- DUART (MC68681 / SCN2681) --- */

struct duart;
struct duart *duart_new(void);
void duart_free(struct duart *d);
u32  duart_read(void *ctx, u32 offset, unsigned size);
void duart_write(void *ctx, u32 offset, u32 value, unsigned size);
void duart_feed_input(struct duart *d, int channel, u8 byte);

/* --- Video control registers (0xAF000000..3 cart-ID etc.) --- */

struct vidctl;
struct vidctl *vidctl_new(void);
u32  vidctl_read(void *ctx, u32 offset, unsigned size);
void vidctl_write(void *ctx, u32 offset, u32 value, unsigned size);

/* --- Memory controller stub (0xFFFE0000) --- */

struct memctl;
struct memctl *memctl_new(void);
u32  memctl_read(void *ctx, u32 offset, unsigned size);
void memctl_write(void *ctx, u32 offset, u32 value, unsigned size);

#endif
