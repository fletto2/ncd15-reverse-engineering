/*============================================================================
 * console_mc68681.c -- MC68681 DUART console driver.
 *
 * Used on the pvs2 test board (and any other board that wires an MC68681
 * to expose channel B as the system console).
 *
 * Default base: DUART_BASE = 0x020000 (PVS2). 16 bytes of register space.
 * Channel B is the console; channel A is exposed to BIOS aux/printer
 * functions but otherwise unused here.
 *
 * MC68681 register layout (one register per byte address):
 *   +0  MR1A/MR2A  channel A mode (auto-increments after MR1 write)
 *   +1  SRA  /CSRA status A / clock-select A
 *   +2  CRA        command A
 *   +3  RBA  /TBA  receive A / transmit A   (the data register)
 *   +4  IPCR /ACR  input port change / aux control
 *   +5  ISR  /IMR  interrupt status / interrupt mask
 *   +6  CTU  /CTUR counter upper
 *   +7  CTL  /CTLR counter lower
 *   +8  MR1B/MR2B  channel B mode
 *   +9  SRB  /CSRB status B / clock-select B
 *   +A  CRB        command B
 *   +B  RBB  /TBB  receive B / transmit B
 *   +C  IVR        interrupt vector
 *   +D  IP   /OPCR input port / output port config
 *   +E  SCC  /SOPR start counter / set output port
 *   +F  STC  /ROPR stop  counter / reset output port
 *
 * Status register bits (channel A):
 *   bit 0 = RxRDY     receiver ready (byte available)
 *   bit 1 = FFULL     receive FIFO full
 *   bit 2 = TxRDY     transmitter ready (TBA empty)
 *   bit 3 = TxEMT     transmitter empty (and FIFO empty)
 *   bit 4 = OE        overrun error
 *   bit 5 = PE        parity error
 *   bit 6 = FE        framing error
 *   bit 7 = RB        received break
 *==========================================================================*/
#include "xcpm.h"

#ifndef DUART_BASE
#define DUART_BASE      0x020000UL
#endif

/* Bus wiring: register N lives at DUART_BASE + N*DUART_STRIDE + DUART_LANE.
 * PVS-2 wires the chip byte-wide and contiguous (stride 1, lane 0). The
 * NCD15 wires it on a 32-bit big-endian bus with a 4-byte register stride
 * and the chip on data lines D15..D8, so each register's byte appears at
 * word+2 (stride 4, lane 2). Boards override both via CFLAGS_EXTRA. */
#ifndef DUART_STRIDE
#define DUART_STRIDE    1
#endif
#ifndef DUART_LANE
#define DUART_LANE      0
#endif

#define DUART_REG(n)    (*(volatile u8 *)(DUART_BASE + (n)*DUART_STRIDE + DUART_LANE))

/* Common (chip-wide) registers */
#define DUART_IPCR_ACR  DUART_REG(0x4)
#define DUART_ISR_IMR   DUART_REG(0x5)
#define DUART_IVR       DUART_REG(0xC)
#define DUART_OPCR      DUART_REG(0xD)
#define DUART_SOPR      DUART_REG(0xE)
#define DUART_ROPR      DUART_REG(0xF)

/* Channel A registers (aux / printer) */
#define DUART_MR1A_2A   DUART_REG(0x0)
#define DUART_SRA_CSRA  DUART_REG(0x1)
#define DUART_CRA       DUART_REG(0x2)
#define DUART_RBA_TBA   DUART_REG(0x3)

/* Channel B registers (CONSOLE) */
#define DUART_MR1B_2B   DUART_REG(0x8)
#define DUART_SRB_CSRB  DUART_REG(0x9)
#define DUART_CRB       DUART_REG(0xA)
#define DUART_RBB_TBB   DUART_REG(0xB)

/* Status bits (same layout for either channel) */
#define SR_RXRDY    0x01
#define SR_TXRDY    0x04

/* Command register opcodes */
#define CR_RST_MR_PTR   0x10        /* reset MR pointer (point to MR1) */
#define CR_RST_RX       0x20        /* reset receiver */
#define CR_RST_TX       0x30        /* reset transmitter */
#define CR_RST_ERR      0x40        /* reset error status */
#define CR_RST_BRK      0x50        /* reset break-change irq */
#define CR_TX_ENABLE    0x04
#define CR_RX_ENABLE    0x01

