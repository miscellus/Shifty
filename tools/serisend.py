#!/usr/bin/env python3
import argparse
import sys
import time
import serial


def main():
  parser = argparse.ArgumentParser(
    description="Send raw binary data over serial with inter-byte delay."
  )
  parser.add_argument("file", help="Path to the binary file to send")
  parser.add_argument(
    "-p",
    "--port",
    default="/dev/ttyUSB0",
    help="Serial port device (default: /dev/ttyUSB0)",
  )
  parser.add_argument(
    "-b",
    "--baud",
    type=int,
    default=9600,
    help="Baud rate (default: 9600)",
  )
  parser.add_argument(
    "-d",
    "--delay",
    type=float,
    default=5.0,
    help="Delay per byte in milliseconds (default: 5.0 ms)",
  )

  args = parser.parse_args()
  delay_sec = args.delay / 1000.0  # Convert ms to seconds

  try:
    ser = serial.Serial(
      port=args.port,
      baudrate=args.baud,
      bytesize=serial.EIGHTBITS,
      parity=serial.PARITY_NONE,
      stopbits=serial.STOPBITS_ONE,
      xonxoff=False,
      rtscts=False,
      timeout=1,
    )
  except serial.SerialException as e:
    print(f"Error opening port {args.port}: {e}", file=sys.stderr)
    sys.exit(1)

  try:
    with open(args.file, "rb") as f:
      data = f.read()

    total_bytes = len(data)
    print(
      f"Sending '{args.file}' ({total_bytes} bytes) to {args.port} at {args.baud} baud..."
    )
    print(f"Inter-byte delay: {args.delay} ms")

    start_time = time.time()
    for i, byte_val in enumerate(data, start=1):
      ser.write(bytes([byte_val]))
      time.sleep(delay_sec)

      # Print progress every 50 bytes or on final byte
      if i % 50 == 0 or i == total_bytes:
        pct = (i / total_bytes) * 100
        print(
          f"\rProgress: {i}/{total_bytes} bytes ({pct:.1f}%)",
          end="",
          flush=True,
        )

    elapsed = time.time() - start_time
    print(f"\nTransfer complete in {elapsed:.2f} seconds.")

  except FileNotFoundError:
    print(f"Error: File '{args.file}' not found.", file=sys.stderr)
    sys.exit(1)
  finally:
    ser.close()


if __name__ == "__main__":
  main()