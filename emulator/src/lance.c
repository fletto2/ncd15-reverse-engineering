/*
 * lance.c -- AMD Am7990 / Am79C90 LANCE Ethernet controller emulation
 *
 * License: BSD-3-Clause (inherited from MAME)
 * Original (C++): Patrick Mackinlay (MAME team), src/devices/machine/am79c90.cpp
 * C port: 3Com CS/200x emulator
 *
 * This is a faithful translation of MAME's am79c90 implementation to plain C,
 * stripped of MAME-specific machinery (devices, attotime, devcb, scheduler,
 * save state, network_interface integration). The host wires four function
 * pointers for DMA in/out, interrupt line, and packet transmit.
 *
 * The chip is a bus master: when the CPU writes the INIT bit in CSR0, the
 * LANCE walks through the init block (12 words at the address in CSR1/CSR2)
 * fetching MODE, PADR, LADRF, RDRA+RLEN, TDRA+TLEN. After init it polls the
 * TX descriptor ring for OWN-by-LANCE entries and transmits any frames it
 * finds; it also accepts incoming frames via lance_recv() and writes them
 * into RX descriptors based on owner/length checks.
 *
 * For the 3Com emulator's use case (passing the OEM CSR2 bit-walk POST and
 * letting the firmware reach its monitor) we set send_cb to NULL and the
 * MODE_INTL (internal loopback) bit causes transmitted packets to circle
 * back through the receive path without ever touching the host network.
 */
#include <stdio.h>
#include <string.h>
#include "lance.h"

/* ---- Logging ---- */

#define LOG_LANCE 0
#if LOG_LANCE
#  define LOG(...)  fprintf(stderr, "[LANCE] " __VA_ARGS__)
#else
#  define LOG(...)  ((void)0)
#endif

/* ---- Constants ---- */

#define INIT_ADDR_MASK   0xFFFFFEU
#define RING_ADDR_MASK   0xFFFFF8U

/* TX poll period: MAME uses 1600 us. At 12 MHz this is 19200 CPU cycles.
 * For our purposes the exact period isn't critical — anything in the
 * "few hundred to few thousand cycles" range is fine. */
#define LANCE_TX_POLL_CYCLES 19200

/* Forward decls */
static void lance_initialize(lance_t *l);
static void lance_update_interrupts(lance_t *l);
static void lance_transmit_poll(lance_t *l);
static void lance_transmit(lance_t *l);
static int  lance_receive(lance_t *l, const uint8_t *buf, int length);
static void lance_recv_complete(lance_t *l, int result);
static void lance_send_complete(lance_t *l, int result);
static void lance_dma_in_block(lance_t *l, uint32_t addr, uint8_t *buf, int length);
static void lance_dma_out_block(lance_t *l, uint32_t addr, const uint8_t *buf, int length);
static int  lance_address_filter(lance_t *l, const uint8_t *buf);
static int  lance_get_buf_length(lance_t *l, uint16_t data);

/* ---- Public API ---- */

void lance_init(lance_t *l, int variant)
{
    memset(l, 0, sizeof(*l));
    l->variant = variant;
    l->intr_out_state = 1;          /* /INT deasserted */
    lance_reset(l);
}

void lance_set_callbacks(lance_t *l,
                         lance_dma_in_fn dma_in,
                         lance_dma_out_fn dma_out,
                         lance_intr_fn intr,
                         lance_send_fn send,
                         void *ctx)
{
    l->dma_in_cb  = dma_in;
    l->dma_out_cb = dma_out;
    l->intr_out_cb= intr;
    l->send_cb    = send;
    l->cb_ctx     = ctx;
}

void lance_reset(lance_t *l)
{
    l->rap     = 0;
    l->csr[0]  = LANCE_CSR0_STOP;
    l->csr[1]  = 0;
    l->csr[2]  = 0;
    l->csr[3]  = 0;

    l->mode    = 0;
    l->lb_length = 0;
    l->idon    = 0;

    l->tx_poll_counter = LANCE_TX_POLL_CYCLES;

    lance_update_interrupts(l);
}

