import sys
from PIL import Image
from pathlib import Path
import argparse


def image_to_segmented_assembly(image_path):
    # Target LCD dimensions
    TARGET_WIDTH = 240
    TARGET_HEIGHT = 64

    try:
        img = Image.open(image_path).convert('1')
    except Exception as e:
        return f"Error loading image: {e}"

    # Force image to exactly 240x64 by creating a white canvas and pasting
    # This prevents out-of-bounds errors if the input image is the wrong size.
    canvas = Image.new('1', (TARGET_WIDTH, TARGET_HEIGHT), color=255)
    canvas.paste(img, (0, 0))
    img = canvas

    # Define the 10 controllers: (start_x, width, start_y)
    controllers = [
        (0,   50, 0),  # Controller 0
        (50,  50, 0),  # Controller 1
        (100, 50, 0),  # Controller 2
        (150, 50, 0),  # Controller 3
        (200, 40, 0),  # Controller 4 (Cropped)
        (0,   50, 32), # Controller 5
        (50,  50, 32), # Controller 6
        (100, 50, 32), # Controller 7
        (150, 50, 32), # Controller 8
        (200, 40, 32)  # Controller 9 (Cropped)
    ]

    asm_lines = []

    for ctrl_idx, (start_x, width, start_y) in enumerate(controllers):
        asm_lines.append(f"\n    ; --- Controller {ctrl_idx} ({width}x32) ---")

        # Each controller has 32 vertical pixels, which is 4 pages of 8 pixels
        for page in range(4):
            asm_lines.append(f"    ; Page {page}")
            y_offset = start_y + (page * 8)
            page_bytes = []

            # Traverse left-to-right within this specific page
            for x in range(start_x, start_x + width):
                byte_val = 0
                for bit in range(8):
                    pixel = img.getpixel((x, y_offset + bit))

                    # 0 is black (ON) in Pillow's '1' mode
                    if pixel == 0:
                        # Top pixel is LSB. Swap to (1 << (7 - bit)) if your screen draws upside down.
                        byte_val |= (1 << bit)

                page_bytes.append(byte_val)

            # Format the page into assembly lines (10 bytes per line for clean alignment)
            bytes_per_line = 10
            for i in range(0, len(page_bytes), bytes_per_line):
                chunk = page_bytes[i:i + bytes_per_line]
                hex_strings = [f"0x{b:02x}" for b in chunk]
                asm_lines.append("    db " + ", ".join(hex_strings))

    return asm_lines


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("input", type=Path, help="240x64 black and white image input")
    p.add_argument("output", type=Path, help="output assembly file to write")
    args = p.parse_args()

    out = image_to_segmented_assembly(args.input)
    args.output.write_text("\n".join(out), encoding="utf-8")

if __name__ == "__main__":
    main()