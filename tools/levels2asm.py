#!/usr/bin/env python3
from __future__ import annotations
import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple, Callable
import sys
import warnings
from itertools import islice

LEVEL_WIDTH = 24
LEVEL_HEIGHT = 8
_BYTES_PER_LINE = 12
_WORDS_PER_LINE = 8  # emit 8 words (16 bytes) per 'dw' line for readability

@dataclass(frozen=True)
class TileDef:
    char: str
    name: str
    tags: List[str]
    index: Optional[int] = None

@dataclass
class Tile:
    char: str
    definition: TileDef

@dataclass
class Level:
    name: str
    width: int
    height: int
    grid: List[List[Tile]]  # grid[y][x]

# regexes
_asm_index_re = re.compile(r'^(?P<label>\w+)_Index\s+equ\s+(?P<idx>\d+)', re.IGNORECASE)
_mapping_line_re = re.compile(r'^(?P<char>.?)\s*=\s*(?P<name>\w+)(?P<rest>.*)$')
_tag_map_re = re.compile(r'^(?P<tag>#?\w+)\s*=\s*(?P<val>0x[0-9A-Fa-f]+|\d+)\s*$')

def parse_asm_indices(path: Path) -> Dict[str, int]:
    indices: Dict[str, int] = {}
    for ln in path.read_text(encoding="utf-8").splitlines():
        m = _asm_index_re.match(ln.strip())
        if m:
            indices[m.group("label")] = int(m.group("idx"))
    return indices

def parse_mapping_line(line: str) -> TileDef:
    m = _mapping_line_re.match(line)
    if not m:
        raise ValueError(f"invalid mapping line: {line!r}")
    ch = m.group("char")
    if len(ch) != 1:
        raise ValueError(f"mapping key must be a single character: {line!r}")
    name = m.group("name")
    tags = [t.lstrip("#") for t in m.group("rest").split() if t.startswith("#")]
    tags.append("needsRedraw")
    return TileDef(char=ch, name=name, tags=tags)

def nice_lines(path: Path):
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            comment_index = line.rfind(';')
            if comment_index >= 0:
                line = line[:comment_index]
            yield line
    yield None

def parse_input_with_tagmap(path: Path) -> Tuple[Dict[str, TileDef], Dict[str, int], List[Level]]:
    """
    Parse input file. Sections:
      - character mappings (lines like ". = TileEmpty #bg")
      - an optional tag->attribute mapping block (lines like: solid = 0x04)
      - levels (name line, then 8 rows of 24 chars)
    """
    mappings: Dict[str, TileDef] = {}
    tag_to_attr: Dict[str, int] = {
        "pushable"        : 1 << 7,
        "needsRedraw"     : 1 << 15,
    }
    levels: List[Level] = []

    lines = nice_lines(path)
    line = ""

    for line in lines:
        if line is None:
            raise ValueError(f"unexpected EOF while reading level '{level_name}'")
        if not line:
            break
        # if '=' not in line:
        #     continue
        td = parse_mapping_line(line)
        if td.char in mappings:
            raise ValueError(f"duplicate mapping for character {td.char!r}")
        mappings[td.char] = td

    while True:
        line = next(lines)
        if line is None:
            break
        if len(line) == 0:
            continue

        level_name = _sanitize_label(line)
        rows: List[List[Tile]] = []
        for row in islice(lines, 8):
            if row is None:
                raise ValueError(f"unexpected EOF while reading level '{level_name}', row {row_index}")

            if len(row) != LEVEL_WIDTH:
                raise ValueError(f"level '{level_name}' row {row_index} has length {len(row)} (expected {LEVEL_WIDTH})")

            for c in row:
                if c not in mappings:
                    raise ValueError(f"level '{level_name}' references unknown tile character {c!r}")

            rows.append([Tile(char=c, definition=mappings[c]) for c in row])

        levels.append(Level(name=level_name, width=LEVEL_WIDTH, height=LEVEL_HEIGHT, grid=rows))

    return mappings, tag_to_attr, levels

def attach_indices(mappings: Dict[str, TileDef], asm_indices: Dict[str, int], levels: List[Level]) -> None:
    # ensure every used tile name is present in asm_indices
    used_names: Set[str] = set()
    for lvl in levels:
        for row in lvl.grid:
            for t in row:
                used_names.add(t.definition.name)
    missing = [n for n in used_names if n not in asm_indices]
    if missing:
        raise ValueError(f"tiles referenced in levels but missing from tiles ASM: {', '.join(sorted(missing))}")
    # attach indices
    for ch, td in list(mappings.items()):
        idx = asm_indices.get(td.name)
        mappings[ch] = TileDef(char=td.char, name=td.name, tags=td.tags, index=idx)

# --- emit helpers ---
def _sanitize_label(name: str) -> str:
    return re.sub(r'[^0-9A-Za-z_]', '_', name)

