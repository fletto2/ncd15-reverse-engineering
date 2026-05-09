#!/usr/bin/env python3
"""Minimal TFTP server over AF_PACKET — for the emulator's --raweth path.

WHY THIS EXISTS: when the emulator runs with `--raweth eth0`, it puts
LANCE frames on the wire with a spoofed source MAC (the NVRAM-configured
00:00:a7:01:02:03 by default). Many switches/APs do port split-horizon
or MAC-table filtering that prevents those frames from reflecting back
to the same host's kernel UDP stack — so a normal userspace tftpd
running on the host *won't see* the emulator's RRQs even though they
hit the wire. This server bypasses the kernel's UDP stack entirely:
it listens via AF_PACKET on the same interface, parses UDP/TFTP from
the raw frames, and crafts replies via raw frames too.

USAGE:
    sudo python3 raw_tftpd.py [--iface eth0] [--ip 192.168.1.78]
                              [--root /tmp/tftproot]

If --ip is not given, it's auto-detected from the interface. If --root
is not given, it defaults to /tmp/tftproot.

Limits: octet mode only, no options (blksize/tsize/timeout), 512-byte
blocks. Enough for the NCD15 monitor's BT (boot-via-tftp) command.
"""
import argparse, os, socket, struct, sys

ETH_P_IP = 0x0800
ETH_P_ALL = 0x0003


def get_iface_mac(iface):
    with open("/sys/class/net/" + iface + "/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


def get_iface_ip(iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 1))
        ip = s.getsockname()[0]
    finally:
        s.close()
    return bytes(int(o) for o in ip.split("."))


def ip_csum(b):
    s = 0
    for i in range(0, len(b), 2):
        s += (b[i] << 8) | b[i+1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def make_udp_frame(my_mac, my_ip, dst_mac, dst_ip, src_port, dst_port, payload):
    eth = dst_mac + my_mac + struct.pack("!H", ETH_P_IP)
    udp_len = 8 + len(payload)
    ip_len  = 20 + udp_len
    ip = struct.pack("!BBHHHBBH", 0x45, 0, ip_len, 0, 0, 64, 17, 0) + my_ip + dst_ip
    ip = ip[:10] + struct.pack("!H", ip_csum(ip)) + ip[12:]
    udp = struct.pack("!HHHH", src_port, dst_port, udp_len, 0) + payload
    return eth + ip + udp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", default="eth0")
    ap.add_argument("--ip", default=None,
                    help="Server IP (defaults to iface IP)")
    ap.add_argument("--root", default="/tmp/tftproot")
    args = ap.parse_args()

    my_mac = get_iface_mac(args.iface)
    if args.ip:
        my_ip = bytes(int(o) for o in args.ip.split("."))
    else:
        my_ip = get_iface_ip(args.iface)

    print("raw_tftpd: iface=%s mac=%s ip=%s root=%s" % (
        args.iface, ":".join("%02x" % b for b in my_mac),
        ".".join(str(b) for b in my_ip), args.root), flush=True)

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((args.iface, 0))

    xfers = {}
    while True:
        frame, _ = s.recvfrom(65535)
        if len(frame) < 14: continue
        et = (frame[12] << 8) | frame[13]
        if et != ETH_P_IP: continue
        if len(frame) < 14 + 20: continue
        ip_hdr = frame[14:14+20]
        if (ip_hdr[0] >> 4) != 4: continue
        proto = ip_hdr[9]
        src_ip = ip_hdr[12:16]; dst_ip = ip_hdr[16:20]
        if dst_ip != my_ip: continue
        if proto != 17: continue
        src_mac = frame[6:12]
        udp = frame[14+20:14+20+8]
        sp = (udp[0] << 8) | udp[1]
        dp = (udp[2] << 8) | udp[3]
        pl_off = 14 + 20 + 8
        pl_len = ((udp[4] << 8) | udp[5]) - 8
        pl = frame[pl_off:pl_off+pl_len]
        if dp != 69 and sp != 69: continue
        op = (pl[0] << 8) | pl[1] if len(pl) >= 2 else 0
        key = (bytes(src_mac), bytes(src_ip), sp)
        if op == 1:  # RRQ
            parts = pl[2:].split(b"\0")
            fname = parts[0].decode("ascii", "replace")
            mode  = parts[1].decode("ascii", "replace") if len(parts) > 1 else ""
            path = os.path.join(args.root, os.path.basename(fname))
            print("RRQ '%s' mode=%s from %d.%d.%d.%d:%d" % (
                fname, mode, src_ip[0], src_ip[1], src_ip[2], src_ip[3], sp),
                flush=True)
            if not os.path.isfile(path):
                err = struct.pack("!HH", 5, 1) + b"file not found\0"
                s.send(make_udp_frame(my_mac, my_ip, src_mac, src_ip, 69, sp, err))
                print("  -> NOT FOUND " + path, flush=True)
                continue
            f = open(path, "rb")
            chunk = f.read(512)
            xfers[key] = {"f": f, "last_n": 1, "done": (len(chunk) < 512)}
            data = struct.pack("!HH", 3, 1) + chunk
            s.send(make_udp_frame(my_mac, my_ip, src_mac, src_ip, 69, sp, data))
            print("  -> DATA blk=1 (%d bytes)" % len(chunk), flush=True)
        elif op == 4:  # ACK
            ack = (pl[2] << 8) | pl[3]
            x = xfers.get(key)
            if not x: continue
            if x.get("done") and ack == x["last_n"]:
                print("  done %d blocks" % ack, flush=True)
                x["f"].close()
                del xfers[key]
                continue
            chunk = x["f"].read(512)
            n = ack + 1
            x["last_n"] = n
            x["done"] = (len(chunk) < 512)
            data = struct.pack("!HH", 3, n) + chunk
            s.send(make_udp_frame(my_mac, my_ip, src_mac, src_ip, 69, sp, data))
            if n % 200 == 0 or x["done"]:
                print("  -> DATA blk=%d (%d)" % (n, len(chunk)), flush=True)


if __name__ == "__main__":
    main()