void lance_reset_w(lance_t *l, int state)
{
    if (!state)
        lance_reset(l);
}

/* ---- Bus accessors (CPU side) ---- */

uint16_t lance_regs_r(lance_t *l, int offset)
{
    if (offset == 0) {
        /* RDP — return whichever CSR is selected by RAP */
        uint16_t val;
        if (l->rap && !(l->csr[0] & LANCE_CSR0_STOP))
            val = 0xFFFF;           /* unmapped per MAME */
        else
            val = l->csr[l->rap];
        return val;
    } else {
        /* RAP — read back the register pointer */
        return l->rap;
    }
}

void lance_regs_w(lance_t *l, int offset, uint16_t data)
{
    if (offset != 0) {
        l->rap = data & 3;
        return;
    }

    LOG("regs_w csr%d = 0x%04x\n", l->rap, data);

    switch (l->rap) {
    case 0: /* CSR0 - control/status */
    {
        /* STOP takes priority. Setting STOP triggers a reset of the chip. */
        if (data & LANCE_CSR0_STOP) {
            if (!(l->csr[0] & LANCE_CSR0_STOP))
                lance_reset(l);
            break;
        }

        /* Clear-on-write-1 bits: BABL, CERR, MISS, MERR, RINT, TINT, IDON */
        l->csr[0] &= ~(data & (LANCE_CSR0_BABL | LANCE_CSR0_CERR |
                               LANCE_CSR0_MISS | LANCE_CSR0_MERR |
                               LANCE_CSR0_RINT | LANCE_CSR0_TINT |
                               LANCE_CSR0_IDON));

        /* INIT — load and parse the init block.
         * Rising edge of INIT always re-initializes. The firmware's
         * POST does back-to-back inits without going through STOP, and
         * the real AM79C90 handles this by re-running the init sequence
         * (it will briefly TX/RX-off during the re-init). */
        if ((data & LANCE_CSR0_INIT) && !(l->csr[0] & LANCE_CSR0_INIT)) {
            lance_initialize(l);
        }

        /* STRT — start the receiver/transmitter */
        if ((data & LANCE_CSR0_STRT) && !(l->csr[0] & LANCE_CSR0_STRT)) {
            LOG("STRT receiver %s transmitter %s\n",
                (l->mode & LANCE_MODE_DRX) ? "OFF" : "ON",
                (l->mode & LANCE_MODE_DTX) ? "OFF" : "ON");

            l->csr[0] |= LANCE_CSR0_STRT;
            l->csr[0] &= ~LANCE_CSR0_STOP;

            if (l->mode & LANCE_MODE_DRX)
                l->csr[0] &= ~LANCE_CSR0_RXON;
            else
                l->csr[0] |= LANCE_CSR0_RXON;

            if (l->mode & LANCE_MODE_DTX)
                l->csr[0] &= ~LANCE_CSR0_TXON;
            else
                l->csr[0] |= LANCE_CSR0_TXON;

            /* Trigger an immediate transmit poll on the next tick */
            l->tx_poll_counter = 1;
        }

        /* TDMD — transmit demand */
        if ((data & LANCE_CSR0_TDMD) && !(l->csr[0] & LANCE_CSR0_TDMD)) {
            l->csr[0] |= LANCE_CSR0_TDMD;
            l->tx_poll_counter = 1;
        }

        /* INEA — interrupt enable (read/write while not stopped) */
        if (!(l->csr[0] & LANCE_CSR0_STOP) || l->variant == LANCE_AM79C90) {
            if (data & LANCE_CSR0_INEA)
                l->csr[0] |= LANCE_CSR0_INEA;
            else
                l->csr[0] &= ~LANCE_CSR0_INEA;
        }

        /* Recompute composite ERR and INTR flags */
        if (l->csr[0] & (LANCE_CSR0_BABL | LANCE_CSR0_CERR |
                         LANCE_CSR0_MISS | LANCE_CSR0_MERR))
            l->csr[0] |= LANCE_CSR0_ERR;
        else
            l->csr[0] &= ~LANCE_CSR0_ERR;

        if (l->csr[0] & (LANCE_CSR0_BABL | LANCE_CSR0_MISS | LANCE_CSR0_MERR |
                         LANCE_CSR0_RINT | LANCE_CSR0_TINT | LANCE_CSR0_IDON))
            l->csr[0] |= LANCE_CSR0_INTR;
        else
            l->csr[0] &= ~LANCE_CSR0_INTR;

        lance_update_interrupts(l);
        break;
    }

    case 1: /* CSR1 — init block address bits 15:00 */
        if (l->csr[0] & LANCE_CSR0_STOP)
            l->csr[1] = data;
        break;

    case 2: /* CSR2 — init block address bits 23:16 (+ reserved high bits).
           * The real AM79C90 stores the full 16-bit word written to CSR2,
           * not just the low 8 bits. The firmware's CSR2 walk test writes
           * walking-1 patterns through all 16 bits and reads them back.
           * The MAME source had this masked to 0xFF for AM79C90 but that
           * was wrong — it prevents the walk test from passing for bits
           * 8-15. Only bits 7:0 are USED for the init-block address, but
           * the register itself is 16-bit read/write. */
        if (l->csr[0] & LANCE_CSR0_STOP)
            l->csr[2] = data;
        break;

    case 3: /* CSR3 — bus interface mode */
        if (l->csr[0] & LANCE_CSR0_STOP)
            l->csr[3] = data & 0x07;    /* BCON | ACON | BSWP */
        break;
    }
}