def _emit_db_bytes(out: List[str], label: Optional[str], bytes_seq: List[int]) -> None:
    if label:
        out.append(f"{label}:")
    for i in range(0, len(bytes_seq), _BYTES_PER_LINE):
        chunk = bytes_seq[i : i + _BYTES_PER_LINE]
        out.append("    db " + ", ".join(f"0b{b:08b}" for b in chunk))

def _emit_dw_words(out: List[str], label: Optional[str], words: List[int]) -> None:
    """
    Emit a label (if provided) then the words in column-major order using 'dw'.
    Each word is formatted as 0xNNNN. Emits WORDS_PER_LINE per line.
    """
    if label:
        out.append(f"{label}:")
    for i in range(0, len(words), _WORDS_PER_LINE):
        chunk = words[i : i + _WORDS_PER_LINE]
        out.append("    dw " + ", ".join(f"0x{w:04x}" for w in chunk))

# --- new tile-word computation ---
def compute_tile_word(td: TileDef, tag_to_attr: Dict[str, int]) -> int:
    r"""
    Compute a 16-bit tile word for one tile definition according to the bitfield:

      Bits 0–1  (2 bits): groundTileImage        // what to draw when tile image is 0
      Bits 2–5  (4 bits): reserved1
      Bit 6     (1 bit) : isPushable
      Bit 7     (1 bit) : isSolid
      Bits 8–11 (4 bits): tileImageIndex         // primary tile image index (0–15)
      Bits 12–14(3 bits): reserved2
      Bit 15    (1 bit) : needsRedraw            // dirty bit (not set by tags by default)


    Old:
    SP....GG
    D...IIII

    New:
    PD.IIIII
    || \\\\\\__TileIndex___
    |\_________NeedsRedraw_
    \__________Pushable____

    """

    word = 0

    if td.index is None:
        raise RuntimeError(f"missing asm index for tile {td.name}")

    # groundTileImage bits 0-1 and tileImage bits 8-11
    if 0 <= td.index <= 3:
        ground = td.index
        tile_image = 0
    elif 0 <= td.index <= 15:
        ground = 0
        tile_image = td.index
    else:
        raise RuntimeError(f"asm index {td.index} out of bounds for {td.name}")

    word |= ground  # bits 0-1
    word |= (tile_image & 0xF) << 8

    for tag in td.tags:
        attr = tag_to_attr.get(tag, 0)
        word |= attr

    # Final bounds check
    if not (0 <= word <= 0xFFFF):
        raise RuntimeError(f"computed tile word out of 16-bit range: 0x{word:X}")
    return word

def level_to_column_major_words(lvl: Level, mappings: Dict[str, TileDef], tag_to_attr: Dict[str, int]) -> List[int]:
    """
    Produce a single column-major sequence of 16-bit words for the level,
    using compute_tile_word for each tile in the grid.
    Column-major ordering is preserved (x outer, y inner).
    """
    seq: List[int] = []
    for x in range(lvl.width):
        for y in range(lvl.height):
            td = mappings[lvl.grid[y][x].char]
            if td.index is None:
                raise RuntimeError(f"missing asm index for tile {td.name}")
            word = compute_tile_word(td, tag_to_attr)
            seq.append(word)
    return seq

def find_player_start(lvl: Level) -> Optional[Tuple[int, int]]:
    """Return (x,y) of first tile that has tag 'player', or None."""
    for y in range(lvl.height):
        for x in range(lvl.width):
            if 'player' in lvl.grid[y][x].definition.tags:
                return x, y
    return None

# --- main ---
def main() -> None:
    p = argparse.ArgumentParser(description="Emit levels as single 16-bit-per-tile assembly block using new tile-word format")
    p.add_argument("input", type=Path, help="input file (mapping + optional tag->attr block + levels)")
    p.add_argument("tiles_asm", type=Path, help="tiles assembly file (used only for indices/validation)")
    p.add_argument("output", type=Path, help="output assembly file to write")
    args = p.parse_args()

    asm_indices = parse_asm_indices(args.tiles_asm)
    mappings, tag_to_attr, levels = parse_input_with_tagmap(args.input)
    attach_indices(mappings, asm_indices, levels)

    out: List[str] = []
    for lvl in levels:
        label = _sanitize_label(lvl.name)
        out.append(";-------------------------------------------------------------------------------")
        out.append(f"{label}:")
        out.append(";-------------------------------------------------------------------------------")
        # player start (if any) - same naming convention but tied to sanitized label
        ps = find_player_start(lvl)
        if ps:
            x, y = ps
            out.append(f".PlayerStartY: db {y}")
            out.append(f".PlayerStartX: db {x}")
            out.append("")  # blank

        # produce single 16-bit-per-tile column-major block
        words = level_to_column_major_words(lvl, mappings, tag_to_attr)

        # Emit a readable dw block label (use .TileData suffix)
        _emit_dw_words(out, ".TileData", words)
        out.append("")  # blank between levels

    args.output.write_text("\n".join(out), encoding="utf-8")

if __name__ == "__main__":
    main()
