#!/usr/bin/env python3
"""
Concatenate disassembly files, adding section comments for filename, block address,
and mode. Only concatenates ARM with ARM and x86 with x86.

Input: paths to .dis files (or directories containing .dis files).
Output: arm_combined.dis and x86_combined.dis (or --output directory).

Usage:
  python scripts/concat_disasm.py block_00001966_thumb.dis block_00001bbc_thumb.dis
  python scripts/concat_disasm.py out/dis/
  python scripts/concat_disasm.py out/dis/ -o combined/
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# First-line patterns to detect type and extract block/mode
ARM_HEADER = re.compile(
    r"^\s*;\s*Block\s+(0x[0-9a-fA-F]+)\s*-\s*(0x[0-9a-fA-F]+)\s+\((ARM|Thumb)\)",
    re.IGNORECASE,
)
X86_HEADER = re.compile(
    r"^\s*;\s*x86-64\s+JIT\s+\(GBA\s+block\s+(0x[0-9a-fA-F]+)\s*-\s*(0x[0-9a-fA-F]+)\s+(ARM|Thumb)\)",
    re.IGNORECASE,
)


def detect_and_parse(path: Path, text: str) -> tuple[str | None, str | None, str | None, str | None]:
    """
    Detect disassembly type (arm or x86) and parse block start, end, mode from first lines.
    Returns (kind, start_hex, end_hex, mode) or (None, None, None, None) if unknown.
    """
    lines = text.splitlines()
    for line in lines[:15]:
        line_stripped = line.strip()
        if not line_stripped or not line_stripped.startswith(";"):
            continue
        m = ARM_HEADER.match(line)
        if m:
            return ("arm", m.group(1), m.group(2), m.group(3))
        m = X86_HEADER.match(line)
        if m:
            return ("x86", m.group(1), m.group(2), m.group(3))

    # Fallback: detect by filename (e.g. block_00001966_x86.dis vs block_00001966_thumb.dis)
    name = path.name.lower()
    if "_x86.dis" in name or name.endswith("_x86.dis"):
        return ("x86", "?", "?", "?")
    if "_arm.dis" in name or "_thumb.dis" in name or name.endswith("_arm.dis") or name.endswith("_thumb.dis"):
        return ("arm", "?", "?", "?")
    return (None, None, None, None)


def collect_dis_files(paths: list[Path]) -> list[Path]:
    """Expand paths to a sorted list of .dis files (recurse into dirs for *.dis)."""
    out: list[Path] = []
    for p in paths:
        if p.is_file():
            if p.suffix == ".dis" or p.name.endswith(".dis"):
                out.append(p.resolve())
        elif p.is_dir():
            out.extend(sorted(p.glob("*.dis")))
        else:
            sys.stderr.write("warning: not a file or directory, skipping: %s\n" % p)
    return sorted(set(out))


def section_comment(basename: str, start: str, end: str, mode: str) -> str:
    return "; === %s (Block %s - %s %s) ===" % (basename, start, end, mode)


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Concatenate ARM/Thumb and x86 disassemblies with section comments."
    )
    ap.add_argument(
        "paths",
        nargs="+",
        type=Path,
        help="Paths to .dis files or directories containing .dis files",
    )
    ap.add_argument(
        "-o", "--output",
        type=Path,
        default=None,
        help="Output directory for arm_combined.dis and x86_combined.dis (default: cwd)",
    )
    ap.add_argument(
        "--arm",
        type=Path,
        default=None,
        help="Output path for ARM-only combined file (overrides -o for ARM)",
    )
    ap.add_argument(
        "--x86",
        type=Path,
        default=None,
        help="Output path for x86-only combined file (overrides -o for x86)",
    )
    args = ap.parse_args()

    files = collect_dis_files(args.paths)
    if not files:
        print("error: no .dis files found", file=sys.stderr)
        sys.exit(1)

    arm_chunks: list[str] = []
    x86_chunks: list[str] = []

    for f in files:
        text = f.read_text()
        kind, start, end, mode = detect_and_parse(f, text)
        if kind is None:
            sys.stderr.write("warning: could not detect ARM/x86 for %s, skipping\n" % f)
            continue
        header = section_comment(f.name, start or "?", end or "?", mode or "?")
        block = header + "\n" + text.strip() + "\n"
        if kind == "arm":
            arm_chunks.append(block)
        else:
            x86_chunks.append(block)

    out_dir = args.output or Path.cwd()
    arm_out = args.arm if args.arm is not None else out_dir / "arm_combined.dis"
    x86_out = args.x86 if args.x86 is not None else out_dir / "x86_combined.dis"

    if arm_chunks:
        out_path = arm_out
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text("\n".join(arm_chunks))
        print("Wrote %d ARM/Thumb block(s) to %s" % (len(arm_chunks), out_path))
    else:
        print("No ARM/Thumb disassemblies to concatenate.")

    if x86_chunks:
        out_path = x86_out
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text("\n".join(x86_chunks))
        print("Wrote %d x86 block(s) to %s" % (len(x86_chunks), out_path))
    else:
        print("No x86 disassemblies to concatenate.")


if __name__ == "__main__":
    main()