/* ---- Periodic tick from the host main loop ---- */

/* Number of CPU cycles to delay IDON after INIT. On real hardware this
 * is the time for the LANCE to DMA the 24-byte init block (12 word
 * reads × ~4 bus cycles each = ~48 bus cycles ≈ ~200 CPU cycles at
 * 12 MHz). We use a slightly larger value to be safe. */
#define LANCE_INIT_DELAY_CYCLES 500

void lance_tick(lance_t *l, int cycles)
{
    if (cycles <= 0) return;

    /* Deferred IDON: if lance_initialize set idon=1 but didn't put
     * IDON into CSR0 yet, count down and then apply it. */
    if (l->idon) {
        static int init_delay = LANCE_INIT_DELAY_CYCLES;
        init_delay -= cycles;
        if (init_delay <= 0) {
            /* Init done: clear INIT (self-clearing on real hw), set IDON
             * and INTR, and consume the one-shot idon flag so we don't
             * re-fire when the ISR clears IDON via W1C. */
            l->csr[0] &= ~LANCE_CSR0_INIT;
            l->csr[0] |= LANCE_CSR0_IDON | LANCE_CSR0_INTR;
            l->idon = 0;
            lance_update_interrupts(l);
            init_delay = LANCE_INIT_DELAY_CYCLES; /* reset for next init */
        }
    }

    if (l->tx_poll_counter > 0) {
        l->tx_poll_counter -= cycles;
        if (l->tx_poll_counter <= 0) {
            l->tx_poll_counter = LANCE_TX_POLL_CYCLES;
            lance_transmit_poll(l);
        }
    }
}

/* ---- Internal: interrupt management ---- */

static void lance_update_interrupts(lance_t *l)
{
    int new_state = l->intr_out_state;

    if (l->csr[0] & LANCE_CSR0_INEA) {
        /* Assert /INT (state=0) when CSR0_INTR is set */
        if (l->csr[0] & LANCE_CSR0_INTR)
            new_state = 0;
        else
            new_state = 1;
    } else {
        /* Interrupts disabled — deassert */
        new_state = 1;
    }

    if (new_state != l->intr_out_state) {
        l->intr_out_state = new_state;
        if (l->intr_out_cb)
            l->intr_out_cb(new_state, l->cb_ctx);
        LOG("interrupt %s\n", new_state ? "deasserted" : "asserted");
    }
}

