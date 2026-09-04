import sys
from PIL import Image
from pathlib import Path
import argparse

def image_to_tiles_assembly(image_path):
    # Target LCD dimensions
    TARGET_WIDTH = 240
    TARGET_HEIGHT = 64

    try:
        img = Image.open(image_path).convert('1')
    except Exception as e:
        sys.exit(f"Error loading image: {e}")

    # Force image to exactly 240x64 by creating a white canvas and pasting
    # This prevents out-of-bounds errors if the input image is the wrong size.
    canvas = Image.new('1', (TARGET_WIDTH, TARGET_HEIGHT), color=255)
    canvas.paste(img, (0, 0))
    img = canvas

    asm_lines = []

    # 240 pixels wide / 10 pixels per tile = 24 tiles horizontally (X)
    # 64 pixels high / 8 pixels per tile = 8 tiles vertically (Y)
    # Total tiles = 24 * 8 = 192 tiles

    # The ASM loops 'l' from 0 to 191.
    # l format: XXXXXYYY (bits 0-2: Y, bits 3-7: X)
    # This means Y increments first, then X. (Column-major ordering)
    for tile_x in range(24):
        for tile_y in range(8):
            # Calculate the literal 'l' value for commenting
            l_val = (tile_x << 3) | tile_y
            asm_lines.append(f"\n    ; --- Tile {l_val} (X:{tile_x}, Y:{tile_y}) ---")

            tile_bytes = []

            # Each tile is 10 pixels wide
            for col in range(10):
                byte_val = 0
                pixel_x = (tile_x * 10) + col

                # Each column is 8 pixels high (1 byte)
                for bit in range(8):
                    pixel_y = (tile_y * 8) + bit
                    pixel = img.getpixel((pixel_x, pixel_y))

                    # 0 is black (ON) in Pillow's '1' mode
                    if pixel == 0:
                        # Top pixel is LSB.
                        byte_val |= (1 << bit)

                tile_bytes.append(byte_val)

            # Format the 10 bytes into a single assembly line
            hex_strings = [f"0x{b:02x}" for b in tile_bytes]
            asm_lines.append("    db " + ", ".join(hex_strings))

    return asm_lines

def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("input", type=Path, help="240x64 black and white image input")
    p.add_argument("output", type=Path, help="output assembly file to write")
    args = p.parse_args()

    out = image_to_tiles_assembly(args.input)
    args.output.write_text("\n".join(out), encoding="utf-8")
    print(f"Successfully wrote 192 tiles to {args.output}")

if __name__ == "__main__":
    main()
