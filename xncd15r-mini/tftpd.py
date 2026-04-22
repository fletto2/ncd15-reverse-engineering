#!/usr/bin/env python3
"""
TFTP server (RFC 1350 + 2347/2348 options) + FS/1 custom file-service
server for the 3Com CS/2500 net stack.

Two UDP listeners run in one process:

  Port 69   — standard TFTP (octet/netascii, RRQ + WRQ, blksize/tsize/timeout).
              WRQ is used by the board's TFTP_WRITE_FILE ROM API ($045E).

  Port 1069 — FS/1 single-packet request/reply.  Speaks four opcodes:
              1=LIST, 2=DELETE, 3=RENAME, 4=MKDIR.  Replies 10=OK or
              11=ERROR (code, ascii message).  Wire format is documented
              in buildable_rom_source/net/fs.h; keep both in sync.

Cross-platform: stock Python 3.5+ on Linux/macOS/Windows.  Port 69 needs
admin/root; --port overrides (e.g. 6969).  --fs-port overrides 1069.

Usage:
    python3 tftpd.py [DIR] [--bind IP] [--port N] [--fs-port N] [--readonly]

`--readonly` disables WRQ and all FS mutation ops (DELETE/RENAME/MKDIR);
LIST still works.
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

# --- TFTP opcodes -----------------------------------------------------------
OP_RRQ = 1
OP_WRQ = 2
OP_DATA = 3
OP_ACK = 4
OP_ERR = 5
OP_OACK = 6

ERR_NOT_FOUND = 1
ERR_ACCESS = 2
ERR_DISK = 3
ERR_ILLEGAL = 4
ERR_UNKNOWN_TID = 5
ERR_EXISTS = 6

DEFAULT_BLKSIZE = 512
MAX_BLKSIZE = 65464  # RFC 2348 max practical

# --- FS/1 opcodes (keep in sync with net/fs.h) ------------------------------
FS_OP_LIST = 1
FS_OP_DELETE = 2
FS_OP_RENAME = 3
FS_OP_MKDIR = 4
FS_OP_OK = 10
FS_OP_ERROR = 11

FS_ERR_NOT_FOUND = 1
FS_ERR_ACCESS = 2
FS_ERR_EXISTS = 3
FS_ERR_ILLEGAL = 4
FS_ERR_IO = 5

FS_MAX_PKT = 1400      # Single packet cap for FS/1 requests and replies.


def log(msg):
    print("[{0}] {1}".format(time.strftime("%H:%M:%S"), msg), flush=True)


# --- TFTP helpers -----------------------------------------------------------

def build_err(code, msg):
    return struct.pack("!HH", OP_ERR, code) + msg.encode("ascii", "replace") + b"\0"


def parse_cstrings(payload):
    parts = payload.split(b"\0")
    if parts and parts[-1] == b"":
        parts = parts[:-1]
    return [p.decode("ascii", "replace") for p in parts]


def safe_path(root, name):
    """Prevent path traversal.  Returns an absolute path guaranteed to
    be at or under `root`, or None if `name` escapes it.  Accepts either
    slash form for Windows compatibility."""
    name = name.replace("\\", "/").lstrip("/")
    full = os.path.normpath(os.path.join(root, name))
    root_abs = os.path.abspath(root)
    full_abs = os.path.abspath(full)
    if not (full_abs == root_abs or full_abs.startswith(root_abs + os.sep)):
        return None
    return full_abs


def send_err(client, code, msg):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(build_err(code, msg), client)
    s.close()


# --- TFTP RRQ ---------------------------------------------------------------

def serve_rrq(root, client, filename, mode, opts):
    peer = "{0}:{1}".format(*client)
    log("RRQ from {0}: {1!r} mode={2} opts={3}".format(peer, filename, mode, opts))
    if mode.lower() not in ("octet", "netascii"):
        return send_err(client, ERR_ILLEGAL, "unsupported mode")

    path = safe_path(root, filename)
    if path is None or not os.path.isfile(path):
        log("  -> NOT FOUND ({0})".format(path))
        return send_err(client, ERR_NOT_FOUND, "file not found")

    try:
        f = open(path, "rb")
    except OSError as e:
        return send_err(client, ERR_ACCESS, str(e))
    size = os.path.getsize(path)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    sock.settimeout(2.0)

    blksize = DEFAULT_BLKSIZE
    resp_opts = {}
    if "blksize" in opts:
        try:
            req = int(opts["blksize"])
            blksize = max(8, min(req, MAX_BLKSIZE))
            resp_opts["blksize"] = str(blksize)
        except ValueError:
            pass
    if "tsize" in opts:
        resp_opts["tsize"] = str(size)
    if "timeout" in opts:
        try:
            t = int(opts["timeout"])
            if 1 <= t <= 255:
                resp_opts["timeout"] = str(t)
        except ValueError:
            pass

    try:
        if resp_opts:
            pkt = struct.pack("!H", OP_OACK)
            for k, v in resp_opts.items():
                pkt += k.encode() + b"\0" + v.encode() + b"\0"
            if not _send_await_ack(sock, client, pkt, expected_block=0):
                return
        blocknum = 1
        while True:
            data = f.read(blksize)
            pkt = struct.pack("!HH", OP_DATA, blocknum & 0xFFFF) + data
            if not _send_await_ack(sock, client, pkt, expected_block=blocknum & 0xFFFF):
                return
            if len(data) < blksize:
                log("  -> DONE {0} bytes to {1}".format(size, peer))
                return
            blocknum += 1
    finally:
        f.close()
        sock.close()


def _send_await_ack(sock, client, pkt, expected_block, retries=5):
    for attempt in range(retries):
        try:
            sock.sendto(pkt, client)
            while True:
                resp, addr = sock.recvfrom(1024)
                if addr[0] != client[0]:
                    continue
                if len(resp) < 4:
                    continue
                op = struct.unpack("!H", resp[:2])[0]
                if op == OP_ACK:
                    blk = struct.unpack("!H", resp[2:4])[0]
                    if blk == expected_block:
                        return True
                elif op == OP_ERR:
                    code = struct.unpack("!H", resp[2:4])[0]
                    msg = resp[4:].split(b"\0", 1)[0].decode("ascii", "replace")
                    log("  -> client ERROR {0}: {1}".format(code, msg))
                    return False
        except socket.timeout:
            log("  -> timeout waiting for ACK {0}, retry {1}/{2}".format(expected_block, attempt + 1, retries))
            continue
    log("  -> gave up waiting for ACK {0}".format(expected_block))
    return False


# --- TFTP WRQ (new, for TFTP_WRITE_FILE ROM API) ---------------------------

def serve_wrq(root, client, filename, mode, opts, readonly):
    peer = "{0}:{1}".format(*client)
    log("WRQ from {0}: {1!r} mode={2} opts={3}".format(peer, filename, mode, opts))
    if readonly:
        return send_err(client, ERR_ACCESS, "server is read-only")
    if mode.lower() not in ("octet", "netascii"):
        return send_err(client, ERR_ILLEGAL, "unsupported mode")

    path = safe_path(root, filename)
    if path is None:
        return send_err(client, ERR_ACCESS, "path escapes root")

    # Create/truncate the target file.
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        f = open(path, "wb")
    except OSError as e:
        return send_err(client, ERR_ACCESS, str(e))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    sock.settimeout(2.0)

    blksize = DEFAULT_BLKSIZE
    resp_opts = {}
    if "blksize" in opts:
        try:
            req = int(opts["blksize"])
            blksize = max(8, min(req, MAX_BLKSIZE))
            resp_opts["blksize"] = str(blksize)
        except ValueError:
            pass
    if "timeout" in opts:
        try:
            t = int(opts["timeout"])
            if 1 <= t <= 255:
                resp_opts["timeout"] = str(t)
        except ValueError:
            pass

    try:
        # Send OACK (if any options negotiated) or ACK 0.
        if resp_opts:
            pkt = struct.pack("!H", OP_OACK)
            for k, v in resp_opts.items():
                pkt += k.encode() + b"\0" + v.encode() + b"\0"
        else:
            pkt = struct.pack("!HH", OP_ACK, 0)
        sock.sendto(pkt, client)

        expected = 1
        total = 0
        retries = 5
        while True:
            for attempt in range(retries):
                try:
                    data, addr = sock.recvfrom(4 + blksize)
                    if addr[0] != client[0]:
                        continue
                    break
                except socket.timeout:
                    sock.sendto(pkt, client)   # resend last ACK
            else:
                log("  -> WRQ timeout")
                return
            if len(data) < 4:
                continue
            op = struct.unpack("!H", data[:2])[0]
            if op == OP_ERR:
                code = struct.unpack("!H", data[2:4])[0]
                msg = data[4:].split(b"\0", 1)[0].decode("ascii", "replace")
                log("  -> client ERROR {0}: {1}".format(code, msg))
                return
            if op != OP_DATA:
                continue
            blk = struct.unpack("!H", data[2:4])[0]
            if blk != expected & 0xFFFF:
                # Duplicate; re-ACK last good block.
                prev = (expected - 1) & 0xFFFF
                sock.sendto(struct.pack("!HH", OP_ACK, prev), client)
                continue
            payload = data[4:]
            f.write(payload)
            total += len(payload)
            pkt = struct.pack("!HH", OP_ACK, blk)
            sock.sendto(pkt, client)
            if len(payload) < blksize:
                log("  -> WROTE {0} bytes to {1}".format(total, path))
                return
            expected += 1
    finally:
        f.close()
        sock.close()


# --- TFTP main dispatch -----------------------------------------------------

def tftp_server_thread(sock, root, readonly):
    while True:
        try:
            data, client = sock.recvfrom(2048)
        except OSError:
            return
        if len(data) < 4:
            continue
        op = struct.unpack("!H", data[:2])[0]
        if op == OP_RRQ:
            parts = parse_cstrings(data[2:])
            if len(parts) < 2:
                send_err(client, ERR_ILLEGAL, "malformed RRQ")
                continue
            filename, mode = parts[0], parts[1]
            opts = {}
            rest = parts[2:]
            for i in range(0, len(rest) - 1, 2):
                opts[rest[i].lower()] = rest[i + 1]
            threading.Thread(
                target=serve_rrq, args=(root, client, filename, mode, opts), daemon=True
            ).start()
        elif op == OP_WRQ:
            parts = parse_cstrings(data[2:])
            if len(parts) < 2:
                send_err(client, ERR_ILLEGAL, "malformed WRQ")
                continue
            filename, mode = parts[0], parts[1]
            opts = {}
            rest = parts[2:]
            for i in range(0, len(rest) - 1, 2):
                opts[rest[i].lower()] = rest[i + 1]
            threading.Thread(
                target=serve_wrq,
                args=(root, client, filename, mode, opts, readonly),
                daemon=True,
            ).start()
        else:
            send_err(client, ERR_ILLEGAL, "expected RRQ or WRQ")


# --- FS/1 server ------------------------------------------------------------

def fs_err(code, msg):
    return (
        struct.pack("!HH", FS_OP_ERROR, code)
        + msg.encode("ascii", "replace")[:256]
        + b"\0"
    )


def fs_ok(payload=b""):
    return struct.pack("!H", FS_OP_OK) + payload


def fs_parse_strings(payload):
    """Split payload into 1+ NUL-terminated strings.  Trailing empty is
    dropped.  Returns a list of str."""
    parts = payload.split(b"\0")
    if parts and parts[-1] == b"":
        parts = parts[:-1]
    return [p.decode("ascii", "replace") for p in parts]


def fs_handle_list(root, path):
    p = safe_path(root, path) if path else os.path.abspath(root)
    if p is None or not os.path.isdir(p):
        return fs_err(FS_ERR_NOT_FOUND, "not a directory: " + path)
    try:
        entries = sorted(os.listdir(p))
    except OSError as e:
        return fs_err(FS_ERR_IO, str(e))

    # Mark directories with a trailing '/' so the client can distinguish.
    buf = bytearray()
    truncated = False
    for name in entries:
        entry = name + ("/" if os.path.isdir(os.path.join(p, name)) else "")
        b = entry.encode("ascii", "replace") + b"\0"
        # Leave 5 bytes for "\0*\0" truncation marker + header.
        if 2 + len(buf) + len(b) > FS_MAX_PKT - 3:
            truncated = True
            break
        buf += b
    if truncated:
        buf += b"*\0"    # sentinel
    return fs_ok(bytes(buf))


def fs_handle_delete(root, path, readonly):
    if readonly:
        return fs_err(FS_ERR_ACCESS, "server is read-only")
    p = safe_path(root, path)
    if p is None or not os.path.exists(p):
        return fs_err(FS_ERR_NOT_FOUND, "no such file: " + path)
    try:
        if os.path.isdir(p):
            os.rmdir(p)
        else:
            os.remove(p)
    except OSError as e:
        return fs_err(FS_ERR_IO, str(e))
    return fs_ok()


def fs_handle_rename(root, old, new, readonly):
    if readonly:
        return fs_err(FS_ERR_ACCESS, "server is read-only")
    po = safe_path(root, old)
    pn = safe_path(root, new)
    if po is None or pn is None:
        return fs_err(FS_ERR_ACCESS, "path escapes root")
    if not os.path.exists(po):
        return fs_err(FS_ERR_NOT_FOUND, "no such path: " + old)
    if os.path.exists(pn):
        return fs_err(FS_ERR_EXISTS, "target exists: " + new)
    try:
        os.rename(po, pn)
    except OSError as e:
        return fs_err(FS_ERR_IO, str(e))
    return fs_ok()


def fs_handle_mkdir(root, path, readonly):
    if readonly:
        return fs_err(FS_ERR_ACCESS, "server is read-only")
    p = safe_path(root, path)
    if p is None:
        return fs_err(FS_ERR_ACCESS, "path escapes root")
    if os.path.exists(p):
        return fs_err(FS_ERR_EXISTS, "exists: " + path)
    try:
        os.mkdir(p)
    except OSError as e:
        return fs_err(FS_ERR_IO, str(e))
    return fs_ok()


def fs_server_thread(sock, root, readonly):
    while True:
        try:
            data, client = sock.recvfrom(FS_MAX_PKT)
        except OSError:
            return
        if len(data) < 2:
            continue
        op = struct.unpack("!H", data[:2])[0]
        parts = fs_parse_strings(data[2:])
        arg1 = parts[0] if parts else ""
        arg2 = parts[1] if len(parts) > 1 else ""

        peer = "{0}:{1}".format(*client)
        try:
            if op == FS_OP_LIST:
                log("FS LIST {0!r} from {1}".format(arg1, peer))
                reply = fs_handle_list(root, arg1)
            elif op == FS_OP_DELETE:
                log("FS DELETE {0!r} from {1}".format(arg1, peer))
                reply = fs_handle_delete(root, arg1, readonly)
            elif op == FS_OP_RENAME:
                log("FS RENAME {0!r} -> {1!r} from {2}".format(arg1, arg2, peer))
                reply = fs_handle_rename(root, arg1, arg2, readonly)
            elif op == FS_OP_MKDIR:
                log("FS MKDIR {0!r} from {1}".format(arg1, peer))
                reply = fs_handle_mkdir(root, arg1, readonly)
            else:
                reply = fs_err(FS_ERR_ILLEGAL, "unknown op {0}".format(op))
        except Exception as e:
            reply = fs_err(FS_ERR_IO, "server exception: {0}".format(e))

        try:
            sock.sendto(reply, client)
        except OSError as e:
            log("  -> reply send failed: {0}".format(e))


# --- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory", nargs="?", default=".", help="root dir to serve")
    ap.add_argument("--bind", default="0.0.0.0", help="bind IP")
    ap.add_argument("--port", type=int, default=69, help="TFTP UDP port (default 69)")
    ap.add_argument("--fs-port", type=int, default=1069, help="FS/1 UDP port (default 1069)")
    ap.add_argument("--readonly", action="store_true",
                    help="disable WRQ, FS DELETE/RENAME/MKDIR (LIST still works)")
    args = ap.parse_args()

    root = os.path.abspath(args.directory)
    if not os.path.isdir(root):
        print("Not a directory: {0}".format(root), file=sys.stderr)
        sys.exit(1)

    tftp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tftp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    fs_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    fs_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        tftp_sock.bind((args.bind, args.port))
    except PermissionError:
        print("Permission denied binding port {0} (try --port 6969).".format(args.port), file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print("bind({0}:{1}) failed: {2}".format(args.bind, args.port, e), file=sys.stderr)
        sys.exit(1)

    try:
        fs_sock.bind((args.bind, args.fs_port))
    except OSError as e:
        print("bind({0}:{1}) failed: {2}".format(args.bind, args.fs_port, e), file=sys.stderr)
        sys.exit(1)

    log("TFTP  on {0}:{1}  root={2}{3}".format(
        args.bind, args.port, root, "  (read-only)" if args.readonly else ""))
    log("FS/1  on {0}:{1}".format(args.bind, args.fs_port))

    try:
        files = sorted(os.listdir(root))
        log("Serving {0} entries: {1}".format(
            len(files), ", ".join(files[:12]) + (" ..." if len(files) > 12 else "")))
    except OSError:
        pass

    t1 = threading.Thread(target=tftp_server_thread,
                          args=(tftp_sock, root, args.readonly), daemon=True)
    t2 = threading.Thread(target=fs_server_thread,
                          args=(fs_sock, root, args.readonly), daemon=True)
    t1.start()
    t2.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        log("shutdown")


if __name__ == "__main__":
    main()
