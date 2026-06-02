#!/usr/bin/env python3
"""
Generate 3D block-letter banner art for MYYOSYS (yosys_banner).

Usage:
    python tools/gen_banner_art.py              # preview in terminal
    python tools/gen_banner_art.py --emit-cc    # print C log() lines for yosys.cc
"""

from __future__ import annotations

import argparse
import re
import sys

# All glyphs: 12×8 body + 1 padding row; double-stroke (##) block style.
GW, GH = 12, 9
LETTER_GAP = 1
WORD = "IRIS"
MAX_VISIBLE_WIDTH = 92  # 7×12 + 6 gaps + 2px extrusion

# Extrusion offsets (dx, dy) from back to front; last layer is the face.
EXTRUSION = [
    ((2, 2), "shadow2"),
    ((1, 1), "shadow1"),
    ((0, 0), "face"),
]

GLYPHS: dict[str, list[str]] = {
    "M": [
        "##........##",
        "###......###",
        "####....####",
        "##.##..##.##",
        "##..####..##",
        "##...##...##",
        "##........##",
        "##........##",
        "............",
    ],
    "Y": [
        "##........##",
        ".##......##.",
        "..##....##..",
        "...##..##...",
        "....####....",
        ".....##.....",
        ".....##.....",
        ".....##.....",
        "............",
    ],
    "O": [
        "..########..",
        ".##......##.",
        "##........##",
        "##........##",
        "##........##",
        "##........##",
        ".##......##.",
        "..########..",
        "............",
    ],
    "S": [
        "..########..",
        "##........##",
        "##..........",
        "..########..",
        "..........##",
        "..........##",
        "##........##",
        "..########..",
        "............",
    ],
    "I": [
        "....####....",
        "....####....",
        "....####....",
        "....####....",
        "....####....",
        "....####....",
        "....####....",
        "....####....",
        "............",
    ],
    "R": [
        "########....",
        "##......##..",
        "##......##..",
        "########....",
        "##......##..",
        "##......##..",
        "##......##..",
        "##......##..",
        "............",
    ],
}


def glyph_rows(ch: str) -> list[str]:
    rows = GLYPHS[ch]
    for i, row in enumerate(rows):
        if len(row) != GW:
            raise ValueError(f"glyph {ch} row {i}: expected width {GW}, got {len(row)}")
    return rows


def glyph_width(ch: str) -> int:
    rows = glyph_rows(ch)
    return len(rows[0]) if rows else 0


def glyph_height(ch: str) -> int:
    return len(glyph_rows(ch))


def glyph_matrix(ch: str) -> list[list[bool]]:
    rows = glyph_rows(ch)
    return [[c == "#" for c in row] for row in rows]


def outline_matrix(mat: list[list[bool]]) -> list[list[bool]]:
    """Pixels on the glyph boundary (4-neighbor has empty or out-of-bounds)."""
    h = len(mat)
    w = len(mat[0]) if h else 0
    out = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not mat[y][x]:
                continue
            for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                ny, nx = y + dy, x + dx
                if ny < 0 or ny >= h or nx < 0 or nx >= w or not mat[ny][nx]:
                    out[y][x] = True
                    break
    return out


def blit(canvas: list[list[str | None]], mat: list[list[bool]], ox: int, oy: int, mark: str) -> None:
    h = len(canvas)
    w = len(canvas[0]) if h else 0
    for y, row in enumerate(mat):
        for x, filled in enumerate(row):
            if not filled:
                continue
            cy, cx = oy + y, ox + x
            if 0 <= cy < h and 0 <= cx < w:
                canvas[cy][cx] = mark


def compose_word(word: str = WORD) -> list[list[str | None]]:
    n = len(word)
    text_w = sum(glyph_width(c) for c in word) + (n - 1) * LETTER_GAP
    max_dx = max(dx for (dx, _), _ in EXTRUSION)
    max_dy = max(dy for (_, dy), _ in EXTRUSION)
    gh = max(glyph_height(c) for c in word)
    cw, ch = text_w + max_dx, gh + max_dy
    canvas: list[list[str | None]] = [[None] * cw for _ in range(ch)]

    ox = 0
    for letter in word:
        mat = glyph_matrix(letter)
        for (dx, dy), layer in EXTRUSION:
            # Shadow layers: outline only, so extrusion does not fill letter counters.
            src = mat if layer == "face" else outline_matrix(mat)
            blit(canvas, src, ox + dx, dy, layer)
        ox += glyph_width(letter) + LETTER_GAP

    return canvas


def visible_len(s: str) -> int:
    return len(re.sub(r"\033\[[0-9;]*m", "", s))


# ANSI styling per layer (face pops; shadows recede like KIMI-style extrusion)
STYLE = {
    "shadow2": "\033[90m",   # dim
    "shadow1": "\033[37m",   # light gray outline
    "face": "\033[1;96m",    # bold bright cyan
}
# ASCII-only glyphs: safe on Windows console without /utf-8 source encoding.
CHAR = {
    "shadow2": ".",
    "shadow1": ":",
    "face": "#",
}
RESET = "\033[0m"


def render_lines(canvas: list[list[str | None]], *, color: bool) -> list[str]:
    lines: list[str] = []
    for row in canvas:
        parts: list[str] = []
        prev_layer: str | None = None
        for cell in row:
            if cell is None:
                if prev_layer is not None:
                    parts.append(RESET if color else "")
                    prev_layer = None
                parts.append(" ")
            else:
                if color:
                    if cell != prev_layer:
                        if prev_layer is not None:
                            parts.append(RESET)
                        parts.append(STYLE[cell])
                        prev_layer = cell
                    parts.append(CHAR[cell])
                else:
                    parts.append(CHAR[cell])
        if color and prev_layer is not None:
            parts.append(RESET)
        line = "".join(parts)
        pad = MAX_VISIBLE_WIDTH - visible_len(line)
        if pad > 0:
            line += " " * pad
        lines.append(line)
    return lines


def to_c_literal(s: str) -> str:
    """Escape for a narrow C string literal (ASCII + ANSI escapes only)."""
    out: list[str] = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\033":
            out.append("\\033")
        else:
            out.append(ch)
    return "".join(out)


def emit_cc(lines: list[str], out_path: str | None = None) -> None:
    buf: list[str] = []
    for line in lines:
        escaped = to_c_literal(line)
        buf.append(
            f'\tlog("%s|   %s%s    |%s\\n", cyan, "{escaped}", cyan, reset);'
        )
    text = "\n".join(buf) + "\n"
    if out_path:
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate MYYOSYS 3D banner art")
    parser.add_argument("--emit-cc", action="store_true", help="Emit C log() lines")
    parser.add_argument(
        "-o",
        "--output",
        metavar="FILE",
        help="Write --emit-cc output to FILE (UTF-8)",
    )
    parser.add_argument("--no-color", action="store_true", help="Plain block preview")
    args = parser.parse_args()

    canvas = compose_word()
    lines = render_lines(canvas, color=not args.no_color and not args.emit_cc)

    if args.emit_cc:
        # Per-layer ANSI inside the art string; outer cyan/bold wraps the line.
        colored = render_lines(canvas, color=True)
        emit_cc(colored, args.output)
        return 0

    for line in lines:
        print(line)
    print(f"\n# {len(lines)} lines, visible width <= {MAX_VISIBLE_WIDTH}", file=sys.stderr)
    w = max(visible_len(l) for l in lines)
    print(f"# max visible width: {w}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
