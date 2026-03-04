#!/usr/bin/env python3
from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path


def hx(v: str) -> int:
    v = v.strip()
    if v.startswith("0x") or v.startswith("0X"):
        return int(v, 16)
    return int(v, 10)


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", newline="") as f:
        return list(csv.DictReader(f))


def unique_hex(values: set[int]) -> str:
    return ", ".join(f"0x{v:08X}" for v in sorted(values))


def main() -> int:
    csv_path = Path("analysis/block_probe/block_probe.csv")
    if not csv_path.exists():
        print(f"missing: {csv_path}")
        return 1

    rows = load_rows(csv_path)
    if not rows:
        print("empty CSV")
        return 1

    # Track queue-link behavior around the known bad region.
    pre_23cc = [r for r in rows if r["target_name"] == "logo_23cc_queue_link" and r["phase"] == "pre"]
    pre_23d8 = [r for r in rows if r["target_name"] == "logo_23d8_queue_store" and r["phase"] == "pre"]
    post_257c = [r for r in rows if r["target_name"] == "logo_257c_link_commit" and r["phase"] == "post"]

    all_zero_r1_23d8 = pre_23d8 and all(hx(r["r1"]) == 0 for r in pre_23d8)
    all_zero_mr4_23cc = pre_23cc and all(hx(r["m_r4_p34"]) == 0 for r in pre_23cc)
    any_nonzero_mr5_257c = any(hx(r["m_r5_p20"]) != 0 for r in post_257c)

    print("=== Block Probe Summary ===")
    print(f"rows: {len(rows)}")
    print(f"23CC(pre) hits: {len(pre_23cc)}")
    print(f"23D8(pre) hits: {len(pre_23d8)}")
    print(f"257C(post) hits: {len(post_257c)}")
    print()
    print(f"23CC [r0+0x34] always zero: {all_zero_mr4_23cc}")
    print(f"23D8 r1 always zero:       {all_zero_r1_23d8}")
    print(f"257C [r5+0x20] ever set:   {any_nonzero_mr5_257c}")
    print()

    # Gather dynamic addresses involved so debugger watchpoints are trivial.
    obj_addrs = {hx(r["r0"]) for r in pre_23cc}
    node_addrs = {hx(r["r3"]) for r in pre_23d8}
    print("Object addresses (r0 @ 23CC):")
    print(unique_hex(obj_addrs) if obj_addrs else "(none)")
    print("Node addresses (r3 @ 23D8):")
    print(unique_hex(node_addrs) if node_addrs else "(none)")
    print()

    print("Suggested breakpoints (Thumb PCs):")
    for pc in (0x22FA, 0x2301, 0x23CC, 0x23D8, 0x256E, 0x257C, 0x2594, 0x0C04):
        print(f"  0x{pc:08X}")
    print()

    print("Suggested watchpoints:")
    for a in sorted(obj_addrs):
        print(f"  [0x{a + 0x34:08X}]  ; object.next (read at 23CC)")
    for a in sorted(node_addrs):
        print(f"  [0x{a + 0x20:08X}]  ; node.link (written at 23D8 / 257C)")
    print()

    # First causality chain: 257C post with non-zero node->20 followed by 23D8 pre r1=0.
    by_event = {hx(r["event"]): r for r in rows}
    events = sorted(by_event.keys())
    chain_found = False
    for e in events:
        r = by_event[e]
        if r["target_name"] != "logo_257c_link_commit" or r["phase"] != "post":
            continue
        if hx(r["m_r5_p20"]) == 0:
            continue
        for e2 in events:
            if e2 <= e:
                continue
            n = by_event[e2]
            if n["target_name"] == "logo_23d8_queue_store" and n["phase"] == "pre":
                print("First non-zero->zero chain:")
                print(
                    f"  event {e}: 257C post m_r5_p20={r['m_r5_p20']} r5={r['r5']} r4={r['r4']}"
                )
                print(
                    f"  event {e2}: 23D8 pre  r1={n['r1']} r3={n['r3']} m_r4_p34={n['m_r4_p34']}"
                )
                chain_found = True
                break
        if chain_found:
            break

    if not chain_found:
        print("No 257C -> 23D8 chain found in this CSV.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
