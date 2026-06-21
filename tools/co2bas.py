#!/usr/bin/env python3
"""
Convert a binary file to an N82 BASIC program that contains DATA statements
and a short loader which reads the bytes into memory and EXECs the start address.

Line endings: CR only (\r), final 0x1A appended.

Usage:
  python3 bin2bas.py input.bin [-o out.bas] [--start 40960] [--bytes-per-line 16]
"""

import sys
import argparse
from pathlib import Path

def chunked(iterable, n):
    it = iter(iterable)
    while True:
        chunk = []
        for _ in range(n):
            try:
                chunk.append(next(it))
            except StopIteration:
                break
        if not chunk:
            break
        yield chunk

def fmt_data_line(line_no, bytes_list):
    return f"{line_no} data " + ",".join(str(b) for b in bytes_list)

def generate_program(data_bytes, co_origin, co_length, co_entry, bytes_per_line=16, start_line=10, line_step=10):
    parts = []
    line_no = start_line

    # DATA lines
    for chunk in chunked(data_bytes, bytes_per_line):
        parts.append(fmt_data_line(line_no, chunk))
        line_no += line_step

    byte_count = len(data_bytes)
    end_addr = co_origin + byte_count - 1

    # We use byte_count instead of co_length here

    # CLEAR: first arg is number of bytes of data
    parts.append(f"{line_no} clear 0, {co_origin}"); line_no += line_step
    parts.append(f"{line_no} for i={co_origin}to{end_addr}:read c:poke i,c:next"); line_no += line_step
    #parts.append(f"{line_no} exec {co_origin}"); line_no += line_step
    parts.append(f"{line_no} bsave \"out.co\",{co_origin},{byte_count},{co_entry}"); line_no += line_step
    #parts.append(f"{line_no} run\"com:9n81xn\"")

    # Join with CR only
    program = ("\r".join(parts) + '\x1a').encode("ansi")
    return program

def main():
    parser = argparse.ArgumentParser(description="Convert binary file to N82 BASIC (CR line endings).")
    parser.add_argument("infile", help="Input binary file")
    parser.add_argument("-o", "--output", help="Output file (default stdout)", default=None)
    parser.add_argument("--bytes-per-line", type=int, default=16, help="Bytes per DATA line")
    parser.add_argument("--start-line", type=int, default=10, help="Starting BASIC line number")
    parser.add_argument("--line-step", type=int, default=10, help="Line number increment")

    args = parser.parse_args()

    infile = Path(args.infile)
    if not infile.exists():
        print("Input file not found.", file=sys.stderr)
        sys.exit(2)

    data = infile.read_bytes()
    data_bytes = list(data[6:])
    co_origin = int.from_bytes(data[0:2], 'little')
    co_length = int.from_bytes(data[2:4], 'little')
    co_entry  = int.from_bytes(data[4:6], 'little')

    prog_bytes = generate_program(
        data_bytes,
        co_origin,
        co_length,
        co_entry,
        bytes_per_line=args.bytes_per_line,
        start_line=args.start_line,
        line_step=args.line_step
    )

    if args.output:
        Path(args.output).write_bytes(prog_bytes)
    else:
        sys.stdout.buffer.write(prog_bytes)

if __name__ == "__main__":
    main()
