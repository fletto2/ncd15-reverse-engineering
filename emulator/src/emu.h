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

/* KSEG1 accesses subtract 0xA0000000 to get physical addresses. So
 * KSEG1 0xBE880000 → phys 0x1E880000, etc. */
#define NCD15_VRAM_PHYS     0x0F000000u  /* KSEG1 0xAF000000 (phys 0xAF..-0xA0..) */
#define NCD15_VRAM_SIZE     0x00100000u

#define NCD15_DUART_PHYS    0x1E880000u  /* KSEG1 0xBE880000 */
#define NCD15_LANCE_PHYS    0x1E482000u  /* KSEG1 0xBE482000 — registers */
#define NCD15_LANCE_SHMEM_PHYS  0x1E480000u  /* KSEG1 0xBE480000 — 128 KB shared mem
                                              * window for LANCE descriptors and
                                              * packet buffers. CPU sees a 256 KB
                                              * region (4-byte stride / halfword
                                              * granularity); LANCE sees byte-
                                              * addressed 128 KB. */
#define NCD15_LANCE_SHMEM_SIZE  0x00020000u  /* 128 KB on the LANCE side */
#define NCD15_CRTC_PHYS     0x1E380000u  /* KSEG1 0xBE380000 */
#define NCD15_KBD_PHYS      0x0EC80000u  /* KSEG1 0xAEC80000 = phys 0x0EC80000 */
#define NCD15_NVRAM_PHYS    0x1E4AA000u  /* KSEG1 0xBE4AA000 — 93C46 bit-bang */
#define NCD15_MEMCTL_PHYS   0xFFFE0000u  /* KSEG2 pass-through on R3052 */
#define NCD15_VIDCTL_PHYS   0x0F000000u  /* KSEG1 0xAF000000 */

/* --- Minimal I-cache for the R3052 ---
 *
 * Direct-mapped, 4 KB, 16-byte lines (4 instructions/line, 256 sets).
 * Populated from DRAM on cached fetches. NOT invalidated by uncached
 * (KSEG1) stores — that's the whole point of having it: the monitor's
 * memory test writes 0x5A5A5A5A through uncached aliases to check
 * DRAM readback, while cached fetches keep running the original code
 * that was already loaded into the I-cache.
 *
 * Invalidated on cached stores (KSEG0/KUSEG) and on explicit cache
 * ops via CP0. For simplicity we flush the whole cache when the
 * monitor writes to the Status register's cache-isolation bits. */

#define ICACHE_SETS      256
#define ICACHE_LINE_BYTES 16
#define ICACHE_WORDS_PER_LINE (ICACHE_LINE_BYTES / 4)

typedef struct icache_line {
    u32 tag;              /* upper bits of physical address; 0xFFFFFFFF = invalid */
    u32 words[ICACHE_WORDS_PER_LINE];
} icache_line;

typedef struct icache {
    icache_line sets[ICACHE_SETS];
    u64 hits, misses;
} icache;

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
    icache icache;
} mips_cpu;

/* Called by the bus when it services a store that could invalidate
 * cache lines — e.g. a cached KSEG0/KUSEG write. Pass the physical
 * address. The bus decides whether to call based on the access path. */
void icache_invalidate(icache *c, u32 pa);
void icache_flush_all(icache *c);

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
    u64 read_count;       /* reads tallied for hot-device diagnostics */
    u64 write_count;
} mmio_region;

#define MAX_MMIO_REGIONS 16

