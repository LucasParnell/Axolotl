#!/usr/bin/env python3
"""
Disassemble GBAEmu block dumps (ARM/Thumb + x86-64 JIT) and crash fault dumps
with address adjustment and metadata-derived comments.

Supports two dump layouts:
  1. Per-block dumps (dump all): block_*_arm.bin, block_*_thumb.bin, block_*_meta.txt
  2. Combined dump (dump_asm):  combined_arm.bin + combined_arm.map

Usage:
  python scripts/disasm_dump.py <dump_dir> [block_prefix]
  python scripts/disasm_dump.py out/ws/dump                    # all blocks
  python scripts/disasm_dump.py out/ws/dump_asm                # combined dump from dump_asm
  python scripts/disasm_dump.py out/ws/dump --all              # same, explicit
  python scripts/disasm_dump.py out/ws/dump block_00001966     # one block
  python scripts/disasm_dump.py out/ws/crash_dump              # all fault dumps
  python scripts/disasm_dump.py out/ws/dump --output out/dis   # write .dis files

Requires: pip install capstone (or: python -m venv .venv && .venv/bin/pip install -r scripts/requirements.txt)
"""

from __future__ import annotations

import argparse
import re
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


# GBA code address ranges (likely return addresses on stack)
CODE_ROM = (0x08000000, 0x0A000000)
CODE_EWRAM = (0x02000000, 0x02040000)
CODE_IWRAM = (0x03000000, 0x03008000)


def _looks_like_code_addr(addr: int) -> bool:
    """True if address is in a typical code region (ROM or EWRAM; not IWRAM stack)."""
    return (
        CODE_ROM[0] <= addr < CODE_ROM[1]
        or CODE_EWRAM[0] <= addr < CODE_EWRAM[1]
    )


# ─── Meta parsing ───────────────────────────────────────────────────────────

def parse_meta(meta_path: Path) -> dict:
    """Parse block_*_meta.txt. Returns dict with:
    - start, end: int (guest address range)
    - mode: 'ARM' | 'Thumb'
    - bg_refs: str (full BG ref line for header)
    - guest_tags: dict[int, str]  guest_addr -> comment e.g. " [BG3X]"
    - x86_guest: list of (x86_offset, guest_addr) (if [X86_GUEST] present)
    - cpu: dict with r0..r15, cpsr, mode (if [CPU] present)
    - stack: dict with sp, lr, pc, words (if [STACK] present)
    - memory: dict with sp_region_bytes, rows (if [MEMORY] present)
    """
    text = meta_path.read_text()
    lines = text.strip().splitlines()
    out = {
        "start": 0,
        "end": 0,
        "mode": "ARM",
        "bg_refs": "",
        "guest_tags": {},
        "x86_guest": [],
        "cpu": {},
        "stack": {},
        "memory": {},
    }
    i = 0
    # Line 1: # Block 0xSTART - 0xEND Thumb|ARM
    if lines:
        m = re.match(r"# Block (0x[0-9a-fA-F]+) - (0x[0-9a-fA-F]+) (Thumb|ARM)", lines[0])
        if m:
            out["start"] = int(m.group(1), 16)
            out["end"] = int(m.group(2), 16)
            out["mode"] = m.group(3)
    # Line 2: # At dump time: ...
    if len(lines) > 1 and lines[1].startswith("# At dump time:"):
        out["bg_refs"] = lines[1].strip().removeprefix("# ").strip()

    # Parse [CPU], [STACK], [MEMORY] and guest 0xADDR lines
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        i += 1
        if not stripped or stripped.startswith("#"):
            continue
        if stripped == "[CPU]" and i < len(lines):
            kv = re.findall(r"(\w+)=([^\s]+)", lines[i])
            i += 1
            for k, v in kv:
                if k.startswith("r") and len(k) <= 4 and k[1:].isdigit():
                    out["cpu"][k] = int(v, 16)
                elif k == "cpsr":
                    out["cpu"]["cpsr"] = int(v, 16)
                elif k == "mode":
                    out["cpu"]["mode"] = v
            continue
        if stripped == "[STACK]" and i < len(lines):
            first = lines[i]
            i += 1
            for part in re.findall(r"(sp|lr|pc)=0x([0-9a-fA-F]+)", first):
                out["stack"][part[0]] = int(part[1], 16)
            if i < len(lines) and lines[i].strip().startswith("stack="):
                rest = lines[i].strip().removeprefix("stack=").strip()
                i += 1
                out["stack"]["words"] = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", rest)]
            continue
        if stripped == "[MEMORY]" and i < len(lines):
            if "=" in lines[i]:
                m = re.match(r"sp_region_bytes=(\d+)", lines[i])
                if m:
                    out["memory"]["sp_region_bytes"] = int(m.group(1))
                i += 1
            out["memory"]["rows"] = []
            while i < len(lines) and re.match(r"\s*\+0x[0-9a-fA-F]+=", lines[i]):
                out["memory"]["rows"].append(lines[i].strip())
                i += 1
            continue
        if stripped == "[X86_GUEST]":
            while i < len(lines):
                line = lines[i]
                m = re.match(r"^\s*0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s*$", line.strip())
                if not m:
                    break
                out["x86_guest"].append((int(m.group(1), 16), int(m.group(2), 16)))
                i += 1
            continue
        # Guest IR address line: 0xADDR or 0xADDR [BG2X]
        m = re.match(r"^(0x[0-9a-fA-F]+)(.*)$", stripped)
        if m and "=" not in stripped:
            addr = int(m.group(1), 16)
            tag = m.group(2).strip()
            if tag:
                out["guest_tags"][addr] = " " + tag
    return out