/* ---- Initialize: parse the init block ---- */

static void lance_initialize(lance_t *l)
{
    uint32_t init_addr = (((uint32_t)l->csr[2] << 16) | l->csr[1]) & INIT_ADDR_MASK;
    uint16_t init_block[12];

    LOG("INITIALIZE init block @ 0x%06x\n", init_addr);

    if (!l->dma_in_cb) {
        /* Can't init without a host bus callback */
        return;
    }

    for (int i = 0; i < 12; i++)
        init_block[i] = l->dma_in_cb(init_addr + i * 2, l->cb_ctx);

    l->mode = init_block[0];

    /* PADR (physical address) - 6 bytes from init_block[1..3] in LE order */
    l->physical_addr[0] =  init_block[1]       & 0xFF;
    l->physical_addr[1] = (init_block[1] >> 8) & 0xFF;
    l->physical_addr[2] =  init_block[2]       & 0xFF;
    l->physical_addr[3] = (init_block[2] >> 8) & 0xFF;
    l->physical_addr[4] =  init_block[3]       & 0xFF;
    l->physical_addr[5] = (init_block[3] >> 8) & 0xFF;

    /* LADRF (logical address filter) - 8 bytes from init_block[4..7] */
    l->logical_addr_filter =
          ((uint64_t)init_block[7] << 48)
        | ((uint64_t)init_block[6] << 32)
        | ((uint64_t)init_block[5] << 16)
        |  (uint64_t)init_block[4];

    /* RX/TX descriptor ring base addresses + lengths */
    l->rx_ring_base = (((uint32_t)init_block[9]  << 16) | init_block[8])  & RING_ADDR_MASK;
    l->tx_ring_base = (((uint32_t)init_block[11] << 16) | init_block[10]) & RING_ADDR_MASK;
    l->rx_ring_mask = (uint8_t)~(1u << ((init_block[9]  >> 13) & 7));
    l->tx_ring_mask = (uint8_t)~(1u << ((init_block[11] >> 13) & 7));

    l->rx_ring_pos = 0;
    l->tx_ring_pos = 0;

    fprintf(stderr, "[LANCE INIT] addr=$%06X mode=$%04X PADR=%02x:%02x:%02x:%02x:%02x:%02x\n",
        init_addr, l->mode,
        l->physical_addr[0], l->physical_addr[1], l->physical_addr[2],
        l->physical_addr[3], l->physical_addr[4], l->physical_addr[5]);
    fprintf(stderr, "[LANCE INIT] rx_ring=$%06X len=%d  tx_ring=$%06X len=%d\n",
        l->rx_ring_base, 1 << ((init_block[9]  >> 13) & 7),
        l->tx_ring_base, 1 << ((init_block[11] >> 13) & 7));
    fprintf(stderr, "[LANCE INIT] raw: ");
    for (int i = 0; i < 12; i++) fprintf(stderr, "%04X ", init_block[i]);
    fprintf(stderr, "\n");

    /* Don't set IDON immediately — on real hardware the DMA takes many
     * bus cycles. Defer to lance_tick() so the CPU has time to execute
     * instructions between the CSR0 write and the IDON appearance.
     * The firmware at $1AE4 runs a 65536-iteration delay loop after
     * writing CSR0, expecting IDON to appear during or after that delay.
     * If we set IDON here synchronously, the /INT fires before the
     * firmware has a chance to execute the next instruction. */
    l->csr[0] |= LANCE_CSR0_INIT;
    l->csr[0] &= ~(LANCE_CSR0_STOP | LANCE_CSR0_STRT |
                   LANCE_CSR0_TXON | LANCE_CSR0_RXON);

    l->idon = 1;
    /* IDON will be applied by lance_tick after a short delay
     * (see LANCE_INIT_DELAY_CYCLES below). */
}

/* ---- Buffer length helper (variant-specific) ---- */

