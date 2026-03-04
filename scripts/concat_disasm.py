#!/usr/bin/env python3
"""
Concatenate disassembly files, adding section comments for filename, block address,
and mode. Only concatenates ARM with ARM and x86 with x86.

ARM/Thumb chunks may include an "--- IR ---" section (when block_*_ir.txt exists
or disasm_dump.py was run with IR). Concat treats each block as one chunk and
preserves ARM+IR combined content.

Input:
  - Paths to .dis files (or directories containing .dis files).
  - Directories containing analysis dumps (block_*_arm.bin, block_*_thumb.bin,
    block_*_ir.txt, block_*_meta.txt, block_*_x86.bin from the debug dump at end of execution).
    For analysis dirs, capstone is required (pip install capstone).

Output: arm_combined.dis and x86_combined.dis (or --output directory).

Usage:
  python scripts/concat_disasm.py block_00001966_thumb.dis block_00001bbc_thumb.dis
  python scripts/concat_disasm.py out/dis/
  python scripts/concat_disasm.py out/analysis/   # analysis dump from Axolotl run
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


def is_analysis_dir(path: Path) -> bool:
    """True if path is a directory containing block_*_meta.txt (analysis dump)."""
    return path.is_dir() and bool(list(path.glob("block_*_meta.txt")))


def collect_from_analysis_dir(
    analysis_dir: Path,
) -> tuple[list[str], list[str]]:
    """
    Disassemble analysis dump (block_*_meta.txt + _arm/_thumb.bin + _x86.bin)
    using disasm_dump logic. Returns (arm_chunks, x86_chunks).
    Requires capstone (disasm_dump will exit with an error if not installed).
    """
    script_dir = Path(__file__).resolve().parent
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))
    from disasm_dump import (
        block_prefix_from_meta_path,
        format_arm_block,
        format_x86_block,
        parse_meta,
    )

    arm_chunks: list[str] = []
    x86_chunks: list[str] = []
    meta_files = sorted(analysis_dir.glob("block_*_meta.txt"))
    for meta_path in meta_files:
        prefix = block_prefix_from_meta_path(meta_path)
        meta = parse_meta(meta_path)
        is_thumb = meta["mode"] == "Thumb"
        arm_suffix = "_thumb.bin" if is_thumb else "_arm.bin"
        arm_path = analysis_dir / (prefix + arm_suffix)
        x86_path = analysis_dir / (prefix + "_x86.bin")
        if not arm_path.exists():
            sys.stderr.write("warning: missing %s, skipping block %s\n" % (arm_path.name, prefix))
            continue
        if not x86_path.exists():
            sys.stderr.write("warning: missing %s, skipping block %s\n" % (x86_path.name, prefix))
            continue
        arm_bytes = arm_path.read_bytes()
        x86_bytes = x86_path.read_bytes()
        start = meta["start"]
        ir_path = analysis_dir / (prefix + "_ir.txt")
        ir_text = ir_path.read_text() if ir_path.exists() else None
        arm_text = format_arm_block(meta, arm_bytes, start, include_context=False, ir_text=ir_text)
        x86_text = format_x86_block(meta, x86_bytes, 0, include_context=False)
        section_name_arm = prefix + arm_suffix.replace(".bin", "")
        section_name_x86 = prefix + "_x86"
        start_hex = "0x%08x" % meta["start"]
        end_hex = "0x%08x" % meta["end"]
        mode = meta["mode"]
        header_arm = section_comment(section_name_arm, start_hex, end_hex, mode)
        header_x86 = section_comment(section_name_x86, start_hex, end_hex, mode)
        arm_chunks.append(header_arm + "\n" + arm_text.strip() + "\n")
        x86_chunks.append(header_x86 + "\n" + x86_text.strip() + "\n")
    return arm_chunks, x86_chunks


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
        help="Paths to .dis files, or directories containing .dis files or analysis dumps (block_*_meta.txt + _arm/_thumb.bin + _x86.bin)",
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

    arm_chunks: list[str] = []
    x86_chunks: list[str] = []

    # Process analysis directories first (block_*_meta.txt + .bin)
    analysis_dirs = [p.resolve() for p in args.paths if is_analysis_dir(p)]
    if analysis_dirs:
        try:
            import capstone  # noqa: F401
        except ImportError:
            print(
                "error: for analysis directories, capstone is required. Run: pip install capstone",
                file=sys.stderr,
            )
            sys.exit(1)
    for adir in sorted(analysis_dirs):
        try:
            a_chunks, x_chunks = collect_from_analysis_dir(adir)
            arm_chunks.extend(a_chunks)
            x86_chunks.extend(x_chunks)
        except Exception as e:
            print("error: failed to process analysis dir %s: %s" % (adir, e), file=sys.stderr)
            sys.exit(1)

    # Process .dis files from paths that are not analysis dirs
    other_paths = [p for p in args.paths if not is_analysis_dir(p.resolve())]
    files = collect_dis_files(other_paths)
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

    if not arm_chunks and not x86_chunks:
        print("error: no .dis files and no analysis blocks found", file=sys.stderr)
        sys.exit(1)

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
