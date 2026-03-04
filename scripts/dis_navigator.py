#!/usr/bin/env python3
"""Navigate concatenated ARM/Thumb block dumps.

Parses analysis/dis/con/arm_combined.dis and builds:
- per-block metadata
- static edges from IR BRANCH/CALL nodes
- optional dynamic edges from pc_trace.log

Use this to trace likely producers upstream of problematic PCs.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import pathlib
import re
import sys
from typing import Dict, Iterable, List, Optional, Set, Tuple


HEADER_RE = re.compile(
    r"^; === block_([0-9A-Fa-f]{8})_(arm|thumb)\.dis "
    r"\(Block 0x([0-9A-Fa-f]+) - 0x([0-9A-Fa-f]+) (ARM|Thumb)\) ===$"
)
IR_RE = re.compile(
    r"^\s*\d+\s+\|\s+(.+?)\s+Rd:\s+R\d+\s+Rn:\s+R\d+\s+Rm:\s+R\d+\s+\|\s+Imm:\s+0x([0-9A-Fa-f]+)(.*)$"
)
TRACE_RE = re.compile(
    r"^\s*\d+\s+([0-9A-Fa-f]{8})\s+([AT])\s+->\s+([0-9A-Fa-f]{8})\s+([AT])\s+"
)


@dataclasses.dataclass(frozen=True)
class Node:
    pc: int
    mode: str  # "A" or "T"

    def key(self) -> str:
        return f"0x{self.pc:08X}{self.mode}"


@dataclasses.dataclass
class Block:
    node: Node
    start: int
    end: int
    lines: List[str]
    ir_lines: List[str]
    literals: Set[int]


def parse_mode(m: str) -> str:
    m = m.lower()
    if m == "arm":
        return "A"
    if m == "thumb":
        return "T"
    raise ValueError(f"Unsupported mode: {m}")


def norm_target_pc(raw: int, mode: str) -> int:
    return raw & (~1 if mode == "T" else ~3)


def parse_blocks(dis_path: pathlib.Path) -> Tuple[Dict[Node, Block], Dict[Node, Set[Node]]]:
    text = dis_path.read_text(encoding="utf-8", errors="replace").splitlines()

    blocks: Dict[Node, Block] = {}
    static_edges: Dict[Node, Set[Node]] = collections.defaultdict(set)

    current_node: Optional[Node] = None
    current_start = 0
    current_end = 0
    current_lines: List[str] = []
    current_ir: List[str] = []
    current_literals: Set[int] = set()

    def flush_current() -> None:
        nonlocal current_node, current_lines, current_ir, current_literals, current_start, current_end
        if current_node is None:
            return
        blocks[current_node] = Block(
            node=current_node,
            start=current_start,
            end=current_end,
            lines=current_lines[:],
            ir_lines=current_ir[:],
            literals=set(current_literals),
        )
        current_node = None
        current_lines.clear()
        current_ir.clear()
        current_literals.clear()

    for line in text:
        m = HEADER_RE.match(line)
        if m:
            flush_current()
            pc = int(m.group(1), 16)
            mode = parse_mode(m.group(2))
            start = int(m.group(3), 16)
            end = int(m.group(4), 16)
            current_node = Node(pc=pc, mode=mode)
            current_start = start
            current_end = end
            continue

        if current_node is None:
            continue

        current_lines.append(line)
        if line.startswith("=== IR Block Start:") or line.startswith("=== IR Block End"):
            current_ir.append(line)
            continue
        if "| " not in line or "Imm:" not in line:
            continue

        irm = IR_RE.match(line)
        if not irm:
            continue

        op = irm.group(1).strip()
        imm = int(irm.group(2), 16)
        rest = irm.group(3)
        current_ir.append(line)

        # Keep potentially useful absolute constants.
        if imm >= 0x02000000:
            current_literals.add(imm)

        op_upper = op.upper()
        if op_upper.startswith("BRANCH") or op_upper.startswith("CALL"):
            # Infer target mode from low bit.
            target_mode = "T" if (imm & 1) else "A"
            target_pc = norm_target_pc(imm, target_mode)
            static_edges[current_node].add(Node(target_pc, target_mode))
            # Conditional branches can also fall through to block end.
            if "(Cond:" in rest:
                ft_pc = norm_target_pc(current_end, current_node.mode)
                static_edges[current_node].add(Node(ft_pc, current_node.mode))

    flush_current()
    return blocks, static_edges


def parse_trace_edges(trace_path: pathlib.Path) -> Dict[Node, Set[Node]]:
    trace_edges: Dict[Node, Set[Node]] = collections.defaultdict(set)
    with trace_path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TRACE_RE.match(line)
            if not m:
                continue
            src = Node(int(m.group(1), 16), m.group(2))
            dst = Node(int(m.group(3), 16), m.group(4))
            trace_edges[src].add(dst)
    return trace_edges


def reverse_edges(edges: Dict[Node, Set[Node]]) -> Dict[Node, Set[Node]]:
    rev: Dict[Node, Set[Node]] = collections.defaultdict(set)
    for src, dsts in edges.items():
        for dst in dsts:
            rev[dst].add(src)
    return rev


def backward_layers(rev: Dict[Node, Set[Node]], target: Node, depth: int) -> List[Set[Node]]:
    layers: List[Set[Node]] = []
    seen: Set[Node] = {target}
    frontier: Set[Node] = {target}
    for _ in range(depth):
        parents: Set[Node] = set()
        for n in frontier:
            parents.update(rev.get(n, set()))
        parents -= seen
        if not parents:
            break
        layers.append(parents)
        seen.update(parents)
        frontier = parents
    return layers


def parse_target(raw: str) -> Node:
    s = raw.strip().lower()
    mode = "A"
    if s.endswith("a"):
        mode = "A"
        s = s[:-1]
    elif s.endswith("t"):
        mode = "T"
        s = s[:-1]
    if s.startswith("0x"):
        s = s[2:]
    pc = int(s, 16)
    pc = norm_target_pc(pc, mode)
    return Node(pc, mode)


def interesting_literals(block: Block) -> List[str]:
    out: List[str] = []
    for v in sorted(block.literals):
        if v in {
            0x03003B2C,
            0x030036EC,
            0x0300372C,
            0x0300390C,
            0x0300394C,
            0x06000000,
            0x07000000,
            0x05000000,
        }:
            out.append(f"0x{v:08X}")
    return out


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="Navigate GBA BIOS block disassembly")
    ap.add_argument("--dis", default="analysis/dis/con/arm_combined.dis", help="Path to arm_combined.dis")
    ap.add_argument("--trace", default="out/pc_trace.log", help="Optional pc_trace.log path")
    ap.add_argument(
        "--targets",
        nargs="+",
        default=["0x00000BD8A", "0x00000BE4A", "0x00000C04A"],
        help="Target nodes, e.g. 0xC04A or 0x1664T",
    )
    ap.add_argument("--depth", type=int, default=4, help="Backward search depth")
    args = ap.parse_args(argv)

    dis_path = pathlib.Path(args.dis)
    if not dis_path.exists():
        print(f"error: disassembly file not found: {dis_path}", file=sys.stderr)
        return 2

    blocks, static_edges = parse_blocks(dis_path)
    static_rev = reverse_edges(static_edges)

    trace_edges: Dict[Node, Set[Node]] = {}
    trace_path = pathlib.Path(args.trace)
    if trace_path.exists():
        trace_edges = parse_trace_edges(trace_path)
    trace_rev = reverse_edges(trace_edges)

    print(f"Loaded {len(blocks)} blocks from {dis_path}")
    print(f"Static edges: {sum(len(v) for v in static_edges.values())}")
    if trace_edges:
        print(f"Dynamic edges from trace: {sum(len(v) for v in trace_edges.values())}")
    else:
        print("Dynamic edges from trace: none (trace file missing or empty)")
    print()

    targets = [parse_target(t) for t in args.targets]
    for t in targets:
        print(f"== Target {t.key()} ==")
        b = blocks.get(t)
        if b:
            print(f"  Block range: 0x{b.start:X}-0x{b.end:X}")
            lits = interesting_literals(b)
            if lits:
                print(f"  Interesting literals: {', '.join(lits)}")
        else:
            print("  Block not found in disassembly.")

        static_parents = static_rev.get(t, set())
        trace_parents = trace_rev.get(t, set())
        if static_parents:
            print("  Static parents:", ", ".join(n.key() for n in sorted(static_parents, key=lambda n: (n.pc, n.mode))))
        if trace_parents:
            print("  Trace parents :", ", ".join(n.key() for n in sorted(trace_parents, key=lambda n: (n.pc, n.mode))))

        layers = backward_layers(static_rev, t, args.depth)
        for i, layer in enumerate(layers, start=1):
            print(f"  Upstream layer {i}:")
            for n in sorted(layer, key=lambda x: (x.pc, x.mode)):
                nb = blocks.get(n)
                lit = ""
                if nb:
                    ints = interesting_literals(nb)
                    if ints:
                        lit = f"  literals={','.join(ints)}"
                print(f"    - {n.key()}{lit}")
        print()

    # Suggest breakpoints: direct trace parents, then static parents.
    print("== Suggested Breakpoints ==")
    suggested: List[Node] = []
    for t in targets:
        for n in sorted(trace_rev.get(t, set()), key=lambda x: (x.pc, x.mode)):
            if n not in suggested:
                suggested.append(n)
        for n in sorted(static_rev.get(t, set()), key=lambda x: (x.pc, x.mode)):
            if n not in suggested:
                suggested.append(n)

    if not suggested:
        print("  (none)")
    else:
        for n in suggested:
            print(f"  b 0x{n.pc:08X} {n.mode}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
