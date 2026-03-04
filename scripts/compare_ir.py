#!/usr/bin/env python3
"""
compare_ir.py  —  automated pass/fail checker for the ARM → IR stage.

Workflow:
    python3 scripts/fuzzer_v2.py --count 200 --seed 42
    cmake --build build
    python3 scripts/compare_ir.py --ir-dump ./build/out/ir_dump

For each line in fuzzer.txt this script:
  1. Calls ir_dump <hex> to get the IR text for that instruction.
  2. Parses the first non-sentinel IR node's opcode from the output.
  3. Compares it against the expected op(s) from column 3 of fuzzer.txt.
  4. Reports PASS / FAIL / SKIP per instruction and a final summary.

Exit code: 0 if all non-skipped tests pass, 1 otherwise.

ir_dump binary contract:
  Takes one argument: the hex encoding WITHOUT "0x" prefix.
  Writes IrPrinter::PrintArena output to stdout, one node per line in the form:
      <idx> | <OPNAME>  Rd: ...  Rn: ...  ...
  The last node is always the B-self sentinel; ignore it.
  
  A minimal ir_dump main:
  
    int main(int argc, char** argv) {
        if (argc < 2) return 1;
        uint32_t instr = std::stoul(argv[1], nullptr, 16);
        MemoryBus bus;
        ArenaAllocator arena(4096);
        IrBuilder builder(&bus, &arena);
        bus.Write32(nullptr, 0x02000000, instr);
        bus.Write32(nullptr, 0x02000004, 0xEAFFFFFE);  // B . sentinel
        builder.BuildBlock<false>(0x02000000);
        std::cout << IrPrinter::PrintArena(&arena, 0x02000000);
        return 0;
    }
"""

import argparse
import os
import subprocess
import sys
import re
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class TestResult:
    idx: int
    hex_str: str
    asm: str
    expected: str
    actual_ops: List[str] = field(default_factory=list)
    passed: bool = False
    skipped: bool = False
    skip_reason: str = ""
    error: str = ""


def run_ir_dump(ir_dump_path: str, hex_str: str, timeout: float = 2.0):
    """Run ir_dump and return stdout. Raises on timeout."""
    hex_clean = hex_str.removeprefix("0x")
    result = subprocess.run(
        [ir_dump_path, hex_clean],
        capture_output=True, text=True, timeout=timeout,
    )
    return result.stdout, result.stderr


def parse_opcodes_from_ir(ir_text: str) -> List[str]:
    """
    Extract the opcode names from IrPrinter output lines.
    Expected format per line:
        0 | ADD         Rd: R0  Rn: R1  Rm: R2  ...
        1 | BRANCH      imm=0x08000000  (sentinel)
    Returns uppercase opcode strings, excluding the final BRANCH sentinel.
    """
    opcodes = []
    lines = [ln.strip() for ln in ir_text.splitlines() if ln.strip()]
    for line in lines:
        # Match: <number> | <OPNAME>
        m = re.match(r'^\d+\s*\|\s*(\w+)', line)
        if m:
            opcodes.append(m.group(1).upper())

    # Drop the last node if it's the BRANCH sentinel
    if opcodes and opcodes[-1] == "BRANCH":
        opcodes = opcodes[:-1]
    return opcodes


# Maps IrPrinter opcode names → the IrOp enum names used in fuzzer output
PRINTER_TO_IROP = {
    "ADD":          "kAdd",
    "SUB":          "kSub",
    "ADC":          "kAdc",
    "SBC":          "kSbc",
    "RSB":          "kRsb",
    "RSC":          "kRsc",
    "AND":          "kAnd",
    "ORR":          "kOrr",
    "EOR":          "kEor",
    "BIC":          "kBic",
    "MOV":          "kMov",
    "MVN":          "kMvn",
    "CMP":          "kCmp",
    "CMN":          "kCmn",
    "TST":          "kTst",
    "TEQ":          "kTeq",
    "MUL":          "kMul",
    "UMULL":        "kUmull",
    "UMLAL":        "kUmlal",
    "SMULL":        "kSmull",
    "SMLAL":        "kSmlal",
    "LOAD32":       "kLoad32",
    "LOAD16":       "kLoad16",
    "LOAD8":        "kLoad8",
    "LOADSIGNED16": "kLoadSigned16",
    "LOADSIGNED8":  "kLoadSigned8",
    "STORE32":      "kStore32",
    "STORE16":      "kStore16",
    "STORE8":       "kStore8",
    "LOADFAST32":   "kLoadFast32",
    "LOADFAST16":   "kLoadFast16",
    "LOADFAST8":    "kLoadFast8",
    "STOREFAST32":  "kStoreFast32",
    "STOREFAST16":  "kStoreFast16",
    "STOREFAST8":   "kStoreFast8",
    "LOADLITERAL":  "kLoadLiteral",
    "BRANCH":       "kBranch",
    "CALL":         "kCall",
    "BRANCHEXCHANGE":"kBranchExchange",
    "MRS":          "kMrs",
    "MSR":          "kMsr",
    "SWI":          "kSwi",
    "OTHER":        "kOther",
}


