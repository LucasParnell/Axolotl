#!/usr/bin/env python3
"""
Delete block dumps that don't exist in both analysis/t1 and analysis/t2.

Keeps only blocks present in both folders so you can diff (e.g. disasm_dump.py)
old emulator vs Axolotl for the same blocks. Dump format is the same
(block_*_meta.txt, _arm/_thumb.bin, _x86.bin, _ir.txt).

With --concat, runs concat_disasm.py on t1 and on t2 separately, writing
arm_combined.dis and x86_combined.dis into each folder (or --concat-output dirs).

Usage:
  python scripts/sync_analysis_blocks.py
  python scripts/sync_analysis_blocks.py --base out/analysis
  python scripts/sync_analysis_blocks.py --concat
  python scripts/sync_analysis_blocks.py --concat -o combined/
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def block_prefixes_from_dir(path: Path) -> set[str]:
    """Return set of block prefixes (e.g. 'block_00001966') from block_*_meta.txt in path."""
    if not path.is_dir():
        return set()
    out = set()
    for f in path.glob("block_*_meta.txt"):
        out.add(f.stem.replace("_meta", ""))
    return out


def files_for_block(block_prefix: str, dir_path: Path) -> list[Path]:
    """All dump files for this block in dir_path (block_<prefix>_*)."""
    return list(dir_path.glob(f"{block_prefix}_*"))


def run_concat(analysis_dir: Path, output_dir: Path, script_path: Path) -> bool:
    """Run concat_disasm.py on analysis_dir, writing arm_combined.dis and x86_combined.dis to output_dir."""
    out_dir = output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [
            sys.executable,
            str(script_path),
            str(analysis_dir.resolve()),
            "-o",
            str(out_dir),
        ],
        capture_output=False,
    )
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Delete block dumps that don't exist in both t1 and t2."
    )
    parser.add_argument(
        "--base",
        type=Path,
        default=Path("analysis"),
        help="Base directory containing t1 and t2 (default: analysis)",
    )
    parser.add_argument(
        "--t1",
        default="t1",
        help="First dump subdir (default: t1)",
    )
    parser.add_argument(
        "--t2",
        default="t2",
        help="Second dump subdir (default: t2)",
    )
    parser.add_argument(
        "--concat",
        action="store_true",
        help="After syncing, run concat_disasm.py on t1 and on t2 separately (writes arm_combined.dis, x86_combined.dis into each dir)",
    )
    parser.add_argument(
        "-o", "--concat-output",
        type=Path,
        default=None,
        metavar="DIR",
        help="With --concat: write t1 concat to DIR/t1, t2 concat to DIR/t2 (default: write into t1 and t2 themselves)",
    )
    args = parser.parse_args()

    base = args.base.resolve()
    t1_dir = base / args.t1
    t2_dir = base / args.t2

    if not t1_dir.is_dir():
        print(f"error: not a directory: {t1_dir}", file=sys.stderr)
        return 1
    if not t2_dir.is_dir():
        print(f"error: not a directory: {t2_dir}", file=sys.stderr)
        return 1

    blocks_t1 = block_prefixes_from_dir(t1_dir)
    blocks_t2 = block_prefixes_from_dir(t2_dir)
    common = blocks_t1 & blocks_t2

    to_drop_t1 = blocks_t1 - common
    to_drop_t2 = blocks_t2 - common

    deleted = 0
    for prefix in to_drop_t1:
        for f in files_for_block(prefix, t1_dir):
            f.unlink()
            print(f"Deleted {f}")
            deleted += 1
    for prefix in to_drop_t2:
        for f in files_for_block(prefix, t2_dir):
            f.unlink()
            print(f"Deleted {f}")
            deleted += 1

    print(f"Done: deleted {deleted} files ({len(to_drop_t1)} blocks from t1, {len(to_drop_t2)} from t2). {len(common)} blocks kept in both.")

    if args.concat:
        script_dir = Path(__file__).resolve().parent
        concat_script = script_dir / "concat_disasm.py"
        if not concat_script.exists():
            print(f"error: concat script not found: {concat_script}", file=sys.stderr)
            return 1
        if args.concat_output is not None:
            out_t1 = args.concat_output.resolve() / args.t1
            out_t2 = args.concat_output.resolve() / args.t2
        else:
            out_t1 = t1_dir
            out_t2 = t2_dir
        print("Concatenating t1...")
        if not run_concat(t1_dir, out_t1, concat_script):
            return 1
        print("Concatenating t2...")
        if not run_concat(t2_dir, out_t2, concat_script):
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
