#!/usr/bin/env python
import argparse
from pathlib import Path
import sys

def main():
    parser = argparse.ArgumentParser(description="Convert binary file to comma-separated C integer literals.")
    parser.add_argument("infile", help="Input binary file")
    parser.add_argument("-o", "--output", help="Output file (default stdout)", default=None)
    parser.add_argument("-w", "--width", type=int, default=80, help="Maximum column width (default: 80)")
    args = parser.parse_args()

    infile = Path(args.infile)
    if not infile.exists():
        print("Input file not found.", file=sys.stderr)
        sys.exit(2)

    data = infile.read_bytes()
    if not data:
        return

    # Convert bytes to C hex literals (e.g., 0x00)
    literals = [f'{x:#04x}' for x in data]

    # Format literals into lines matching the max column width
    lines = []
    current_line = ""

    for lit in literals:
        if not current_line:
            current_line = lit
        # Check if adding the next literal exceeds the max width
        elif len(current_line) + len(", ") + len(lit) <= args.width:
            current_line += ", " + lit
        else:
            lines.append(current_line + ",")
            current_line = lit

    if current_line:
        lines.append(current_line)

    # Combine lines with newlines
    formatted_output = "\n".join(lines) + "\n"

    if args.output:
        Path(args.output).write_text(formatted_output)
    else:
        sys.stdout.write(formatted_output)

if __name__ == "__main__":
    main()