def check_result(actual_printer_ops: List[str], expected_str: str) -> bool:
    """
    expected_str is comma-separated IrOp names, e.g. "kAdd" or "kMul,kAdd".
    For LDM/STM the expected is "LDMfirst"/"STMfirst" meaning just check node[0].
    actual_printer_ops is a list of printer opcode strings (uppercase, no 'k' prefix).
    """
    if not expected_str:
        return True  # no expectation = skip

    # Special LDM/STM case: just check first node
    if expected_str in ("LDMfirst", "STMfirst"):
        if not actual_printer_ops:
            return False
        expected_irop = "kLoad32" if expected_str == "LDMfirst" else "kStore32"
        return PRINTER_TO_IROP.get(actual_printer_ops[0]) == expected_irop

    expected_ops = [e.strip() for e in expected_str.split(",")]
    actual_irops = [PRINTER_TO_IROP.get(op, "kUnknown") for op in actual_printer_ops]

    # Check that expected ops appear as a prefix of actual ops
    if len(actual_irops) < len(expected_ops):
        return False
    return actual_irops[:len(expected_ops)] == expected_ops


def main():
    parser = argparse.ArgumentParser(description="Automated ARM → IR correctness checker")
    parser.add_argument("--input",    default="analysis/fuzzer_out/fuzzer.txt")
    parser.add_argument("--output",   default="analysis/fuzzer_out/compare_results.txt")
    parser.add_argument("--ir-dump",  default=None, help="Path to ir_dump binary")
    parser.add_argument("--fail-only",action="store_true", help="Only print failures")
    parser.add_argument("--stop-on-fail", action="store_true")
    parser.add_argument("--timeout",  type=float, default=2.0)
    args = parser.parse_args()

    # Locate ir_dump
    ir_dump = args.ir_dump
    if not ir_dump:
        for cand in ["out/ir_dump", "build/out/ir_dump", "build/ir_dump"]:
            if os.path.isfile(cand):
                ir_dump = cand
                break
    if not ir_dump or not os.path.isfile(ir_dump):
        print("error: ir_dump binary not found. Build it or pass --ir-dump /path",
              file=sys.stderr)
        return 1

    # Load input
    try:
        with open(args.input) as f:
            lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    except FileNotFoundError:
        print(f"error: {args.input} not found. Run fuzzer.py first.", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    results: List[TestResult] = []
    passes = 0; fails = 0; skips = 0

    for idx, line in enumerate(lines, 1):
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        hex_str  = parts[0].strip()
        asm      = parts[1].strip()
        expected = parts[2].strip() if len(parts) >= 3 else ""

        res = TestResult(idx=idx, hex_str=hex_str, asm=asm, expected=expected)

        if not expected:
            res.skipped = True
            res.skip_reason = "no expected op in fuzzer output"
            skips += 1
            results.append(res)
            continue

        try:
            stdout, stderr = run_ir_dump(ir_dump, hex_str, args.timeout)
        except subprocess.TimeoutExpired:
            res.error = "ir_dump timed out"
            res.passed = False
            fails += 1
            results.append(res)
            if args.stop_on_fail:
                break
            continue
        except Exception as e:
            res.error = str(e)
            res.passed = False
            fails += 1
            results.append(res)
            if args.stop_on_fail:
                break
            continue

        actual_ops = parse_opcodes_from_ir(stdout)
        res.actual_ops = actual_ops

        # kOther means the lifter didn't recognise the instruction; always a fail
        if actual_ops and PRINTER_TO_IROP.get(actual_ops[0]) == "kOther":
            res.passed = False
            res.error = "lifter produced kOther (unrecognised instruction)"
            fails += 1
        elif check_result(actual_ops, expected):
            res.passed = True
            passes += 1
        else:
            res.passed = False
            fails += 1

        results.append(res)
        if not res.passed and args.stop_on_fail:
            break

    # Write report
    with open(args.output, "w") as out:
        out.write(f"Results: {passes} passed, {fails} failed, {skips} skipped "
                  f"/ {len(results)} total\n")
        out.write("=" * 72 + "\n\n")
        for res in results:
            if args.fail_only and (res.passed or res.skipped):
                continue
            status = "PASS" if res.passed else ("SKIP" if res.skipped else "FAIL")
            out.write(f"[{status}] #{res.idx:4d}  {res.hex_str}  {res.asm}\n")
            if not res.passed and not res.skipped:
                out.write(f"         Expected : {res.expected}\n")
                actual_str = ",".join(
                    PRINTER_TO_IROP.get(op, "?"+op) for op in res.actual_ops
                ) or "(no nodes)"
                out.write(f"         Actual   : {actual_str}\n")
                if res.error:
                    out.write(f"         Error    : {res.error}\n")
            out.write("\n")

    # Always print summary to stdout
    print(f"\nResults: {passes} passed  {fails} failed  {skips} skipped  "
          f"({len(results)} total)")
    print(f"Full report: {args.output}")

    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
