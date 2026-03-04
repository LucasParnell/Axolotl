#!/usr/bin/env python3
"""
Disassemble the last-executed block's ARM/Thumb and x86-64 bytes.
Invoked by the Axolotl executable when DISASM_LAST_BLOCK is defined (debug build).

Usage:
  python scripts/disasm_last_block.py --arm <path> --x86 <path> --pc <hex> [--thumb]

Requires: pip install capstone
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from capstone import (
        CS_ARCH_ARM,
        CS_ARCH_X86,
        CS_MODE_ARM,
        CS_MODE_THUMB,
        CS_MODE_64,
        Cs,
    )
except ImportError:
    print("error: capstone not installed. Run: pip install capstone", file=sys.stderr)
    sys.exit(1)


def disasm_arm_thumb(code: bytes, base_addr: int, is_thumb: bool) -> list[tuple[int, int, str]]:
    """Returns list of (addr, size, disasm_line)."""
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if is_thumb else CS_MODE_ARM)
    md.detail = True
    return [(i.address, i.size, f"{i.mnemonic}\t{i.op_str}") for i in md.disasm(code, base_addr)]


def disasm_x86(code: bytes, base_addr: int = 0) -> list[tuple[int, int, str]]:
    """Returns list of (addr, size, disasm_line)."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return [(i.address, i.size, f"{i.mnemonic}\t{i.op_str}") for i in md.disasm(code, base_addr)]


def main() -> int:
    parser = argparse.ArgumentParser(description="Disassemble last block ARM + x86 dumps (single block only)")
    parser.add_argument("--arm", required=True, type=Path, help="Path to ARM/Thumb binary dump")
    parser.add_argument("--x86", required=True, type=Path, help="Path to x86-64 binary dump")
    parser.add_argument("--pc", required=True, type=lambda s: int(s, 16), help="Block PC (hex)")
    parser.add_argument("--thumb", action="store_true", help="Block is Thumb (default: ARM)")
    parser.add_argument("--arm-len", type=int, default=None, metavar="N", help="Only disassemble first N bytes of ARM (default: whole file)")
    parser.add_argument("--x86-len", type=int, default=None, metavar="N", help="Only disassemble first N bytes of x86 (default: whole file)")
    parser.add_argument("--ir", type=Path, default=None, help="Path to IR dump for this block (optional, included after ARM/Thumb)")
    args = parser.parse_args()

    if not args.arm.exists():
        print(f"error: ARM dump not found: {args.arm}", file=sys.stderr)
        return 1
    if not args.x86.exists():
        print(f"error: x86 dump not found: {args.x86}", file=sys.stderr)
        return 1

    arm_bytes = args.arm.read_bytes()
    x86_bytes = args.x86.read_bytes()
    if args.arm_len is not None:
        arm_bytes = arm_bytes[: args.arm_len]
    if args.x86_len is not None:
        x86_bytes = x86_bytes[: args.x86_len]
    mode = "Thumb" if args.thumb else "ARM"

    print("=== Last block only (ARM/Thumb + x86-64) ===")
    print(f"PC=0x{args.pc:08x} ({mode})")
    print()
    print("--- ARM/Thumb ---")
    for addr, size, line in disasm_arm_thumb(arm_bytes, args.pc, args.thumb):
        print(f"  0x{addr:08x}:  {line}")
    if args.ir is not None and args.ir.exists():
        print()
        print("--- IR ---")
        print(args.ir.read_text().strip())
    print()
    print("--- x86-64 ---")
    for addr, size, line in disasm_x86(x86_bytes):
        print(f"  0x{addr:04x}:  {line}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
