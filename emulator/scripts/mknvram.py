#!/usr/bin/env python3
"""
mknvram.py — generate a 128-byte NCD15 93C46 NVRAM image.

The file format matches what `ncd15-emu --nvram <file>` loads and saves:
128 bytes, 64 little-endian 16-bit words, no header.

Known NVRAM layout (empirically determined from DA output with probe
images on the emulator):

    offset 0..1     unknown (often 0xFFFF on blank chip)
    offset 2..7     Ethernet MAC (6 bytes, in-order)
    offset 8+       other fields not yet mapped — IP, mask, gateway,
                    boot-server info, hostname all land somewhere here
                    but need SE (the NVRAM setup tool) or direct
                    RE to map to specific offsets

Usage:

    mknvram.py --out nvram.bin --mac random
    mknvram.py --out nvram.bin --mac 02:00:5e:de:ad:01
    mknvram.py --out nvram.bin --mac random --ip 192.168.1.65 \\
                                --mask 255.255.255.0

The --ip / --mask args are accepted but not written anywhere yet —
until the offsets are mapped, use the monitor's `SE` command from
inside the running emulator to set them interactively, then exit so
the updated NVRAM file is saved back.
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--mac", default="random")
    ap.add_argument("--ip", default=None)
    ap.add_argument("--mask", default=None)
    ap.add_argument("--gateway", default=None)
    args = ap.parse_args()

    mac = parse_mac(args.mac)
    # Start with erased-state image (all 0xFF — what a blank 93C46 reads).
    data = bytearray([0xff] * NVRAM_BYTES)

    # Offset 2..7: MAC.
    data[2:8] = mac

    # IP / mask / gateway not yet mapped to specific NVRAM offsets — write
    # them alongside the MAC for now (so the raw bytes are present) and
    # document that they're not actually picked up by the monitor yet.
    if args.ip:
        data[8:12] = parse_ip(args.ip)
    if args.mask:
        data[12:16] = parse_ip(args.mask)
    if args.gateway:
        data[16:20] = parse_ip(args.gateway)

    with open(args.out, "wb") as f:
        f.write(bytes(data))

    print(f"wrote {args.out} ({len(data)} bytes)")
    print(f"  MAC:     {':'.join(f'{b:02x}' for b in mac)}")
    if args.ip:      print(f"  IP:      {args.ip}   (not yet mapped — see SE)")
    if args.mask:    print(f"  mask:    {args.mask} (not yet mapped — see SE)")
    if args.gateway: print(f"  gateway: {args.gateway} (not yet mapped — see SE)")

if __name__ == "__main__":
    main()