static int lance_get_buf_length(lance_t *l, uint16_t data)
{
    /* AM7990: get_buf_length(data) = (data == 0xf000) ? 4096 : -s16(0xf000 | data)
     * AM79C90: same except returns 0 if data == 0
     */
    if (l->variant == LANCE_AM79C90 && data == 0)
        return 0;
    if (data == 0xF000)
        return 4096;
    /* The encoding is two's-complement: bytes_remaining = -value, where value
     * is sign-extended from 12 bits with the high 4 bits forced to 1. */
    int16_t v = (int16_t)(0xF000 | data);
    return -v;
}

/* ---- DMA helpers (block-mode read/write into host memory) ---- */

static void lance_dma_in_block(lance_t *l, uint32_t addr, uint8_t *buf, int length)
{
    int bswap = (l->csr[3] & 0x04) != 0;        /* CSR3 BSWP bit */

    /* Odd start address: read one byte */
    if (addr & 1) {
        uint16_t w = l->dma_in_cb(addr & ~1u, l->cb_ctx);
        buf[0] = bswap ? (w & 0xFF) : (w >> 8);
        buf++; addr++; length--;
    }

    while (length > 1) {
        uint16_t w = l->dma_in_cb(addr, l->cb_ctx);
        if (bswap) {
            buf[0] = w >> 8;
            buf[1] = w & 0xFF;
        } else {
            buf[0] = w & 0xFF;
            buf[1] = w >> 8;
        }
        buf += 2; addr += 2; length -= 2;
    }

    /* Trailing byte */
    if (length) {
        uint16_t w = l->dma_in_cb(addr, l->cb_ctx);
        buf[0] = bswap ? (w >> 8) : (w & 0xFF);
    }
}

static void lance_dma_out_block(lance_t *l, uint32_t addr, const uint8_t *buf, int length)
{
    int bswap = (l->csr[3] & 0x04) != 0;

    if (addr & 1) {
        if (bswap)
            l->dma_out_cb(addr & ~1u, buf[0], l->cb_ctx);
        else
            l->dma_out_cb(addr & ~1u, (uint16_t)buf[0] << 8, l->cb_ctx);
        buf++; addr++; length--;
    }

    while (length > 1) {
        uint16_t w;
        if (bswap)
            w = ((uint16_t)buf[0] << 8) | buf[1];
        else
            w = ((uint16_t)buf[1] << 8) | buf[0];
        l->dma_out_cb(addr, w, l->cb_ctx);
        buf += 2; addr += 2; length -= 2;
    }

    if (length) {
        if (bswap)
            l->dma_out_cb(addr, (uint16_t)buf[0] << 8, l->cb_ctx);
        else
            l->dma_out_cb(addr, buf[0], l->cb_ctx);
    }
}

/* ---- CRC-32 (Ethernet, polynomial 0xEDB88320, residue 0xDEBB20E3) ---- */

static uint32_t lance_crc32(const uint8_t *buf, int length)
{
    static uint32_t table[256];
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        initialized = 1;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < length; i++)
        crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- Address filter (called on every received packet) ---- */

static const uint8_t LANCE_ETH_BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static int lance_address_filter(lance_t *l, const uint8_t *buf)
{
    if (l->mode & LANCE_MODE_PROM)
        return 1;       /* promiscuous mode */

    if (buf[0] & 1) {
        /* Multicast/broadcast destination address */
        if (memcmp(LANCE_ETH_BROADCAST, buf, 6) == 0)
            return 1;

        /* Multicast — index by upper 6 bits of FCS of the destination */
        uint32_t crc = lance_crc32(buf, 6);
        int bit = 63 - (crc >> 26);
        if ((l->logical_addr_filter >> bit) & 1)
            return 1;
    } else {
        /* Unicast — match against PADR */
        if (memcmp(l->physical_addr, buf, 6) == 0)
            return 1;
    }
    return 0;
}

/* ---- Receive path (lance_recv → lance_receive → lance_recv_complete) ---- */

