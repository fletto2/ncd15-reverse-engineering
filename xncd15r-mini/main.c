/* Minimal Xncd15r replacement — prints a banner to DUART channel B.
 * DUART is SCN2681/MC68681 at 0xBE880000, 4-byte register stride.
 * Channel B: SRB = 0x26 (status, bit 2 = TxRDY), THRB = 0x2C (TX holding). */

#define DUART_SRB  (*(volatile unsigned char *)0xBE880026)
#define DUART_THRB (*(volatile unsigned char *)0xBE88002C)
#define DUART_SRA  (*(volatile unsigned char *)0xBE880006)
#define DUART_THRA (*(volatile unsigned char *)0xBE88000C)
#define TXRDY      0x04

static void putc_b(char c) {
    while (!(DUART_SRB & TXRDY)) { }
    DUART_THRB = (unsigned char)c;
}

static void putc_a(char c) {
    while (!(DUART_SRA & TXRDY)) { }
    DUART_THRA = (unsigned char)c;
}

static void puts_both(const char *s) {
    while (*s) {
        putc_a(*s);
        putc_b(*s);
        s++;
    }
}

void main(void) {
    puts_both("\r\n");
    puts_both("=== custom Xncd15r running at 0x0ED00000 ===\r\n");
    puts_both("hello from MIPS-I bare metal\r\n");
    for (;;) { }
}