typedef struct bus {
    u8 *rom;              /* NCD15_ROM_SIZE bytes, big-endian order */
    u8 *dram;             /* NCD15_DRAM_SIZE bytes, main DRAM at phys 0 */
    u8 *shadow;           /* 4 MiB, backs phys 0x0EC00000..0x0F000000 */
    u8 *vram;             /* NCD15_VRAM_SIZE bytes, at phys 0x0F000000 */
    u8 *lance_shmem;      /* 128 KB, LANCE-side shared memory (byte addressed) */
    mmio_region regions[MAX_MMIO_REGIONS];
    size_t nregions;
    bool trace;           /* log every access */
    u32 last_pc;          /* updated by the CPU before each access */
    u32 last_ra;          /* $ra at the moment of access — for tracing callers */
    u32 call_stack[16];   /* shallow JAL/JALR call-target stack for diagnostics */
    int call_depth;
    u64 cur_cycles;       /* updated by CPU each step (for tracing) */
    /* User-supplied network config, injected into the monitor's
     * runtime IP-config slots via memory-read intercepts. Zero
     * means "no override". */
    u32 inject_ip;        /* data_0x0EC000C8 */
    u32 inject_mask;      /* data_0x0EC000EC */
    u32 inject_server;    /* data_0x0EC00104 */
    u32 inject_gateway;   /* data_0x0EC000CC */
    u8 *flash;            /* linear-flash card buffer (KSEG1 0xBF800000+) */
    size_t flash_size;
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
int  duart_rx_empty(struct duart *d, int channel);
struct nvram *duart_nvram(struct duart *d);

/* --- Video control registers (0xAF000000..3 cart-ID etc.) --- */

struct vidctl;
struct vidctl *vidctl_new(void);
u32  vidctl_read(void *ctx, u32 offset, unsigned size);
void vidctl_write(void *ctx, u32 offset, u32 value, unsigned size);

/* --- LANCE Ethernet (Am79C90) at 0xBE482000 --- */
void *lance_glue_new(bus *b);
u32   lance_glue_read(void *ctx, u32 off, unsigned sz);
void  lance_glue_write(void *ctx, u32 off, u32 v, unsigned sz);
/* Wire an outgoing-frame callback. `send` takes (buf, length, ctx). */
void  lance_glue_set_send(void *glue,
                          void (*send)(const u8 *buf, int len, void *ctx),
                          void *ctx);
/* Deliver a received frame from host-side networking into the chip. */
void  lance_glue_recv(void *glue, const u8 *buf, int len);
/* Advance the LANCE chip by N CPU cycles. Drives the deferred IDON
 * after INIT and the periodic TX-poll. */
void  lance_glue_tick(void *glue, int cycles);
void  lance_glue_mirror_csr0(void *glue);

/* --- Memory controller stub (0xFFFE0000) --- */

struct memctl;
struct memctl *memctl_new(void);
u32  memctl_read(void *ctx, u32 offset, unsigned size);
void memctl_write(void *ctx, u32 offset, u32 value, unsigned size);

/* --- CRTC stub (0xBE380000). Toggles the vsync bit so the monitor's
 * vsync-polling loops advance. --- */
struct crtc;
struct crtc *crtc_new(bus *b);
u32  crtc_read(void *ctx, u32 offset, unsigned size);
void crtc_write(void *ctx, u32 offset, u32 value, unsigned size);
void crtc_dump_hist(void *ctx);

/* --- 93C46 NVRAM bit-bang, with the keyboard serial line multiplexed
 * through the same 7407 buffer at 0xAEC80000. Minimal device; exact
 * pin layout is a best guess since we don't have real bus traces. --- */
struct nvram;
struct nvram *nvram_new(void);
u32  nvram_read(void *ctx, u32 offset, unsigned size);
void nvram_write(void *ctx, u32 offset, u32 value, unsigned size);
/* Serialize the 64 × 16-bit EEPROM memory to/from a 128-byte flat file
 * (big-endian words). Returns 0 on success. The file format matches
 * what `nvram_save` writes — no header, no endianness markers, just
 * 128 raw bytes. */
int  nvram_load_file(struct nvram *n, const char *path);
int  nvram_save_file(struct nvram *n, const char *path);
/* Direct word access for tooling / initial-image setup. */
void nvram_set_word(struct nvram *n, unsigned addr, u16 val);
u16  nvram_get_word(struct nvram *n, unsigned addr);

/* --- SDL2 framebuffer window (src/fb.c) --- */
void fb_init(int enabled);
void fb_tick(bus *b);
int  fb_should_quit(void);
int  fb_poll_input(unsigned char *out);

#endif