int lance_recv(lance_t *l, const uint8_t *buf, int length)
{
    /* Discard short packets */
    if (length < 64) {
        LOG("recv: runt packet length %d discarded\n", length);
        return -1;
    }

    /* If we're in pure internal loopback mode, the chip ignores the wire. */
    if ((l->mode & LANCE_MODE_LOOP) && (l->mode & LANCE_MODE_INTL)) {
        return -1;
    }

    int result = lance_receive(l, buf, length);
    lance_recv_complete(l, result);
    return (result < 0) ? result : 0;
}

static int lance_receive(lance_t *l, const uint8_t *buf, int length)
{
    if (!(l->csr[0] & LANCE_CSR0_RXON))
        return -1;
    if (!lance_address_filter(l, buf))
        return -1;
    if (!l->dma_in_cb || !l->dma_out_cb)
        return -1;

    LOG("receive packet length %d\n", length);

    /* Fetch the current RX descriptor */
    uint32_t ring_address = (l->rx_ring_base + ((uint32_t)l->rx_ring_pos << 3)) & RING_ADDR_MASK;
    l->rx_md[1] = l->dma_in_cb(ring_address | 2, l->cb_ctx);

    if (!(l->rx_md[1] & 0x8000))    /* RMD1_OWN */
        return -2;                  /* missed packet — chip doesn't own it */

    /* Mark this descriptor as Start-of-Packet */
    l->rx_md[1] |= 0x0200;          /* RMD1_STP */

    int offset = 0;
    while (offset < length) {
        l->rx_md[0] = l->dma_in_cb(ring_address | 0, l->cb_ctx);
        l->rx_md[2] = l->dma_in_cb(ring_address | 4, l->cb_ctx);

        uint32_t rx_buf_address = ((uint32_t)(l->rx_md[1] & 0xFF) << 16) | l->rx_md[0];
        int rx_buf_length = lance_get_buf_length(l, l->rx_md[2]);
        if (rx_buf_length <= 0) rx_buf_length = 64;

        int count = (length - offset < rx_buf_length) ? (length - offset) : rx_buf_length;
        lance_dma_out_block(l, rx_buf_address, &buf[offset], count);
        offset += count;

        l->rx_md[1] &= ~0x8000u;    /* clear OWN */

        if (offset < length) {
            /* Look ahead to next descriptor */
            uint8_t next_pos = (l->rx_ring_pos + 1) & l->rx_ring_mask;
            uint32_t next_addr = (l->rx_ring_base + ((uint32_t)next_pos << 3)) & RING_ADDR_MASK;
            uint16_t next_md1 = (next_addr != ring_address)
                              ? l->dma_in_cb(next_addr | 2, l->cb_ctx)
                              : l->rx_md[1];

            if (next_md1 & 0x8000) {
                /* Update the current descriptor and advance */
                l->dma_out_cb(ring_address | 2, l->rx_md[1], l->cb_ctx);
                l->rx_ring_pos = next_pos;
                ring_address   = next_addr;
                l->rx_md[1]    = next_md1;
            } else {
                /* Buffer-error overflow */
                l->rx_md[1] |= 0x4400u;     /* ERR | BUFF */
                break;
            }
        }
    }

    if (offset == length) {
        /* Verify FCS unless we're skipping CRC for some reason */
        if (!(l->mode & LANCE_MODE_LOOP) || (l->mode & LANCE_MODE_DTCR)) {
            uint32_t crc = lance_crc32(buf, length);
            /* Residue magic for a correctly-FCS'd frame, with our
             * lance_crc32 returning the inverted (standard Ethernet) CRC. */
            if (crc != 0x2144DF1Cu) {
                LOG("receive: incorrect FCS 0x%08x\n", crc);
                l->rx_md[1] |= 0x4800u;     /* ERR | CRC */
            }
        }
        l->rx_md[1] |= 0x0100u;             /* RMD1_ENP */
    }

    return offset;
}