def parse_fault_meta(meta_path: Path) -> dict:
    """Parse fault_0x<guestPc>_meta.txt from crash_dump. Returns dict with:
    - start, end: guest_pc (same for fault blocks)
    - mode: 'Thumb' | 'ARM' when ARM/Thumb block was dumped, else 'x86'
    - fault: True
    - rip_offset: int (x86 offset where fault occurred)
    - block_size / x86_block_size: int
    - bg_refs, guest_tags, x86_guest, cpu, stack, memory: empty/default
    """
    text = meta_path.read_text()
    lines = text.strip().splitlines()
    out = {
        "start": 0,
        "end": 0,
        "mode": "x86",
        "fault": True,
        "rip_offset": 0,
        "block_size": 0,
        "bg_refs": "",
        "guest_tags": {},
        "x86_guest": [],
        "cpu": {},
        "stack": {},
        "memory": {},
    }
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#") or not stripped:
            continue
        if stripped.startswith("guest_pc="):
            val = stripped.split("=", 1)[1].strip()
            addr = int(val, 16)
            out["start"] = out["end"] = addr
        elif stripped.startswith("rip_offset="):
            val = stripped.split("=", 1)[1].strip()
            out["rip_offset"] = int(val, 16)
        elif stripped.startswith("block_size=") or stripped.startswith("x86_block_size="):
            out["block_size"] = int(stripped.split("=", 1)[1].strip())
        elif stripped.startswith("arm_block_size="):
            pass  # optional; presence of is_thumb is enough for mode
        elif stripped.startswith("is_thumb="):
            out["mode"] = "Thumb" if stripped.split("=", 1)[1].strip() in ("1", "true", "yes") else "ARM"
    return out


def block_prefix_from_meta_path(meta_path: Path) -> str:
    """e.g. block_00001966_meta.txt -> block_00001966, fault_0x08001234_meta.txt -> fault_0x08001234"""
    return meta_path.stem.removesuffix("_meta")


# ─── Disassembly ────────────────────────────────────────────────────────────

def disasm_arm_thumb(
    code: bytes,
    base_addr: int,
    is_thumb: bool,
    guest_tags: dict[int, str],
) -> list[tuple[int, int, str, str | None]]:
    """Returns list of (addr, size, disasm_line, comment)."""
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if is_thumb else CS_MODE_ARM)
    md.detail = True
    result = []
    for insn in md.disasm(code, base_addr):
        line = f"{insn.mnemonic}\t{insn.op_str}"
        comment = guest_tags.get(insn.address)
        result.append((insn.address, insn.size, line, comment))
    return result


