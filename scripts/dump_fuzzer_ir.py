#!/usr/bin/env python3
"""
Dump a single file with each fuzzer instruction and its generated IR (micro-ops)
so you can compare ARM instructions to the IR by hand.

Usage:
  1. python3 scripts/fuzzer.py                    # generate analysis/fuzzer_out/fuzzer.txt
  2. cmake --build build && ./out/ir_dump --help   # build ir_dump (or use your build dir)
  3. python3 scripts/dump_fuzzer_ir.py [--ir-dump ./out/ir_dump] [--input ...] [--output ...]

Output: one file with blocks like:

  ========== Instruction 1 ==========
  Encoding: 0xe0800001
  Assembly: ADD r0, r0, r1
  Expected IR op: kAdd
  --- IR (micro-ops) ---
  0 | ADD         Rd: R0  Rn: R0  Rm: R1 | ...
  1 | BRANCH      ...   (sentinel so block ends; compare node 0)
  ================================

The last IR node in each block is a branch-to-self sentinel; the real micro-ops
for the instruction are the earlier nodes.
"""

import argparse
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description="Dump fuzzer instructions + generated IR to one file")
    parser.add_argument(
        "--input",
        default="analysis/fuzzer_out/fuzzer.txt",
        help="Input: lines of hex\\tasm\\texpected_IrOp (from fuzzer.py)",
    )
    parser.add_argument(
        "--output",
        default="analysis/fuzzer_out/fuzzer_ir_dump.txt",
        help="Output file with instructions and IR",
    )
    parser.add_argument(
        "--ir-dump",
        default=None,
        help="Path to ir_dump binary (default: out/ir_dump or build/out/ir_dump)",
    )
    args = parser.parse_args()

    ir_dump = args.ir_dump
    if not ir_dump:
        for cand in ["out/ir_dump", "build/out/ir_dump"]:
            if os.path.isfile(cand):
                ir_dump = cand
                break
        if not ir_dump:
            print("error: ir_dump binary not found. Build with: cmake --build build", file=sys.stderr)
            print("  or pass --ir-dump /path/to/ir_dump", file=sys.stderr)
            return 1
    if not os.path.isfile(ir_dump):
        print(f"error: not a file: {ir_dump}", file=sys.stderr)
        return 1

    try:
        with open(args.input, "r") as f:
            lines = [ln.strip() for ln in f if ln.strip()]
    except FileNotFoundError:
        print(f"error: input not found: {args.input}", file=sys.stderr)
        print("  Run first: python3 scripts/fuzzer.py", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    with open(args.output, "w") as out:
        for idx, line in enumerate(lines, 1):
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            hex_str = parts[0].strip()
            asm = parts[1].strip()
            expected_op = parts[2].strip() if len(parts) >= 3 else ""

            if not hex_str.startswith("0x"):
                continue
            hex_clean = hex_str[2:]
            try:
                result = subprocess.run(
                    [ir_dump, hex_clean],
                    capture_output=True,
                    text=True,
                    timeout=2,
                )
            except subprocess.TimeoutExpired:
                ir_text = "(ir_dump timed out)\n"
            except Exception as e:
                ir_text = f"(ir_dump error: {e})\n"
            else:
                ir_text = result.stdout or result.stderr or "(no output)\n"

            out.write("========== Instruction " + str(idx) + " ==========\n")
            out.write("Encoding: " + hex_str + "\n")
            out.write("Assembly: " + asm + "\n")
            if expected_op:
                out.write("Expected IR op: " + expected_op + "\n")
            out.write("--- IR (micro-ops) ---\n")
            out.write(ir_text)
            out.write("================================\n\n")

    print(f"Wrote {args.output} ({len(lines)} instructions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