static void lance_recv_complete(lance_t *l, int result)
{
    switch (result) {
    case -2:    /* missed packet */
        l->csr[0] |= LANCE_CSR0_ERR | LANCE_CSR0_MISS;
        break;
    case -1:    /* discarded or filtered */
        return;
    default:    /* received something */
    {
        uint32_t ring_address = (l->rx_ring_base + ((uint32_t)l->rx_ring_pos << 3)) & RING_ADDR_MASK;
        l->dma_out_cb(ring_address | 2, l->rx_md[1], l->cb_ctx);
        /* Always write MCNT — the firmware's CRC-error POST test checks
         * that MCNT still reflects the received length even on ERR. */
        l->dma_out_cb(ring_address | 6, (uint16_t)result & 0x0FFF, l->cb_ctx);
        l->rx_ring_pos = (l->rx_ring_pos + 1) & l->rx_ring_mask;
        break;
    }
    }

    l->csr[0] |= LANCE_CSR0_RINT | LANCE_CSR0_INTR;
    lance_update_interrupts(l);
}

/* ---- Transmit path (transmit_poll → transmit → send_complete) ---- */

static void lance_transmit_poll(lance_t *l)
{
    if (l->csr[0] & LANCE_CSR0_TXON) {
        l->csr[0] &= ~LANCE_CSR0_TDMD;

        if (!l->dma_in_cb) goto check_loopback;

        uint32_t ring_address = (l->tx_ring_base + ((uint32_t)l->tx_ring_pos << 3)) & RING_ADDR_MASK;
        l->tx_md[1] = l->dma_in_cb(ring_address | 2, l->cb_ctx);

        if (l->tx_md[1] & 0x8000u) {            /* TMD1_OWN */
            if (!(l->tx_md[1] & 0x0200u)) {     /* TMD1_STP */
                /* Not start-of-packet — clear OWN and advance */
                l->dma_out_cb(ring_address | 2, l->tx_md[1] & ~0x8000u, l->cb_ctx);
                l->tx_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;
            } else {
                lance_transmit(l);
            }
        }
    }

check_loopback:
    /* Pending loopback data → feed it back into the receive path */
    if (l->lb_length && (l->mode & LANCE_MODE_LOOP)) {
        LOG("loopback: receive %d bytes\n", l->lb_length);
        int result = lance_receive(l, l->lb_buf, l->lb_length);
        l->lb_length = 0;
        lance_recv_complete(l, result);
    }
}

