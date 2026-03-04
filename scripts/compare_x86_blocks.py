#!/usr/bin/env python3
"""
Compare x86 block dumps across two analysis directories while normalizing away
host-emitter noise (register allocation, immediates, helper pointers).

Usage examples:
  scripts/venv/bin/python scripts/compare_x86_blocks.py analysis/t1 analysis/t2
  scripts/venv/bin/python scripts/compare_x86_blocks.py analysis/t1 analysis/t2 \
      --poi 0x219e 0x21ce 0x23c6 0x257c
"""

from __future__ import annotations

import argparse
import difflib
import re
from dataclasses import dataclass
from pathlib import Path

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    HAVE_CAPSTONE = True
except Exception:
    HAVE_CAPSTONE = False


BLOCK_RE = re.compile(r"block_([0-9A-Fa-f]{8})_x86\.bin$")
ARM_RE = re.compile(r"block_([0-9A-Fa-f]{8})_(arm|thumb)\.bin$")
META_OR_IR_RE = re.compile(r"block_([0-9A-Fa-f]{8})_(meta\.txt|ir\.txt)$")


@dataclass
class Insn:
    text: str
    sig: str


def _is_boilerplate(text: str) -> bool:
    t = text.strip().lower()
    if t.startswith("push\t") or t.startswith("pop\t") or t.startswith("ret"):
        return True
    if t.startswith("sub\trsp,") or t.startswith("add\trsp,"):
        return True
    if t.startswith("mov\tr13, rdi") or t.startswith("mov\trdi, r13"):
        return True
    if t.startswith("mov\tqword ptr [rsp +"):
        return True
    if t.startswith("mov\t") and ", dword ptr [rdi +" in t:
        return True
    if t.startswith("mov\tdword ptr [rdi +"):
        return True
    if t.startswith("mov\tqword ptr [rdi +"):
        return True
    if t.startswith("mov\trbp, rsp"):
        return True
    if t.startswith("mov\tqword ptr [rsp], rbx"):
        return True
    return False


def _is_ctrl_insn(text: str) -> bool:
    mnem = text.split("\t", 1)[0].strip().lower()
    if mnem in {"cmp", "test", "call", "ret", "jmp"}:
        return True
    if mnem.startswith("j"):  # jz/jnz/jb/...
        return True
    if mnem.startswith("set"):  # setc/setz/...
        return True
    if mnem.startswith("cmov"):
        return True
    return False


def parse_pc_list(values: list[str]) -> list[int]:
    out: list[int] = []
    for v in values:
        out.append(int(v, 16) if v.lower().startswith("0x") else int(v, 16))
    return out


def scan_x86_blocks(root: Path) -> dict[int, Path]:
    out: dict[int, Path] = {}
    for f in root.iterdir():
        if not f.is_file():
            continue
        m = BLOCK_RE.match(f.name)
        if not m:
            continue
        out[int(m.group(1), 16)] = f
    return out


def scan_arm_payload_blocks(root: Path) -> dict[int, str]:
    out: dict[int, str] = {}
    for f in root.iterdir():
        if not f.is_file():
            continue
        m = ARM_RE.match(f.name)
        if not m:
            continue
        out[int(m.group(1), 16)] = m.group(2)
    return out


def scan_non_x86_candidates(root: Path) -> set[int]:
    pcs: set[int] = set()
    for f in root.iterdir():
        if not f.is_file():
            continue
        m_arm = ARM_RE.match(f.name)
        if m_arm:
            pcs.add(int(m_arm.group(1), 16))
            continue
        m_misc = META_OR_IR_RE.match(f.name)
        if m_misc:
            pcs.add(int(m_misc.group(1), 16))
    return pcs


def parse_t1_index_order(path: Path) -> list[int]:
    out: list[int] = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        m = re.search(
            r"block_([0-9A-Fa-f]{8})_(?:arm|thumb)\.bin\s+block_([0-9A-Fa-f]{8})_x86\.bin",
            line,
        )
        if not m:
            continue
        pc_a = int(m.group(1), 16)
        pc_b = int(m.group(2), 16)
        if pc_a == pc_b:
            out.append(pc_a)
    return out


def parse_x86_combined_order(path: Path) -> list[int]:
    out: list[int] = []
    if not path.exists():
        return out
    for line in path.read_text().splitlines():
        m = re.search(r"; === block_([0-9A-Fa-f]{8})_x86", line)
        if m:
            out.append(int(m.group(1), 16))
    return out


def unique_preserve_order(seq: list[int]) -> list[int]:
    seen: set[int] = set()
    out: list[int] = []
    for x in seq:
        if x in seen:
            continue
        seen.add(x)
        out.append(x)
    return out


