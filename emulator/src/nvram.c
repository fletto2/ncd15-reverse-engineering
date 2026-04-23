/* 93C46 serial EEPROM emulation, bit-banged through the 7407 buffer
 * at 0xAEC80000.
 *
 * The 93C46 is a 1 Kbit serial EEPROM organized as 64 × 16-bit words
 * (in x16 ORG mode, which the NCD15 uses). Serial interface:
 *
 *   CS  — chip select (high = active)
 *   SK  — serial clock (shift on rising edge)
 *   DI  — data from CPU to chip
 *   DO  — data from chip to CPU
 *
 * Command format (after CS rising edge):
 *
 *   START(1) | OP(2) | ADDR(6)  [| DATA(16) for writes]
 *
 * Opcodes (per NM93C46 datasheet, x16 ORG=1 mode):
 *   10: READ  → chip outputs leading dummy 0 + 16 data bits MSB-first
 *   01: WRITE → CPU supplies 16 data bits; chip commits when CS drops
 *   11: ERASE → sets the addressed word to 0xFFFF
 *   00 | addr[5:4] = 11: EWEN  (write-enable)
 *   00 | addr[5:4] = 00: EWDS  (write-disable)
 *   00 | addr[5:4] = 10: ERAL  (erase-all)
 *   00 | addr[5:4] = 01: WRAL  (write-all, 16 data bits follow)
 *
 * Pin layout at 0xBE4AA000 (confirmed from emulator bit-pattern trace
 * during monitor boot — the monitor DOES access NVRAM during boot,
 * not through the data_0x0EC008D8 fn-ptr path but via a direct MMIO
 * sequence at 0xBE4AA000, NOT 0xAEC80000 as CLAUDE.md speculated):
 *
 *   writes to byte 0xBE4AA000:
 *     bit 0 = DI   (data from CPU to chip)
 *     bit 1 = SK   (serial clock)
 *     bit 2 = CS   (chip select, active high)
 *   reads from byte 0xBE4AA000:
 *     bit 0 = DO   (data from chip to CPU) */

#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVRAM_WORDS 64
#define NVRAM_BITS  (NVRAM_WORDS * 16)

typedef struct nvram {
    u16 mem[NVRAM_WORDS];

    /* Bit-bang state. */
    u8  cs;            /* last CS level */
    u8  sk;            /* last SK level */
    u8  di;            /* last DI level */

    /* Protocol state. */
    int  state;         /* 0 = idle, 1 = receiving cmd, 2 = read-out, 3 = write-in */
    u32  rx_shift;      /* receiving-command shift reg */
    int  rx_count;      /* bits received */
    u32  tx_shift;      /* data-out shift reg */
    int  tx_count;      /* bits shifted out */
    int  ewen;          /* erase/write enabled */
    int  addr;          /* addressed word */
} nvram;

enum { IDLE, CMD, READ_OUT, WRITE_IN };

struct nvram *nvram_new(void) {
    nvram *n = (nvram*)calloc(1, sizeof(*n));
    /* Default image: 0xFFFF in every cell (93C46 erased state). */
    for (int i = 0; i < NVRAM_WORDS; i++) n->mem[i] = 0xFFFF;
    n->ewen = 1;  /* start write-enabled (real chips boot as EWDS
                   * but our monitor doesn't EWEN first — matches
                   * hardware configs where EWEN is implicit). */
    return n;
}

static void nvram_begin_cmd(nvram *n) {
    n->state = CMD;
    n->rx_shift = 0;
    n->rx_count = 0;
    n->tx_count = 0;
}

static void nvram_sk_rise(nvram *n) {
    if (!n->cs) return;
    if (n->state == CMD) {
        /* Shift DI into rx_shift. After 9 bits we have start+OP+ADDR. */
        n->rx_shift = (n->rx_shift << 1) | (n->di & 1);
        n->rx_count++;
        if (n->rx_count < 9) return;
        /* Wait for start bit; drop leading zeros. */
        if (!(n->rx_shift & 0x100) && n->rx_count < 24) return;
        u32 cmd = n->rx_shift & 0xff;   /* 2 op + 6 addr */
        int op   = (cmd >> 6) & 3;
        int addr = cmd & 0x3f;
        n->addr = addr;
        switch (op) {
        case 2: /* READ */
            if (addr < NVRAM_WORDS) {
                n->tx_shift = n->mem[addr];
            } else n->tx_shift = 0xffff;
            n->tx_count = 0;
            n->state = READ_OUT;
            break;
        case 1: /* WRITE */
            n->rx_shift = 0;
            n->rx_count = 0;
            n->state = WRITE_IN;
            break;
        case 3: /* ERASE */
            if (n->ewen && addr < NVRAM_WORDS) n->mem[addr] = 0xffff;
            n->state = IDLE;
            break;
        case 0: /* EWEN/EWDS/ERAL/WRAL — sub-opcode in top 2 bits of addr */
            switch ((addr >> 4) & 3) {
            case 0: n->ewen = 0; break;   /* EWDS */
            case 1: /* WRAL — one more 16-bit word follows; ignored here */
                break;
            case 2: /* ERAL */
                if (n->ewen) memset(n->mem, 0xff, sizeof(n->mem));
                break;
            case 3: n->ewen = 1; break;   /* EWEN */
            }
            n->state = IDLE;
            break;
        }
    } else if (n->state == WRITE_IN) {
        n->rx_shift = (n->rx_shift << 1) | (n->di & 1);
        n->rx_count++;
        if (n->rx_count == 16) {
            if (n->ewen && n->addr < NVRAM_WORDS)
                n->mem[n->addr] = (u16)n->rx_shift;
            n->state = IDLE;
        }
    }
    /* READ_OUT: chip clocks out one bit per SK rising edge. No RX work. */
    if (n->state == READ_OUT) n->tx_count++;
}

u32 nvram_read(void *ctx, u32 offset, unsigned size) {
    nvram *n = (nvram*)ctx;
    (void)size;
    if (offset != 0) return 0;
    /* DO is tristate when not outputting — on the NCD15's 7407 buffer
     * that reads back as logic 1 (pull-up). During READ_OUT, the
     * first SK edge clocks out a dummy 0, then 16 data bits MSB-first. */
    u8 out = 1;  /* pull-up default */
    if (n->state == READ_OUT) {
        if (n->tx_count == 1) out = 0;  /* leading dummy 0 */
        else if (n->tx_count >= 2 && n->tx_count <= 17) {
            int bit = 17 - n->tx_count;
            out = (n->tx_shift >> bit) & 1;
        }
    }
    return out;
}

void nvram_write(void *ctx, u32 offset, u32 value, unsigned size) {
    nvram *n = (nvram*)ctx;
    (void)size;
    /* Silent. */
    if (offset != 0) return;
    u8 new_di = (value >> 0) & 1;
    u8 new_sk = (value >> 1) & 1;
    u8 new_cs = (value >> 2) & 1;

    /* CS rising edge → begin a new command sequence. */
    if (new_cs && !n->cs) nvram_begin_cmd(n);
    /* CS falling edge → terminate. */
    if (!new_cs && n->cs) n->state = IDLE;
    /* SK rising edge → clock. */
    if (new_sk && !n->sk && new_cs) nvram_sk_rise(n);

    n->cs = new_cs;
    n->sk = new_sk;
    n->di = new_di;
}
