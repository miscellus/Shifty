#!/usr/bin/env python3
from __future__ import annotations
import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple, Callable
import sys
import warnings

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
    return TileDef(char=ch, name=name, tags=tags)

def parse_input_with_tagmap(path: Path) -> Tuple[Dict[str, TileDef], Dict[str, int], List[Level]]:
    """
    Parse input file. Sections:
      - character mappings (lines like ". = TileEmpty #bg")
      - an optional tag->attribute mapping block (lines like: solid = 0x04)
      - levels (name line, then 8 rows of 24 chars)
    """
    mappings: Dict[str, TileDef] = {}
    tag_to_attr: Dict[str, int] = {}
    levels: List[Level] = []

    with path.open("r", encoding="utf-8") as f:
        # 1) mappings until blank line
        for raw in f:
            line = raw.strip()
            if not line:
                break
            if line.startswith(";") or (line.startswith("#") and "=" not in line):
                continue
            td = parse_mapping_line(line)
            if td.char in mappings:
                raise ValueError(f"duplicate mapping for character {td.char!r}")
            mappings[td.char] = td

        # 2) optional tag->attr mapping block: peek next non-empty non-comment line
        pending_lines: List[str] = []
        next_line = None
        for raw in f:
            stripped = raw.strip()
            if not stripped:
                continue  # skip blank while looking for start
            if stripped.startswith(";") or (stripped.startswith("#") and "=" not in stripped):
                continue
            next_line = stripped
            break

        if next_line is not None and "=" in next_line:
            # next_line is first tag map entry; parse tag map until blank line
            m = _tag_map_re.match(next_line)
            if not m:
                raise ValueError(f"invalid tag map line: {next_line!r}  (expected 'tag = 0xNN' or 'tag = N')")
            tag = m.group("tag").lstrip("#")
            val = m.group("val")
            tag_to_attr[tag] = int(val, 0)

            # continue reading tag map lines until blank line
            for raw in f:
                line = raw.strip()
                if not line:
                    break
                if line.startswith(";") or (line.startswith("#") and "=" not in line):
                    continue
                m = _tag_map_re.match(line)
                if not m:
                    raise ValueError(f"invalid tag map line: {line!r}  (expected 'tag = 0xNN' or 'tag = N')")
                tag = m.group("tag").lstrip("#")
                val = m.group("val")
                attr = int(val, 0)
                if not (0 <= attr <= 0xFFFF):
                    raise ValueError(f"attribute value out of range: {val}")
                tag_to_attr[tag] = attr
        else:
            # no tag map: if next_line exists and is not a tag map, push it back into an iterator
            if next_line is not None:
                pending_lines.append(next_line)

        # 3) levels
        # We'll create an iterator that yields pending_lines first, then the rest of file lines.
        def line_iter():
            for l in pending_lines:
                yield l + "\n"
            for l in f:
                yield l

        it = line_iter()
        while True:
            # find next non-empty name line
            for raw in it:
                name_line = raw.rstrip("\n")
                if name_line.strip():
                    break
            else:
                break  # EOF
            level_name = name_line.strip()
            rows: List[List[Tile]] = []
            for row_index in range(1, LEVEL_HEIGHT + 1):
                try:
                    row_raw = next(it)
                except StopIteration:
                    raise ValueError(f"unexpected EOF while reading level '{level_name}', row {row_index}")
                row = row_raw.rstrip("\n")
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
def compute_tile_word(td: TileDef, tag_to_attr: Dict[str, int], mapping_strategy: Optional[Callable[[int], int]] = None) -> int:
    """
    Compute a 16-bit tile word for one tile definition according to the bitfield:

      Bits 0–1  (2 bits): groundTileImage        // what to draw when tile image is 0
      Bits 2–4  (3 bits): reserved1
      Bit 5     (1 bit) : needsRedraw            // dirty bit (not set by tags by default)
      Bit 6     (1 bit) : isPushable
      Bit 7     (1 bit) : isSolid
      Bits 8–11 (4 bits): tileImage               // primary tile image index (0–15)
      Bits 12–15(4 bits): reserved2

    Mapping rules implemented:
      - tileImage (bits 8–11) comes from the tile's asm index reduced to 4 bits via mapping_strategy.
        Default mapping_strategy reduces via index % 16.
      - Exception: if the tile index is in 0..3 (a pure ground/background tile), tileImage is forced to 0
        and groundTileImage (bits 0–1) is set to that index.
      - groundTileImage (bits 0–1) is set to the tile index if tile index in 0..3; otherwise 0.
      - Tag-driven flags:
          - 'solid' sets bit 7 (isSolid)
          - 'pushable' sets bit 6 (isPushable)
          - 'player' does not set bits but is used elsewhere for player start
      - Entries in tag_to_attr that map to these semantic tags are ignored for per-byte OR logic;
        if a tag->attr mapping exists for a known semantic tag and its bit value disagrees with the
        semantic bit, a warning is emitted and the semantic mapping is preferred.
      - Any other tag_to_attr entries are currently ignored for tile-word bits (reserved/future use).
    """
    if td.index is None:
        raise RuntimeError(f"missing asm index for tile {td.name}")

    idx = td.index
    if mapping_strategy is None:
        mapping_strategy = lambda v: v & 0xF  # default: modulo 16

    word = 0

    # groundTileImage bits 0-1 and tileImage bits 8-11
    if 0 <= idx <= 3:
        # pure ground/background tile
        ground = idx & 0x3
        tile_image = 0
        word |= ground  # bits 0-1
        word |= (tile_image & 0xF) << 8
    else:
        ground = 0
        tile_image = mapping_strategy(idx) & 0xF
        word |= ground
        word |= (tile_image & 0xF) << 8

    # tag-driven flags
    # bit 7: isSolid
    if 'solid' in td.tags:
        word |= (1 << 7)
        # if tag_to_attr has 'solid' mapped, warn if conflicting
        if 'solid' in tag_to_attr:
            mapped = tag_to_attr['solid']
            expected = 1 << 7
            if mapped != expected:
                warnings.warn(
                    f"tag->attr mapping for 'solid' ({mapped:#04x}) conflicts with semantic bit (0x{expected:02X}); semantic mapping used"
                )
    else:
        # if tag_to_attr defines 'solid' but tile doesn't have tag, we don't set it
        pass

    # bit 6: isPushable
    if 'pushable' in td.tags:
        word |= (1 << 6)
        if 'pushable' in tag_to_attr:
            mapped = tag_to_attr['pushable']
            expected = 1 << 6
            if mapped != expected:
                warnings.warn(
                    f"tag->attr mapping for 'pushable' ({mapped:#04x}) conflicts with semantic bit (0x{expected:02X}); semantic mapping used"
                )

    # Note: other tags that appear in tag_to_attr are intentionally ignored for now.
    # If future behavior requires mapping arbitrary tag->attr into reserved bits, that can be implemented.
    # Documented behavior: tag->attr entries do not influence the defined named bits (solid/pushable/player)
    # and are ignored unless explicitly recognized above.

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