def lcs_last(a: list[int], b: list[int]) -> tuple[int, int | None]:
    if not a or not b:
        return 0, None
    n = len(a)
    m = len(b)
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        ai = a[i - 1]
        row = dp[i]
        prev = dp[i - 1]
        for j in range(1, m + 1):
            if ai == b[j - 1]:
                row[j] = prev[j - 1] + 1
            else:
                row[j] = row[j - 1] if row[j - 1] >= prev[j] else prev[j]
    i, j = n, m
    seq_rev: list[int] = []
    while i > 0 and j > 0:
        if a[i - 1] == b[j - 1]:
            seq_rev.append(a[i - 1])
            i -= 1
            j -= 1
        elif dp[i - 1][j] >= dp[i][j - 1]:
            i -= 1
        else:
            j -= 1
    if not seq_rev:
        return 0, None
    return len(seq_rev), seq_rev[0]


def disasm_x86(path: Path) -> list[Insn]:
    if not HAVE_CAPSTONE:
        raise RuntimeError("capstone is required for x86 disassembly comparison")
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    code = path.read_bytes()
    out: list[Insn] = []
    for insn in md.disasm(code, 0):
        kinds = []
        for op in insn.operands:
            if op.type == X86_OP_REG:
                kinds.append("R")
            elif op.type == X86_OP_MEM:
                kinds.append("M")
            elif op.type == X86_OP_IMM:
                kinds.append("I")
            else:
                kinds.append("O")
        sig = f"{insn.mnemonic}:{''.join(kinds)}"
        out.append(Insn(text=f"{insn.mnemonic}\t{insn.op_str}", sig=sig))
    return out


def compare_block(a: list[Insn], b: list[Insn]) -> tuple[float, int, str]:
    sig_a = [x.sig for x in a]
    sig_b = [x.sig for x in b]
    ratio = difflib.SequenceMatcher(a=sig_a, b=sig_b).ratio()
    k = min(len(sig_a), len(sig_b))
    first_diff = -1
    for i in range(k):
        if sig_a[i] != sig_b[i]:
            first_diff = i
            break
    if first_diff == -1 and len(sig_a) != len(sig_b):
        first_diff = k
    if first_diff == -1:
        return ratio, first_diff, "identical-signature"

    a_txt = a[first_diff].text if first_diff < len(a) else "<end>"
    b_txt = b[first_diff].text if first_diff < len(b) else "<end>"
    a_sig = sig_a[first_diff] if first_diff < len(sig_a) else "<end>"
    b_sig = sig_b[first_diff] if first_diff < len(sig_b) else "<end>"
    detail = (
        f"idx={first_diff} "
        f"A[{a_sig}] {a_txt}  |  B[{b_sig}] {b_txt}"
    )
    return ratio, first_diff, detail


def compare_block_core(a: list[Insn], b: list[Insn]) -> tuple[float, int, str, int, int]:
    fa = [x for x in a if not _is_boilerplate(x.text)]
    fb = [x for x in b if not _is_boilerplate(x.text)]
    ratio, first_diff, detail = compare_block(fa, fb)
    return ratio, first_diff, detail, len(fa), len(fb)


