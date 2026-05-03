#!/usr/bin/env python3
"""
mknvram.py — generate a 128-byte NCD15 93C46 NVRAM image.

The file format matches what `ncd15-emu --nvram <file>` loads and saves:
128 bytes, 64 little-endian 16-bit words, no header.

Layout (RE'd from monitor.dis sub_0ec11774_dump):

    offset 0..1     unknown (often 0xFFFF on blank chip)
    offset 2..7     Ethernet MAC (6 bytes; pair-swapped on the wire,
                    written here in plain MAC order — see below)
    offset 8..11    Terminal/local IP (4 bytes, pair-swapped)
    offset 17       bit 1 OR bit 2 set → BOOTP/RARP, else use stored IP
    offset 19..22   Subnet mask (4 bytes, pair-swapped, UNALIGNED)
    offset 23..26   Gateway IP (4 bytes, pair-swapped, UNALIGNED)
    offset 106..109 First boot host IP (4 bytes, pair-swapped, words 53-54)

The monitor stores NVRAM as 16-bit LE words in the file but reads them
BE-order into a mirror, then byte-copies to runtime slots. The net
effect is a per-pair byte-swap: an IP a.b.c.d at file[N..N+3] is laid
down as [b, a, d, c]. parse_ip_pairswapped() handles that here so the
on-CLI form remains 'a.b.c.d'.

Usage:

    mknvram.py --out nvram.bin --mac random
    mknvram.py --out nvram.bin --mac 02:00:5e:de:ad:01 \\
               --ip 192.168.1.65 --mask 255.255.255.0 \\
               --gateway 192.168.1.1 --server 192.168.1.15

If --ip / --mask / --gateway / --server are given, this also clears
file[17] bits 1+2 so the monitor uses the stored values rather than
trying BOOTP.
"""
import argparse
import random
import struct
import sys

NVRAM_BYTES = 128

def parse_mac(s):
    if s == "random":
        # Locally-administered, unicast (bit 1 set, bit 0 clear of byte 0).
        b = [(random.randint(0, 255) & 0xfc) | 0x02]
        b += [random.randint(0, 255) for _ in range(5)]
        return bytes(b)
    parts = s.split(":")
    if len(parts) != 6:
        raise ValueError(f"bad MAC: {s}")
    return bytes(int(p, 16) for p in parts)

def parse_ip(s):
    parts = s.split(".")
    if len(parts) != 4:
        raise ValueError(f"bad IP: {s}")
    return bytes(int(p) for p in parts)

def write_ip_pairswapped(data, off, ip_str):
    """Write IP a.b.c.d at file[off..off+3] in the monitor's expected
    pair-swapped form: [b, a, d, c]. Used for local IP, mask, gateway,
    and boot-server IP fields."""
    ip = parse_ip(ip_str)
    data[off + 0] = ip[1]
    data[off + 1] = ip[0]
    data[off + 2] = ip[3]
    data[off + 3] = ip[2]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--mac", default="random")
    ap.add_argument("--ip", default=None)
    ap.add_argument("--mask", default=None)
    ap.add_argument("--gateway", default=None)
    ap.add_argument("--server", default=None,
                    help="First boot host IP (TFTP server)")
    ap.add_argument("--broadcast", default=None,
                    help="Broadcast IP (defaults to IP|~mask if --ip and --mask given)")
    args = ap.parse_args()

    mac = parse_mac(args.mac)
    # Start with erased-state image (all 0xFF — what a blank 93C46 reads).
    data = bytearray([0xff] * NVRAM_BYTES)

    # MAC at NVRAM file bytes 2..7. Same pair-swap as IP fields: the
    # monitor stores NVRAM as LE 16-bit words and byte-copies to the
    # runtime MAC slot, which the LANCE init block then reads in straight
    # byte order. Writing the MAC pair-swapped in the file makes the
    # LANCE see it correctly on the wire.
    data[2] = mac[1]
    data[3] = mac[0]
    data[4] = mac[3]
    data[5] = mac[2]
    data[6] = mac[5]
    data[7] = mac[4]

    have_static = any([args.ip, args.mask, args.gateway, args.server])

    # The X-server's setup_interfaces (at 0x8EE4DC50..0x8EE4E708) reads
    # mask/gateway/broadcast as 4-byte logical IPs out of an interface
    # struct at shadow+0x2EE1B0+. The struct's mirror starts at struct+8
    # and stores each 93C46 word in HIGH-LOW byte order, with word[w] =
    # (file[2w+1] << 8) | file[2w]. That makes mirror[2j]   = file[17+2j]
    # and mirror[2j+1] = file[16+2j].
    #
    # Each 4-byte logical field at struct[F..F+3] = mirror[F-8..F-5] thus
    # spans three NVRAM words. With base = F+7 (= file address of the
    # logical b0 byte), the field's logical bytes [b0, b1, b2, b3] land at:
    #
    #   file[base+0] = b0   file[base+3] = b1
    #   file[base+2] = b2   file[base+5] = b3
    #
    # i.e. byte 1 is at +3 and byte 3 is at +5; bytes +1 and +4 in the
    # NVRAM are unused for this field. The setup_interfaces bcopy targets:
    #
    #   F=11 (mask):       base=18  -> file[18, 21, 20, 23]
    #   F=15 (gateway):    base=22  -> file[22, 25, 24, 27]
    #   F=19 (broadcast):  base=26  -> file[26, 29, 28, 31]
    #
    # The "free" bytes overlap with the NEXT field's b0/b2, so adjacent
    # fields share NVRAM bytes — that's how three 4-byte IPs fit into
    # 14 NVRAM bytes (18..31).
    def write_xncd_field(data, base, ip_str):
        ip = parse_ip(ip_str)
        data[base + 0] = ip[0]
        data[base + 2] = ip[2]
        data[base + 3] = ip[1]
        data[base + 5] = ip[3]

    if args.ip:
        write_ip_pairswapped(data, 8, args.ip)
    if args.mask:
        write_xncd_field(data, 18, args.mask)
    if args.gateway:
        write_xncd_field(data, 22, args.gateway)
    if args.broadcast:
        write_xncd_field(data, 26, args.broadcast)
    elif args.ip and args.mask:
        # Default broadcast = IP | ~mask (standard CIDR)
        ip = parse_ip(args.ip); mk = parse_ip(args.mask)
        bc = bytes(((a | (~b)) & 0xFF) for a, b in zip(ip, mk))
        bc_str = ".".join(str(b) for b in bc)
        write_xncd_field(data, 26, bc_str)
    if args.server:
        write_ip_pairswapped(data, 106, args.server)

    if have_static:
        # Clear bits 1+2 of file[17] so the monitor uses NVRAM-stored IPs
        # rather than searching via BOOTP/RARP. file[17] is the high byte
        # of NVRAM word 8, which the loader reads via `lw 0xc48($v0)` at
        # 0x0EC117FC and masks against 0x06000000.
        data[17] &= ~0x06

    with open(args.out, "wb") as f:
        f.write(bytes(data))

    print(f"wrote {args.out} ({len(data)} bytes)")
    print(f"  MAC:     {':'.join(f'{b:02x}' for b in mac)}")
    if args.ip:      print(f"  IP:      {args.ip}")
    if args.mask:    print(f"  mask:    {args.mask}")
    if args.gateway: print(f"  gateway: {args.gateway}")
    if args.server:  print(f"  server:  {args.server}")
    if have_static:  print(f"  mode:    static (file[17] bits 1+2 cleared)")

if __name__ == "__main__":
    main()