def _guest_for_x86_offset(x86_offset: int, x86_guest: list[tuple[int, int]]) -> int | None:
    """Largest guest address such that (x86_off, guest_addr) has x86_off <= x86_offset."""
    best = None
    for off, guest in x86_guest:
        if off <= x86_offset:
            best = guest
    return best


def disasm_x86(
    code: bytes,
    base_addr: int,
) -> list[tuple[int, int, str]]:
    """Returns list of (addr, size, disasm_line). No per-instruction guest mapping."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    result = []
    for insn in md.disasm(code, base_addr):
        line = f"{insn.mnemonic}\t{insn.op_str}"
        result.append((insn.address, insn.size, line))
    return result


# ─── Formatting ──────────────────────────────────────────────────────────────

def format_context_section(meta: dict, include_memory: bool = True) -> str:
    """Format [CPU], [STACK], and optionally [MEMORY] as comment block for disassembly."""
    lines = []
    cpu = meta.get("cpu") or {}
    stack = meta.get("stack") or {}
    memory = meta.get("memory") or {}
    if not cpu and not stack:
        return ""

    if cpu:
        regs = " ".join(f"r{i}=0x{cpu.get('r' + str(i), 0):x}" for i in range(16))
        cpsr = cpu.get("cpsr", 0)
        mode = cpu.get("mode", "?")
        lines.append(f"; [CPU] {regs} cpsr=0x{cpsr:x} mode={mode}")
    if stack:
        sp = stack.get("sp", 0)
        lr = stack.get("lr", 0)
        pc = stack.get("pc", 0)
        lines.append(f"; [STACK] sp=0x{sp:x} lr=0x{lr:x} pc=0x{pc:x}")
        words = stack.get("words") or []
        ret_addrs = [w for w in words if _looks_like_code_addr(w)]
        if ret_addrs:
            lines.append("; likely return addrs on stack: " + " ".join(f"0x{a:x}" for a in ret_addrs))
    if include_memory and memory.get("rows"):
        lines.append("; [MEMORY] 64 bytes at SP:")
        for row in memory["rows"][:4]:  # first 4 lines (64 bytes)
            lines.append(";   " + row)
    if lines:
        lines.append("")
    return "\n".join(lines)


def format_arm_block(
    meta: dict,
    code: bytes,
    start: int,
    include_context: bool = True,
) -> str:
    is_thumb = meta["mode"] == "Thumb"
    lines = []
    lines.append(f"; Block 0x{meta['start']:x} - 0x{meta['end']:x} ({meta['mode']})")
    if meta["bg_refs"]:
        lines.append(f"; {meta['bg_refs']}")
    if include_context:
        ctx = format_context_section(meta)
        if ctx:
            lines.append(ctx)
    for addr, size, disasm, comment in disasm_arm_thumb(
        code, start, is_thumb, meta["guest_tags"]
    ):
        comment_str = ("  ; " + comment.strip()) if comment else ""
        lines.append(f"  0x{addr:08x}:  {disasm}{comment_str}")
    return "\n".join(lines)


def format_x86_block(
    meta: dict,
    code: bytes,
    base_addr: int,
    include_context: bool = True,
) -> str:
    lines = []
    if meta.get("fault"):
        lines.append("; Crash fault block (guest PC 0x%x)" % meta["start"])
        lines.append("; Fault at x86 offset 0x%x (RIP offset from block start)" % meta.get("rip_offset", 0))
    else:
        lines.append("; x86-64 JIT (GBA block 0x%x - 0x%x %s)" % (
            meta["start"], meta["end"], meta["mode"]))
    if meta.get("bg_refs"):
        lines.append("; %s" % meta["bg_refs"])
    if include_context:
        ctx = format_context_section(meta)
        if ctx:
            lines.append(ctx)
    x86_guest = meta.get("x86_guest") or []
    if x86_guest:
        lines.append("; x86 offset -> guest address mapping from [X86_GUEST].")
    else:
        lines.append("; Guest addresses in meta refer to ARM/Thumb; no per-instruction mapping for x86.")
    if meta.get("fault"):
        lines.append("; (Fault block: no x86->guest mapping in crash dump)")
    lines.append("")
    guest_tags = meta.get("guest_tags") or {}
    fault_rip = meta.get("rip_offset") if meta.get("fault") else None
    for addr, size, disasm in disasm_x86(code, base_addr):
        x86_offset = addr - base_addr
        guest = _guest_for_x86_offset(x86_offset, x86_guest) if x86_guest else None
        comment = ""
        if guest is not None:
            tag = guest_tags.get(guest, "")
            comment = "  ; guest 0x%x%s" % (guest, tag)
        if fault_rip is not None and fault_rip >= x86_offset and fault_rip < x86_offset + size:
            comment = "  ; <-- FAULT (RIP)"
        lines.append("  0x%08x:  %s%s" % (addr, disasm, comment))
    return "\n".join(lines)


# ─── Combined dump (dump_asm) ─────────────────────────────────────────────────

def parse_combined_map(map_path: Path) -> list[tuple[int, int, int, bool]]:
    """Parse combined_arm.map. Returns list of (file_offset, start_addr, size_bytes, is_thumb)."""
    entries = []
    for line in map_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            file_offset = int(parts[0], 16)
            start_addr = int(parts[1], 16)
            size = int(parts[2])
            is_thumb = parts[3].lower() == "thumb"
            entries.append((file_offset, start_addr, size, is_thumb))
        except (ValueError, IndexError):
            continue
    return entries


def run_combined_asm(
    dump_dir: Path,
    out_path: Path | None,
    include_context: bool,
) -> None:
    """Disassemble combined_arm.bin using combined_arm.map (output from dump_asm)."""
    bin_path = dump_dir / "combined_arm.bin"
    map_path = dump_dir / "combined_arm.map"
    if not bin_path.exists():
        print("error: missing combined_arm.bin in", dump_dir, file=sys.stderr)
        sys.exit(1)
    if not map_path.exists():
        print("error: missing combined_arm.map in", dump_dir, file=sys.stderr)
        sys.exit(1)

    blob = bin_path.read_bytes()
    entries = parse_combined_map(map_path)
    if not entries:
        print("error: no entries in combined_arm.map", file=sys.stderr)
        sys.exit(1)

    lines = []
    lines.append("; Combined ARM/Thumb disassembly (dump_asm)")
    lines.append("; Map: file_offset start_address size mode")
    lines.append("")

    for file_offset, start_addr, size, is_thumb in entries:
        end = file_offset + size
        if end > len(blob):
            lines.append(f"; [block 0x{start_addr:08x}] skip: slice {file_offset}+{size} exceeds file size {len(blob)}")
            lines.append("")
            continue
        code = blob[file_offset:end]
        mode = "Thumb" if is_thumb else "ARM"
        lines.append(f"; --- Block 0x{start_addr:08x} - 0x{start_addr + size:08x} ({mode}) ---")
        for addr, insn_size, disasm, comment in disasm_arm_thumb(code, start_addr, is_thumb, {}):
            comment_str = ("  ; " + comment.strip()) if comment else ""
            lines.append(f"  0x{addr:08x}:  {disasm}{comment_str}")
        lines.append("")

    text = "\n".join(lines)
    if out_path:
        out_path.mkdir(parents=True, exist_ok=True)
        dis_path = out_path / "combined_arm.dis"
        dis_path.write_text(text)
        print("Written:", dis_path)
    else:
        print(text)


def find_meta_files(dump_dir: Path) -> list[tuple[Path, bool]]:
    """Return [(meta_path, is_fault), ...] for block_* and fault_* meta files."""
    block_meta = sorted(dump_dir.glob("block_*_meta.txt"))
    fault_meta = sorted(dump_dir.glob("fault_*_meta.txt"))
    return [(p, False) for p in block_meta] + [(p, True) for p in fault_meta]


def run_block(
    dump_dir: Path,
    prefix: str,
    meta: dict,
    x86_base: int,
    out_dir: Path | None,
    include_context: bool = True,
) -> None:
    is_thumb = meta["mode"] == "Thumb"
    arm_suffix = "_thumb.bin" if is_thumb else "_arm.bin"
    arm_path = dump_dir / (prefix + arm_suffix)
    x86_path = dump_dir / (prefix + "_x86.bin")

    out_lines = []

    if arm_path.exists():
        code = arm_path.read_bytes()
        start = meta["start"]
        text = format_arm_block(meta, code, start, include_context)
        out_lines.append("--- ARM/Thumb (guest addresses) ---")
        out_lines.append(text)
        out_lines.append("")
        if out_dir:
            (out_dir / (prefix + arm_suffix.replace(".bin", ".dis"))).write_text(text)
    elif not meta.get("fault"):
        out_lines.append("(no ARM/Thumb file: %s)" % arm_path.name)
        out_lines.append("")

    if x86_path.exists():
        code = x86_path.read_bytes()
        text = format_x86_block(meta, code, x86_base, include_context)
        out_lines.append("--- x86-64 JIT ---")
        out_lines.append(text)
        if out_dir:
            (out_dir / (prefix + "_x86.dis")).write_text(text)
    else:
        out_lines.append("(no x86 file: %s)" % x86_path.name)

    if not out_dir:
        print("\n".join(out_lines))


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Disassemble GBAEmu block dumps with metadata comments."
    )
    ap.add_argument(
        "dump_dir",
        type=Path,
        help="Directory containing block_* or fault_* dumps (_arm.bin/_thumb.bin/_x86.bin, _meta.txt)",
    )
    ap.add_argument(
        "block_prefix",
        nargs="?",
        default=None,
        help="Optional: only process block_XXXXXXXX or fault_0xXXXXXXXX (e.g. block_00001966, fault_0x08001234)",
    )
    ap.add_argument(
        "-a", "--all",
        action="store_true",
        help="Disassemble all blocks in dump_dir (ignores block_prefix if given)",
    )
    ap.add_argument(
        "-o", "--output",
        type=Path,
        default=None,
        help="Write .dis files to this directory instead of stdout",
    )
    ap.add_argument(
        "--x86-base",
        type=lambda s: int(s, 0),
        default=0,
        help="Base address for x86 disassembly (default 0)",
    )
    ap.add_argument(
        "--no-context",
        action="store_true",
        help="Omit CPU/stack/memory context from disassembly output",
    )
    args = ap.parse_args()

    dump_dir = args.dump_dir
    if not dump_dir.is_dir():
        print("error: not a directory:", dump_dir, file=sys.stderr)
        sys.exit(1)

    # Combined dump from dump_asm: combined_arm.bin + combined_arm.map
    if (dump_dir / "combined_arm.bin").exists() and (dump_dir / "combined_arm.map").exists():
        run_combined_asm(
            dump_dir,
            args.output,
            include_context=not args.no_context,
        )
        return

    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)

    meta_entries = find_meta_files(dump_dir)
    if not args.all and args.block_prefix:
        meta_entries = [
            (p, is_fault) for p, is_fault in meta_entries
            if block_prefix_from_meta_path(p) == args.block_prefix
        ]
        if not meta_entries:
            print("error: no block/fault matching prefix:", args.block_prefix, file=sys.stderr)
            sys.exit(1)
    if not meta_entries:
        print("error: no block or fault meta files (block_*_meta.txt, fault_*_meta.txt) in %s" % dump_dir, file=sys.stderr)
        sys.exit(1)

    include_context = not args.no_context
    for meta_path, is_fault in meta_entries:
        prefix = block_prefix_from_meta_path(meta_path)
        meta = parse_fault_meta(meta_path) if is_fault else parse_meta(meta_path)
        run_block(dump_dir, prefix, meta, args.x86_base, args.output, include_context)


if __name__ == "__main__":
    main()