def compare_block_ctrl(a: list[Insn], b: list[Insn]) -> tuple[float, int, str, int, int]:
    fa = [x for x in a if _is_ctrl_insn(x.text)]
    fb = [x for x in b if _is_ctrl_insn(x.text)]
    ratio, first_diff, detail = compare_block(fa, fb)
    return ratio, first_diff, detail, len(fa), len(fb)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir_a", type=Path)
    ap.add_argument("dir_b", type=Path)
    ap.add_argument("--poi", nargs="*", default=[])
    ap.add_argument("--max-blocks", type=int, default=200)
    args = ap.parse_args()

    a_map = scan_x86_blocks(args.dir_a)
    b_map = scan_x86_blocks(args.dir_b)
    a_arm_map = scan_arm_payload_blocks(args.dir_a)
    b_arm_map = scan_arm_payload_blocks(args.dir_b)

    a_pcs = set(a_map.keys())
    b_pcs = set(b_map.keys())
    both = sorted(a_pcs & b_pcs)
    a_only = sorted(a_pcs - b_pcs)
    b_only = sorted(b_pcs - a_pcs)

    a_non_x86 = scan_non_x86_candidates(args.dir_a) - a_pcs
    b_non_x86 = scan_non_x86_candidates(args.dir_b) - b_pcs

    order_a = parse_t1_index_order(args.dir_a / "index.txt")
    if not order_a:
        order_a = parse_x86_combined_order(args.dir_a / "x86_combined.dis")
    order_b = parse_t1_index_order(args.dir_b / "index.txt")
    if not order_b:
        order_b = parse_x86_combined_order(args.dir_b / "x86_combined.dis")
    order_a = unique_preserve_order(order_a)
    order_b = unique_preserve_order(order_b)

    print(f"A={args.dir_a} blocks={len(a_pcs)}")
    print(f"B={args.dir_b} blocks={len(b_pcs)}")
    print(f"common={len(both)} A_only={len(a_only)} B_only={len(b_only)}")
    print(f"A_non_x86_filtered={len(a_non_x86)} B_non_x86_filtered={len(b_non_x86)}")

    in_b = set(order_b)
    in_a = set(order_a)
    last_common_a_order = next((pc for pc in reversed(order_a) if pc in b_pcs), None)
    last_common_b_order = next((pc for pc in reversed(order_b) if pc in a_pcs), None)
    a_exec_filtered = [pc for pc in order_a if pc in a_pcs and pc in b_pcs]
    b_exec_filtered = [pc for pc in order_b if pc in a_pcs and pc in b_pcs]
    lcs_len, lcs_last_pc = lcs_last(a_exec_filtered, b_exec_filtered)

    last_common_arm = None
    for pc in reversed(order_a):
        if pc in a_pcs and pc in b_pcs and pc in a_arm_map and pc in b_arm_map:
            last_common_arm = (pc, a_arm_map[pc], b_arm_map[pc])
            break

    print("")
    print("== Last-Common Summary (x86-only / prewarm-no-x86 filtered) ==")
    print(f"last_common_in_A_order={f'0x{last_common_a_order:08X}' if last_common_a_order is not None else 'none'}")
    print(f"last_common_in_B_order={f'0x{last_common_b_order:08X}' if last_common_b_order is not None else 'none'}")
    print(f"lcs_len={lcs_len} lcs_last={f'0x{lcs_last_pc:08X}' if lcs_last_pc is not None else 'none'}")
    if last_common_arm is not None:
        pc, mode_a, mode_b = last_common_arm
        print(f"last_common_arm_payload=0x{pc:08X} A_mode={mode_a} B_mode={mode_b}")
    else:
        print("last_common_arm_payload=none")

    if args.poi:
        target = parse_pc_list(args.poi)
    else:
        target = both[: args.max_blocks]

    print("")
    print("== POI Status ==")
    for pc in target:
        in_a = pc in a_map
        in_b = pc in b_map
        status = "both" if in_a and in_b else ("A-only" if in_a else ("B-only" if in_b else "none"))
        print(f"0x{pc:08X} {status}")

    comparable = [pc for pc in target if pc in a_map and pc in b_map]
    if not HAVE_CAPSTONE and comparable:
        print("")
        print("capstone not available; skipping instruction-level x86 diff.")
        comparable = []
    print("")
    print(f"== Comparable ({len(comparable)}) ==")
    rows = []
    for pc in comparable:
        ins_a = disasm_x86(a_map[pc])
        ins_b = disasm_x86(b_map[pc])
        ratio_raw, first_diff_raw, detail_raw = compare_block(ins_a, ins_b)
        ratio_core, first_diff_core, detail_core, len_core_a, len_core_b = compare_block_core(ins_a, ins_b)
        ratio_ctrl, first_diff_ctrl, detail_ctrl, len_ctrl_a, len_ctrl_b = compare_block_ctrl(ins_a, ins_b)
        rows.append((
            ratio_core,
            pc,
            len(ins_a),
            len(ins_b),
            ratio_raw,
            first_diff_raw,
            detail_raw,
            len_core_a,
            len_core_b,
            first_diff_core,
            detail_core,
            ratio_ctrl,
            len_ctrl_a,
            len_ctrl_b,
            first_diff_ctrl,
            detail_ctrl,
        ))

    rows.sort(key=lambda x: x[0])  # lowest core similarity first
    for (
        ratio_core,
        pc,
        na,
        nb,
        ratio_raw,
        first_diff_raw,
        detail_raw,
        len_core_a,
        len_core_b,
        first_diff_core,
        detail_core,
        ratio_ctrl,
        len_ctrl_a,
        len_ctrl_b,
        first_diff_ctrl,
        detail_ctrl,
    ) in rows[: args.max_blocks]:
        print(
            f"0x{pc:08X} core_sim={ratio_core:.3f} core_lenA={len_core_a} core_lenB={len_core_b} "
            f"raw_sim={ratio_raw:.3f} lenA={na} lenB={nb} "
            f"ctrl_sim={ratio_ctrl:.3f} ctrl_lenA={len_ctrl_a} ctrl_lenB={len_ctrl_b} "
            f"core_diff={first_diff_core} ctrl_diff={first_diff_ctrl} raw_diff={first_diff_raw}"
        )
        print(f"  core: {detail_core}")
        print(f"  ctrl: {detail_ctrl}")
        print(f"  raw : {detail_raw}")
        print("")

    if a_only:
        print("")
        print("== A-only sample ==")
        print(" ".join(f"0x{x:08X}" for x in a_only[:80]))
    if b_only:
        print("")
        print("== B-only sample ==")
        print(" ".join(f"0x{x:08X}" for x in b_only[:80]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
