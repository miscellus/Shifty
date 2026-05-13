#!/usr/bin/env python3
from __future__ import annotations
import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

LEVEL_WIDTH = 24
LEVEL_HEIGHT = 8
_BYTES_PER_LINE = 12

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
        # We'll read ahead until a non-empty, non-comment line is found.
        # If that line contains '=' we treat it as tag map entries; otherwise it's the start of levels.
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
                if not (0 <= attr <= 0xFF):
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

def level_to_column_major_fg(lvl: Level, mappings: Dict[str, TileDef], tag_to_attr: Dict[str, int]) -> List[int]:
    """
    Foreground byte = tile index OR attribute bits (from tags present on the tile).
    Attribute bits are taken from tag_to_attr: for each tag in tile.tags, OR its value.
    Column-major ordering.
    """
    seq: List[int] = []
    for x in range(lvl.width):
        for y in range(lvl.height):
            td = mappings[lvl.grid[y][x].char]
            if td.index is None:
                raise RuntimeError(f"missing asm index for tile {td.name}")
            attr = 0
            for tag in td.tags:
                attr |= tag_to_attr.get(tag, 0)
            seq.append((td.index & 0xFF) | (attr & 0xFF))
    return seq

def level_to_column_major_bg(lvl: Level, mappings: Dict[str, TileDef], tag_to_attr: Dict[str, int], default_bg_char: Optional[str]) -> List[int]:
    """
    Background index per cell:
      - if tile has tag 'bg' and that tile has an index, use that index
      - else use default_bg_char's index (if provided and present)
      - else pick first mapping with 'bg' tag
    Column-major ordering.
    """
    # find default bg index
    default_idx = None
    if default_bg_char and default_bg_char in mappings:
        default_idx = mappings[default_bg_char].index
    if default_idx is None:
        for td in mappings.values():
            if 'bg' in td.tags and td.index is not None:
                default_idx = td.index
                break
    if default_idx is None:
        default_idx = 0
    seq: List[int] = []
    for x in range(lvl.width):
        for y in range(lvl.height):
            td = mappings[lvl.grid[y][x].char]
            if 'bg' in td.tags and td.index is not None:
                seq.append(td.index)
            else:
                seq.append(default_idx)
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
    p = argparse.ArgumentParser(description="Emit levels as assembly (fg + bg), with tag->attribute mapping and player start")
    p.add_argument("input", type=Path, help="input file (mapping + optional tag->attr block + levels)")
    p.add_argument("tiles_asm", type=Path, help="tiles assembly file (used only for indices/validation)")
    p.add_argument("output", type=Path, help="output assembly file to write")
    args = p.parse_args()

    asm_indices = parse_asm_indices(args.tiles_asm)
    mappings, tag_to_attr, levels = parse_input_with_tagmap(args.input)
    attach_indices(mappings, asm_indices, levels)

    default_bg_char = '.' if '.' in mappings else None

    out: List[str] = []
    for lvl in levels:
        label = _sanitize_label(lvl.name)
        out.append(";-------------------------------------------------------------------------------")
        out.append(f"{label}:")
        out.append(";-------------------------------------------------------------------------------")
        # player start (if any)
        ps = find_player_start(lvl)
        if ps:
            x, y = ps
            out.append(f".PlayerStartY: db {y}")
            out.append(f".PlayerStartX: db {x}")
            out.append("")  # blank

        fg = level_to_column_major_fg(lvl, mappings, tag_to_attr)
        bg = level_to_column_major_bg(lvl, mappings, tag_to_attr, default_bg_char)

        # emit foreground (label already used above; create a .FG suffix)
        _emit_db_bytes(out, ".FG", fg)
        out.append("")  # blank
        # emit background block
        _emit_db_bytes(out, ".BG", bg)
        out.append("")  # blank between levels

    args.output.write_text("\n".join(out), encoding="utf-8")

if __name__ == "__main__":
    main()
