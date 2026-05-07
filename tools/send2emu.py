#!/usr/bin/env python3
"""
Send a binary file with 6-byte header to VirtualT emulator memory via its socket interface.

Usage:
  python3 send2emu.py <file.bin> [--host HOST] [--port PORT] [--chunk CHUNK]

Defaults:
  HOST=localhost
  PORT=9999
  CHUNK=64
"""
import socket
import struct
import argparse
import sys

def read_header(path):
    with open(path, "rb") as f:
        hdr = f.read(6)
        if len(hdr) < 6:
            raise ValueError("File too small for header")
        load_to, size, jump_to = struct.unpack("<HHH", hdr)
        data = f.read(size)
        if len(data) < size:
            raise ValueError(f"File shorter than header size: expected {size}, got {len(data)}")
        return load_to, size, jump_to, data

def recv_until_ok(sock):
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("Socket closed by server")
        buf += chunk
        if b"Ok" in buf:
            return buf

def send_cmd_wait_ok(sock, cmd):
    if isinstance(cmd, str):
        cmd = cmd.encode("ascii")
    sock.sendall(cmd)
    resp = recv_until_ok(sock)
    return resp.decode(errors="replace")

def format_bytes_for_wm(start_addr, data_bytes):
    parts = [f"write_mem {hex(start_addr)}"]
    parts += [hex(b) for b in data_bytes]
    return " ".join(parts)

def main():
    ap = argparse.ArgumentParser(description="Send binary to VirtualT memory via socket")
    ap.add_argument("file", help="Binary file with 6-byte header")
    ap.add_argument("--host", default="localhost", help="VirtualT host (default: localhost)")
    ap.add_argument("--port", type=int, default=9999, help="VirtualT port (default: 9999)")
    ap.add_argument("--chunk", type=int, default=64, help="Bytes per write_mem command (default: 64)")
    args = ap.parse_args()

    try:
        load_to, size, jump_to, data = read_header(args.file)
    except Exception as e:
        print("Error reading file:", e, file=sys.stderr)
        sys.exit(1)

    print(f"{args.file}: load_to=0x{load_to:04x}, size={size}, jump_to=0x{jump_to:04x}")

    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        print("halting CPU...")
        print(send_cmd_wait_ok(sock, "halt"))

        addr = load_to
        offset = 0
        while offset < size:
            chunk = data[offset: offset + args.chunk]
            cmd = format_bytes_for_wm(addr, chunk)
            resp = send_cmd_wait_ok(sock, cmd)
            print(f"Wrote {len(chunk)} bytes to 0x{addr:04x}")
            offset += len(chunk)
            addr += len(chunk)

        # set PC to jump_to and run (jump to entry point)
        print(f"Setting PC to 0x{jump_to:04x} and running")
        print(send_cmd_wait_ok(sock, f"wr pc={hex(jump_to)}"))
        print(send_cmd_wait_ok(sock, "run"))

        print("Done.")

if __name__ == "__main__":
    main()