/*--------------------------------------------------------------------------
 * Init sequence for 8N1, 9600 baud, no parity, no flow control.
 * Assumes the standard 3.6864 MHz crystal -> ACR set 2 -> CSRB 0xBB = 9600.
 *
 * Channel B is the console.  Channel A is left in idle / aux state.
 *
 * Sequence (from MC68681 datasheet, channel B console init):
 *   Common:
 *     ACR  = 0x80   (BRG set 2, IRQ disabled on input change)
 *     IMR  = 0x00   (mask all interrupts; we run polled)
 *     OPCR = 0x00   (output port: general-purpose; not used)
 *     IVR  = 0x0F   (parked vector — never fires while IMR=0)
 *
 *   Channel A (placeholder, parked but defined):
 *     CRA  = 0x1A   (reset receiver + MR pointer)
 *     CRA  = 0x30   (reset transmitter)
 *     CRA  = 0x40   (clear error)
 *     CSRA = 0xBB   (still 9600 in case BIOS aux uses it later)
 *     MR1A = 0x13;  MR2A = 0x07
 *     CRA  = 0x00   (Tx and Rx disabled until something selects A)
 *
 *   Channel B (THE CONSOLE):
 *     CRB  = 0x1A   (reset receiver, reset MR pointer)
 *     CRB  = 0x30   (reset transmitter)
 *     CRB  = 0x40   (clear errors)
 *     CRB  = 0x50   (clear break)
 *     CRB  = 0x10   (MR pointer -> MR1B)
 *     CSRB = 0xBB   (9600 in / 9600 out)
 *     MR1B = 0x13   (no parity, 8 data bits, no RxRTS, no R/B int)
 *     MR2B = 0x07   (1 stop bit, no TxRTS, normal channel mode)
 *     CRB  = 0x05   (enable Tx + Rx)
 *--------------------------------------------------------------------------*/
void uart_init(void)
{
    /*-- Common --*/
    DUART_IPCR_ACR  = 0x80;
    DUART_ISR_IMR   = 0x00;
    DUART_OPCR      = 0x00;
    DUART_IVR       = 0x0F;

    /*-- Channel A: parked, configured but not enabled --*/
    DUART_CRA       = CR_RST_RX | CR_RST_MR_PTR;
    DUART_CRA       = CR_RST_TX;
    DUART_CRA       = CR_RST_ERR;
    DUART_SRA_CSRA  = 0xBB;
    DUART_MR1A_2A   = 0x13;
    DUART_MR1A_2A   = 0x07;
    DUART_CRA       = 0x00;

    /*-- Channel B: the system console --*/
    DUART_CRB       = CR_RST_RX | CR_RST_MR_PTR;    /* 0x1A */
    DUART_CRB       = CR_RST_TX;                    /* 0x30 */
    DUART_CRB       = CR_RST_ERR;                   /* 0x40 */
    DUART_CRB       = CR_RST_BRK;                   /* 0x50 */
    DUART_CRB       = CR_RST_MR_PTR;                /* 0x10 */
    DUART_SRB_CSRB  = 0xBB;                         /* CSRB: 9600/9600 */
    DUART_MR1B_2B   = 0x13;                         /* MR1B: 8N1 */
    DUART_MR1B_2B   = 0x07;                         /* MR2B: 1 stop, normal */
    DUART_CRB       = CR_TX_ENABLE | CR_RX_ENABLE;  /* 0x05 */
}

void uart_putc(int c)
{
    while ((DUART_SRB_CSRB & SR_TXRDY) == 0)
        ;
    DUART_RBB_TBB = (u8)c;
}

int uart_getc(void)
{
    while ((DUART_SRB_CSRB & SR_RXRDY) == 0)
        ;
    return DUART_RBB_TBB;
}

int uart_poll(void)
{
    if (DUART_SRB_CSRB & SR_RXRDY)
        return DUART_RBB_TBB;
    return -1;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_panic(void)
{
    static const char *msg = "\r\n?XCP/M-68K: unhandled exception, halted.\r\n";
    const char *p = msg;
    while (*p) uart_putc(*p++);
}

/* Detailed panic that dumps the first 16 bytes of the supervisor stack so
   we can distinguish a short (TRAP/illegal/privilege/etc., 6-byte) frame
   from a long (bus/address error, 14-byte) frame. */
void uart_panic_at(u32 stack_addr)
{
    volatile u16 *p = (volatile u16 *)stack_addr;
    int i;
    uart_puts("\r\n?XCP/M-68K: unhandled exception\r\n  ssp dump:");
    for (i = 0; i < 8; i++) {
        uart_putc(' ');
        xputhex(p[i], 4);
    }
    uart_puts("\r\n");
}

/*-- Tiny utilities (shared with other UART drivers) ----------------------*/
size_t xstrlen(const char *s)
{
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}

void xputhex(u32 v, int digits)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[9]; int i;
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    buf[digits] = 0;
    for (i = digits - 1; i >= 0; i--) { buf[i] = hex[v & 0xF]; v >>= 4; }
    uart_puts(buf);
}

void xputdec(u32 v)
{
    char buf[12]; int i = sizeof(buf) - 1;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    else { while (v && i >= 0) { buf[i--] = '0' + (char)(v % 10); v /= 10; } }
    uart_puts(&buf[i + 1]);
}