static void lance_transmit(lance_t *l)
{
    int append_fcs = !(l->mode & LANCE_MODE_DTCR);
    if (l->variant == LANCE_AM79C90)
        append_fcs = append_fcs || (l->tx_md[1] & 0x2000);  /* TMD1_ADD_FCS */
    else
        l->tx_md[1] &= ~0x2000u;

    uint32_t ring_address = (l->tx_ring_base + ((uint32_t)l->tx_ring_pos << 3)) & RING_ADDR_MASK;
    uint8_t buf[4096];
    int length = 0;

    while (1) {
        l->tx_md[0] = l->dma_in_cb(ring_address | 0, l->cb_ctx);
        l->tx_md[2] = l->dma_in_cb(ring_address | 4, l->cb_ctx);
        l->tx_md[3] = 0;

        uint32_t tx_buf_address = ((uint32_t)(l->tx_md[1] & 0xFF) << 16) | l->tx_md[0];
        int tx_buf_length = lance_get_buf_length(l, l->tx_md[2]);

        l->tx_md[1] &= ~0x8000u;    /* clear OWN */

        if (tx_buf_length <= 0) {
            /* Zero-length buffer — drop and advance */
            l->dma_out_cb(ring_address | 2, l->tx_md[1], l->cb_ctx);
            l->tx_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;
            return;
        }

        if (length + tx_buf_length > (int)sizeof(buf))
            tx_buf_length = sizeof(buf) - length;
        if (tx_buf_length > 0) {
            lance_dma_in_block(l, tx_buf_address, &buf[length], tx_buf_length);
            length += tx_buf_length;
        }

        if (!(l->tx_md[1] & 0x0100u)) {   /* TMD1_ENP */
            /* Multi-buffer chain: look ahead */
            uint8_t next_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;
            uint32_t next_addr = (l->tx_ring_base + ((uint32_t)next_pos << 3)) & RING_ADDR_MASK;
            uint16_t next_tmd1 = (next_addr != ring_address)
                               ? l->dma_in_cb(next_addr | 2, l->cb_ctx)
                               : l->tx_md[1];
            if (next_tmd1 & 0x8000u) {
                l->dma_out_cb(ring_address | 2, l->tx_md[1], l->cb_ctx);
                l->tx_ring_pos = next_pos;
                ring_address   = next_addr;
                l->tx_md[1]    = next_tmd1;
            } else {
                /* Buffer error - underrun */
                l->tx_md[1] |= 0x4000u;             /* TMD1_ERR */
                l->tx_md[3] |= 0x4000u | 0x8000u;   /* UFLO | BUFF */
                l->dma_out_cb(ring_address | 6, l->tx_md[3], l->cb_ctx);
                l->csr[0] &= ~LANCE_CSR0_TXON;
                break;
            }
        } else {
            break;
        }
    }

    /* Babble */
    if (length > 1518)
        l->csr[0] |= LANCE_CSR0_ERR | LANCE_CSR0_BABL;

    /* Append CRC if asked */
    if (append_fcs) {
        uint32_t crc = lance_crc32(buf, length);
        buf[length++] = crc & 0xFF;
        buf[length++] = (crc >> 8) & 0xFF;
        buf[length++] = (crc >> 16) & 0xFF;
        buf[length++] = (crc >> 24) & 0xFF;
    }

    LOG("transmit: %d bytes\n", length);

    /* Loopback handling */
    if (l->mode & LANCE_MODE_LOOP) {
        if ((l->mode & LANCE_MODE_COLL) && (l->mode & LANCE_MODE_INTL)) {
            lance_send_complete(l, -1);
            return;
        }
        int fcs_length = append_fcs ? 4 : 0;
        if ((length - fcs_length) < 8 || (length - fcs_length) > 32) {
            LOG("invalid loopback packet length %d\n", length - fcs_length);
            lance_send_complete(l, -2);
            return;
        }
        if (length > (int)sizeof(l->lb_buf))
            length = sizeof(l->lb_buf);
        memcpy(l->lb_buf, buf, length);
        l->lb_length = length;
        if (l->mode & LANCE_MODE_INTL) {
            lance_send_complete(l, length);
            return;
        }
    }

    /* External send */
    if (l->send_cb)
        l->send_cb(buf, length, l->cb_ctx);
    lance_send_complete(l, length);
}

static void lance_send_complete(lance_t *l, int result)
{
    uint32_t ring_address = (l->tx_ring_base + ((uint32_t)l->tx_ring_pos << 3)) & RING_ADDR_MASK;

    switch (result) {
    case -2:    /* invalid loopback packet */
        l->tx_md[1] |= 0x4000u;     /* TMD1_ERR */
        break;
    case -1:    /* forced collision */
        l->tx_md[1] |= 0x4000u;
        l->tx_md[3] |= 0x0400u;     /* TMD3_RTRY */
        l->dma_out_cb(ring_address | 6, l->tx_md[3], l->cb_ctx);
        break;
    case 0:     /* loss of carrier */
        l->tx_md[1] |= 0x4000u;
        l->tx_md[3] |= 0x0800u;     /* TMD3_LCAR */
        l->dma_out_cb(ring_address | 6, l->tx_md[3], l->cb_ctx);
        break;
    }

    l->dma_out_cb(ring_address | 2, l->tx_md[1], l->cb_ctx);
    l->tx_ring_pos = (l->tx_ring_pos + 1) & l->tx_ring_mask;

    l->csr[0] |= LANCE_CSR0_TINT | LANCE_CSR0_INTR;
    lance_update_interrupts(l);

    /* Resume transmit polling for back-to-back packets */
    l->tx_poll_counter = 1;
}
