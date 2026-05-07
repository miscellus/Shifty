#!/usr/bin/env python3
"""
levels2asm.py

Usage:
  python levels2asm.py levels.txt tiles.s8085 > levels_output.s8085

Notes:
 - Tile byte format (1 byte per tile): SDDTTTTT
     S = 1 if solid, 0 if walkable (bit7)
     DD = direction: 0=west,1=north,2=east,3=south (bits6-5)
     TTTTT = tile image index 0..31 (bits4-0)
 - Legend may include direction tags: #west #north #east #south
 - Non-phony tiles must exist in tiles.s8085 and have index <= 31 (error otherwise).
 - Phony tiles map to their named tile if that tile exists; otherwise TileEmpty if present; otherwise index 0.
"""

import sys
import re
from collections import OrderedDict

if len(sys.argv) != 3:
    print("Usage: python levels2asm.py levels.txt tiles.s8085", file=sys.stderr)
    sys.exit(2)

levels_file = sys.argv[1]
tiles_file = sys.argv[2]

# --- parse tiles.s8085 to get tile label order ---
tile_labels = []
label_re = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):\s*(;.*)?$')
with open(tiles_file, 'r', encoding='utf-8') as f:
    for line in f:
        m = label_re.match(line.strip())
        if m:
            name = m.group(1)
            tile_labels.append(name)
tile_index = {name: i for i, name in enumerate(tile_labels)}

# --- parse levels file ---
with open(levels_file, 'r', encoding='utf-8') as f:
    raw_lines = [ln.rstrip('\n') for ln in f]

legend = {}   # char -> (tile_name, set(tags))
i = 0
legend_re = re.compile(r'^(.?)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$')
while i < len(raw_lines):
    line = raw_lines[i].strip()
    if line == "":
        i += 1
        continue
    m = legend_re.match(line)
    if not m:
        break
    ch = m.group(1)
    tile = m.group(2)
    tags = set(re.findall(r'#([A-Za-z0-9_]+)', m.group(3) or ""))
    if tile not in tile_labels and 'phony' not in tags:
        print(f"WARNING: Unknown tile name, '{tile}'", file=sys.stderr)
    legend[ch] = (tile, tags)
    i += 1

# parse multiple levels
levels = OrderedDict()
while i < len(raw_lines):
    if raw_lines[i].strip() == "":
        i += 1
        continue
    level_name = raw_lines[i].strip()
    i += 1
    rows = []
    while i < len(raw_lines) and len(rows) < 8:
        ln = raw_lines[i]
        i += 1
        if ln.strip() == "":
            continue
        rows.append(ln.rstrip())
    if len(rows) != 8:
        print(f"Error: level '{level_name}' expects 8 non-empty rows, found {len(rows)}", file=sys.stderr)
        sys.exit(2)
    for r in rows:
        if len(r) != 24:
            print(f"Error: level '{level_name}' row length must be 24 chars: '{r}'", file=sys.stderr)
            sys.exit(2)
    levels[level_name] = rows

if not levels:
    print("Error: no levels found in input", file=sys.stderr)
    sys.exit(2)

# --- helper: direction tag -> value ---
dir_map = {
    'west':  0,
    'north': 1,
    'east':  2,
    'south': 3
}

# --- process levels ---
all_referenced_tiles = OrderedDict()
per_level_data = OrderedDict()

for lname, grid in levels.items():
    player_positions = []
    col_tile_bytes = [[0]*8 for _ in range(24)]  # final tile byte per tile
    for y in range(8):
        row = grid[y]
        for x in range(24):
            ch = row[x]
            if ch not in legend:
                print(f"Error: character '{ch}' at ({x},{y}) in level '{lname}' not found in legend", file=sys.stderr)
                sys.exit(2)
            tile_name, tags = legend[ch]

            # solid bit
            is_solid = 1 if 'solid' in tags else 0

            # direction
            dir_val = 0
            for tname, val in dir_map.items():
                if tname in tags:
                    dir_val = val
                    break
            # determine tile index (handle phony)
            if 'phony' in tags:
                # phony: try to map to named tile if exists, else TileEmpty, else 0
                if tile_name in tile_index:
                    idx = tile_index[tile_name]
                elif 'TileEmpty' in tile_index:
                    idx = tile_index['TileEmpty']
                else:
                    idx = 0
            else:
                if tile_name not in tile_index:
                    print(f"Error: tile '{tile_name}' used in level '{lname}' not found in {tiles_file}", file=sys.stderr)
                    sys.exit(2)
                idx = tile_index[tile_name]
                all_referenced_tiles.setdefault(tile_name, idx)

            # player
            if 'player' in tags:
                player_positions.append((x,y))
                idx = tile_index['TileEmpty'] # @hack @hardcode TODO(jkk)

            if idx > 31:
                print(f"Error: tile index {idx} for '{tile_name}' exceeds 31 (fits in 5 bits) in level '{lname}'", file=sys.stderr)
                sys.exit(2)
            # compose byte: S (bit7), DD (bits6-5), TTTTT (bits4-0)
            byte = (is_solid << 7) | ((dir_val & 0x3) << 5) | (idx & 0x1F)
            col_tile_bytes[x][y] = byte

    # Validate player count
    if len(player_positions) > 1:
        coords = ", ".join(f"({x},{y})" for x,y in player_positions)
        print(f"Error: multiple player positions found in level '{lname}': {coords}", file=sys.stderr)
        sys.exit(2)
    px, py = player_positions[0] if player_positions else (0,0)

    per_level_data[lname] = {
        'grid': grid,
        'col_tile_bytes': col_tile_bytes,
        'player': (px, py)
    }

# --- emit assembly ---
out = []
out.append(";")
out.append("; Generated by tools/level2asm.py")
out.append(";")
out.append("")


for lname, data in per_level_data.items():
    out.append(";-------------------------------------------------------------------------------")
    out.append(f"{lname}:")
    out.append(";-------------------------------------------------------------------------------")
    out.append("")
    px, py = data['player']
    out.append(f".PlayerStartY: db {py}")
    out.append(f".PlayerStartX: db {px}")
    out.append("")
    out.append(".TileData: ; 1 byte per tile SDDTTTTT (col-major: 24 columns of 8 rows)")
    for x in range(24):
        entries = []
        for y in range(8):
            byte = data['col_tile_bytes'][x][y]
            entries.append(f"0b{byte:08b}")
        out.append("\tdb  " + ", ".join(entries))
    out.append("")

print("\n".join(out))
