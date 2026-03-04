#!/usr/bin/env python3
"""
Static check: assert RSP is 16-byte aligned immediately before every CALL
in x86-64 JIT output (System V AMD64 ABI requirement).

Reads the x86-64 section from disasm_dump.py output (.dis or stdout), or
disassembles a raw binary with capstone, and tracks RSP modulo 16 from
function entry (entry RSP ≡ 8 mod 16 after the caller's CALL pushed 8 bytes).
Reports any CALL that occurs when RSP % 16 != 0.

Usage:
  python scripts/check_rsp_alignment.py path/to/block_xxx_x86.dis
  python scripts/disasm_dump.py out/dump -o out/dis && python scripts/check_rsp_alignment.py out/dis/block_*_x86.dis
  python scripts/check_rsp_alignment.py --binary block_x86.bin  # capstone disasm then check
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

try:
    from capstone import CS_ARCH_X86, CS_MODE_64, Cs
except ImportError:
    Cs = None


# At function entry (after caller's CALL), RSP ≡ 8 (mod 16).
INITIAL_RSP_MOD = 8


def parse_dis_line(line: str) -> tuple[str | None, str | None, int | None]:
    """
    Parse a line like "  0x00000000:  sub  rsp, 0x48" or "  0x00000010:  call  rax".
    Returns (mnemonic, op_str, size_or_none). size_or_none is only for sub/add rsp.
    """
    line = line.strip()
    if not line or line.startswith(";"):
        return None, None, None
    m = re.match(r"0x[0-9a-fA-F]+\:\s+(\w+)\s*(.*)$", line)
    if not m:
        return None, None, None
    mnemonic = m.group(1).lower()
    op_str = m.group(2).strip()
    # Parse "rsp, 0x48" or "rsp, 72" for sub/add
    imm = None
    if mnemonic in ("sub", "add") and "rsp" in op_str.split(",")[0].lower():
        imm_m = re.search(r",\s*(?:0x([0-9a-fA-F]+)|(\d+))", op_str)
        if imm_m:
            if imm_m.group(1):
                imm = int(imm_m.group(1), 16)
            else:
                imm = int(imm_m.group(2), 10)
    return mnemonic, op_str, imm


def check_dis_file(path: Path) -> list[str]:
    """
    Read a .dis file, find the x86 section, track RSP mod 16, report violations.
    Returns list of error messages (empty if OK).
    """
    text = path.read_text()
    lines = text.splitlines()
    # Find "--- x86-64 JIT ---" or similar and take lines until next "---" or end
    in_x86 = False
    errors = []
    rsp_mod = INITIAL_RSP_MOD

    for i, line in enumerate(lines):
        if "x86" in line and "---" in line:
            in_x86 = True
            rsp_mod = INITIAL_RSP_MOD
            continue
        if in_x86 and line.strip().startswith("---") and "x86" not in line:
            in_x86 = False
            continue
        if not in_x86:
            continue

        mnemonic, op_str, imm = parse_dis_line(line)
        if mnemonic is None:
            continue

        if mnemonic == "call":
            if rsp_mod % 16 != 0:
                errors.append(
                    f"{path}:{i + 1}: CALL with RSP ≡ {rsp_mod} (mod 16); must be 0 for System V ABI"
                )
            continue
        if mnemonic == "sub" and imm is not None and "rsp" in (op_str.split(",")[0].lower()):
            rsp_mod = (rsp_mod - imm) % 16
            continue
        if mnemonic == "add" and imm is not None and "rsp" in (op_str.split(",")[0].lower()):
            rsp_mod = (rsp_mod + imm) % 16
            continue
        if mnemonic == "push":
            rsp_mod = (rsp_mod - 8) % 16
            continue
        if mnemonic == "pop":
            rsp_mod = (rsp_mod + 8) % 16
            continue

    return errors


def _parse_rsp_imm(op_str: str) -> int | None:
    """Parse 'rsp, 0x48' or 'rsp, 72' -> 72. Returns None if not rsp, imm."""
    if not op_str.strip().lower().startswith("rsp"):
        return None
    imm_m = re.search(r",\s*(?:0x([0-9a-fA-F]+)|(\d+))", op_str)
    if not imm_m:
        return None
    if imm_m.group(1):
        return int(imm_m.group(1), 16)
    return int(imm_m.group(2), 10)


def check_binary(data: bytes, base: int = 0) -> list[str]:
    """
    Disassemble raw x86-64 with capstone, track RSP, return list of violations.
    """
    if Cs is None:
        return ["capstone not installed; pip install capstone"]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    errors = []
    rsp_mod = INITIAL_RSP_MOD

    for insn in md.disasm(data, base):
        mnemonic = insn.mnemonic
        op_str = insn.op_str

        if mnemonic == "call":
            if rsp_mod % 16 != 0:
                errors.append(
                    f"0x{insn.address:x}: CALL with RSP ≡ {rsp_mod} (mod 16); must be 0 for System V ABI"
                )
            continue
        if mnemonic == "sub":
            imm = _parse_rsp_imm(op_str)
            if imm is not None:
                rsp_mod = (rsp_mod - imm) % 16
            continue
        if mnemonic == "add":
            imm = _parse_rsp_imm(op_str)
            if imm is not None:
                rsp_mod = (rsp_mod + imm) % 16
            continue
        if mnemonic == "push":
            rsp_mod = (rsp_mod - 8) % 16
            continue
        if mnemonic == "pop":
            rsp_mod = (rsp_mod + 8) % 16
            continue

    return errors


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Check that RSP is 16-byte aligned before every CALL in x86-64 JIT output."
    )
    ap.add_argument(
        "files",
        nargs="*",
        type=Path,
        help=".dis files (from disasm_dump.py) to check",
    )
    ap.add_argument(
        "--binary",
        type=Path,
        default=None,
        help="Raw x86-64 binary to disassemble and check",
    )
    ap.add_argument(
        "--base",
        type=lambda s: int(s, 0),
        default=0,
        help="Base address for --binary (default 0)",
    )
    args = ap.parse_args()

    all_errors = []

    if args.binary is not None:
        data = args.binary.read_bytes()
        all_errors.extend(check_binary(data, args.base))

    for path in args.files:
        if not path.exists():
            all_errors.append(f"{path}: file not found")
            continue
        all_errors.extend(check_dis_file(path))

    if all_errors:
        for e in all_errors:
            print(e, file=sys.stderr)
        sys.exit(1)
    print("OK: RSP 16-byte aligned before all CALLs.")


if __name__ == "__main__":
    main()
