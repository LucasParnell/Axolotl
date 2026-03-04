#!/usr/bin/env python3
"""
ARMv4T instruction fuzzer v2.

Changes from v1:
  - Outputs a third column: the expected top-level IrOp(s) your lifter should produce.
  - The compare_ir.py script uses this to do automated pass/fail checking, so you
    no longer need to eyeball the dump file to spot regressions.
  - Uses a fixed seed by default (--seed) so CI runs are deterministic.
  - --count controls how many instructions to generate.

Output format (tab-separated):
    0xe0800001\tADD r0, r0, r1\tkAdd
    0xe0200291\tMUL r0, r1, r2\tkMul,kAdd       (MLA → two nodes)

Run:
    python3 scripts/fuzzer.py --count 200 --seed 42
"""

import argparse
import os
import random
import sys

# Maps (template_key, sub_variant) → expected IrOp(s) the lifter should emit.
# The list is the ordered sequence of opcodes expected for nodes[0], nodes[1], ...
# (excluding the terminal branch sentinel).
EXPECTED_OPS = {
    "dp":       ["kAdd", "kSub", "kRsb", "kAdc", "kSbc", "kRsc", "kAnd", "kEor", "kOrr", "kBic"],
    "mov":      ["kMov", "kMvn"],
    "cmp":      ["kCmp", "kCmn", "kTst", "kTeq"],
    "mul":      ["kMul"],                   # MUL only
    "mla":      ["kMul", "kAdd"],           # MLA → 2 nodes
    "mull":     ["kUmull", "kUmlal", "kSmull", "kSmlal"],
    "ldr":      ["kLoad32"],
    "ldrb":     ["kLoad8"],
    "str":      ["kStore32"],
    "strb":     ["kStore8"],
    "ldrh":     ["kLoad16"],
    "strh":     ["kStore16"],
    "ldrsh":    ["kLoadSigned16"],
    "ldrsb":    ["kLoadSigned8"],
    "ldmia":    None,   # count depends on reglist; just check first node = kLoad32
    "stmia":    None,   # first node = kStore32
    "swp":      ["kLoad32", "kStore32"],
    "swpb":     ["kLoad8", "kStore8"],
    "b":        ["kBranch"],
    "bl":       ["kCall"],
    "bx":       ["kBranchExchange"],
    "swi":      ["kSwi"],
    "mrs":      ["kMrs"],
    "msr":      ["kMsr"],
}

ARMv4T_ENTRIES = [
    # (template_string, op_key, sub_op_idx_or_None)
    # Data processing
    ("{dp}{cond} r{rd}, r{rn}, r{rm}",                  "dp",   None),
    ("{dp}{cond}S r{rd}, r{rn}, r{rm}",                 "dp",   None),
    ("{dp}{cond} r{rd}, r{rn}, #{imm8}",                "dp",   None),
    ("{dp}{cond}S r{rd}, r{rn}, #{imm8}",               "dp",   None),
    ("{dp}{cond} r{rd}, r{rn}, r{rm}, {shift} #{shift_imm}", "dp", None),
    ("{dp}{cond} r{rd}, r{rn}, r{rm}, {shift} r{rs}",   "dp",   None),
    # MOV/MVN
    ("{mov}{cond} r{rd}, r{rm}",                         "mov",  None),
    ("{mov}{cond}S r{rd}, r{rm}",                        "mov",  None),
    ("{mov}{cond} r{rd}, #{imm8}",                       "mov",  None),
    ("{mov}{cond} r{rd}, r{rm}, {shift} #{shift_imm}",   "mov",  None),
    # Compare/Test
    ("{cmpop}{cond} r{rn}, r{rm}",                       "cmp",  None),
    ("{cmpop}{cond} r{rn}, #{imm8}",                     "cmp",  None),
    ("{cmpop}{cond} r{rn}, r{rm}, {shift} #{shift_imm}", "cmp",  None),
    # Multiply
    ("MUL{cond} r{rd}, r{rm}, r{rs}",                   "mul",  None),
    ("MUL{cond}S r{rd}, r{rm}, r{rs}",                  "mul",  None),
    ("MLA{cond} r{rd}, r{rm}, r{rs}, r{rn}",            "mla",  None),
    ("MLA{cond}S r{rd}, r{rm}, r{rs}, r{rn}",           "mla",  None),
    ("{mull}{cond} r{rdlo}, r{rdhi}, r{rm}, r{rs}",      "mull", None),
    ("{mull}{cond}S r{rdlo}, r{rdhi}, r{rm}, r{rs}",     "mull", None),
    # LDR/STR word
    ("LDR{cond} r{rd}, [r{rn}, #{imm12}]",              "ldr",  None),
    ("LDR{cond} r{rd}, [r{rn}, #-{imm12}]",             "ldr",  None),
    ("LDR{cond} r{rd}, [r{rn}, r{rm}]",                 "ldr",  None),
    ("LDR{cond} r{rd}, [r{rn}], #{imm12}",              "ldr",  None),
    ("LDR{cond} r{rd}, [r{rn}, #{imm12}]!",             "ldr",  None),
    ("STR{cond} r{rd}, [r{rn}, #{imm12}]",              "str",  None),
    ("STR{cond} r{rd}, [r{rn}, r{rm}]",                 "str",  None),
    # LDRB/STRB
    ("LDRB{cond} r{rd}, [r{rn}, #{imm12}]",             "ldrb", None),
    ("STRB{cond} r{rd}, [r{rn}, #{imm12}]",             "strb", None),
    # Halfword
    ("LDRH{cond} r{rd}, [r{rn}, #{imm8_hw}]",           "ldrh", None),
    ("LDRH{cond} r{rd}, [r{rn}, r{rm}]",                "ldrh", None),
    ("STRH{cond} r{rd}, [r{rn}, #{imm8_hw}]",           "strh", None),
    ("LDRSH{cond} r{rd}, [r{rn}, #{imm8_hw}]",          "ldrsh",None),
    ("LDRSB{cond} r{rd}, [r{rn}, #{imm8_hw}]",          "ldrsb",None),
    # Block transfer
    ("LDMIA{cond} r{rn}!, {{{reglist}}}",               "ldmia",None),
    ("STMIA{cond} r{rn}!, {{{reglist}}}",               "stmia",None),
    # Swap
    ("SWP{cond} r{rd}, r{rm}, [r{rn}]",                 "swp",  None),
    ("SWPB{cond} r{rd}, r{rm}, [r{rn}]",                "swpb", None),
    # Branch
    ("B{cond} #{branch_imm}",                            "b",    None),
    ("BL{cond} #{branch_imm}",                           "bl",   None),
    ("BX{cond} r{rm}",                                   "bx",   None),
    # System
    ("SWI{cond} #{imm24}",                               "swi",  None),
    ("MRS{cond} r{rd}, CPSR",                            "mrs",  None),
    ("MRS{cond} r{rd}, SPSR",                            "mrs",  None),
    ("MSR{cond} CPSR_f, #{imm_msr}",                    "msr",  None),
    ("MSR{cond} CPSR_f, r{rm}",                          "msr",  None),
]

# Choices that map op_key → the actual assembly mnemonic used in a particular call
DP_OPS    = ["ADD", "SUB", "RSB", "ADC", "SBC", "RSC", "AND", "EOR", "ORR", "BIC"]
MOV_OPS   = ["MOV", "MVN"]
CMP_OPS   = ["CMP", "CMN", "TST", "TEQ"]
MULL_OPS  = ["UMULL", "UMLAL", "SMULL", "SMLAL"]

def op_key_to_expected(op_key, chosen_dp=None, chosen_mov=None,
                        chosen_cmp=None, chosen_mull=None):
    """Return a comma-joined string of expected IrOps."""
    if op_key == "dp":
        op = chosen_dp or "ADD"
        map_ = dict(zip(DP_OPS, [
            "kAdd","kSub","kRsb","kAdc","kSbc","kRsc","kAnd","kEor","kOrr","kBic"
        ]))
        return map_.get(op, "kAdd")
    if op_key == "mov":
        op = chosen_mov or "MOV"
        return "kMov" if op == "MOV" else "kMvn"
    if op_key == "cmp":
        op = chosen_cmp or "CMP"
        map_ = {"CMP":"kCmp","CMN":"kCmn","TST":"kTst","TEQ":"kTeq"}
        return map_.get(op, "kCmp")
    if op_key == "mul":  return "kMul"
    if op_key == "mla":  return "kMul,kAdd"
    if op_key == "mull":
        op = chosen_mull or "UMULL"
        map_ = {"UMULL":"kUmull","UMLAL":"kUmlal","SMULL":"kSmull","SMLAL":"kSmlal"}
        return map_.get(op, "kUmull")
    if op_key == "ldr":  return "kLoad32"
    if op_key == "ldrb": return "kLoad8"
    if op_key == "str":  return "kStore32"
    if op_key == "strb": return "kStore8"
    if op_key == "ldrh": return "kLoad16"
    if op_key == "strh": return "kStore16"
    if op_key == "ldrsh":return "kLoadSigned16"
    if op_key == "ldrsb":return "kLoadSigned8"
    if op_key in ("ldmia","stmia"): return op_key.replace("ia","").upper() + "first"
    if op_key == "swp":  return "kLoad32,kStore32"
    if op_key == "swpb": return "kLoad8,kStore8"
    if op_key == "b":    return "kBranch"
    if op_key == "bl":   return "kCall"
    if op_key == "bx":   return "kBranchExchange"
    if op_key == "swi":  return "kSwi"
    if op_key == "mrs":  return "kMrs"
    if op_key == "msr":  return "kMsr"
    return "kOther"


def main():
    parser = argparse.ArgumentParser(description="ARMv4T fuzzer with expected IrOp output")
    parser.add_argument("--count",  type=int,  default=200,  help="Number of instructions to generate")
    parser.add_argument("--seed",   type=int,  default=42,   help="Random seed (0 = random)")
    parser.add_argument("--output", default="analysis/fuzzer_out/fuzzer.txt")
    parser.add_argument("--attempts", type=int, default=100000)
    args = parser.parse_args()

    try:
        from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KsError
    except ImportError:
        print("keystone-engine required: pip install keystone-engine", file=sys.stderr)
        return 1

    if args.seed:
        random.seed(args.seed)

    ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)

    written = 0
    with open(args.output, "w") as f:
        for _ in range(args.attempts):
            if written >= args.count:
                break

            template_entry = random.choice(ARMv4T_ENTRIES)
            template, op_key, _ = template_entry

            rn       = str(random.randint(0, 12))
            rd       = str(random.randint(0, 12))
            rm       = str(random.randint(0, 12))
            rs       = str(random.randint(0, 12))
            rdlo     = str(random.randint(0, 5))
            rdhi     = str(random.randint(6, 12))

            pool = set(range(13))
            if int(rn) in pool:
                pool.remove(int(rn))
            regs    = random.sample(sorted(pool), random.randint(2, 4))
            reglist = ", ".join("r" + str(x) for x in regs)

            cond       = random.choice(["", "EQ", "NE", "CS", "CC", "MI", "PL",
                                         "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"])
            chosen_dp   = random.choice(DP_OPS)
            chosen_mov  = random.choice(MOV_OPS)
            chosen_cmp  = random.choice(CMP_OPS)
            chosen_mull = random.choice(MULL_OPS)
            shift       = random.choice(["LSL", "LSR", "ASR", "ROR"])

            imm8        = str(random.choice([0, 1, 4, 128, 255]))
            shift_imm   = str(random.choice([1, 4, 15, 31]))
            imm12       = str(random.choice([0, 4, 1024, 4095]))
            imm8_hw     = str(random.choice([0, 4, 128, 255]))
            branch_imm  = str(random.choice([0, 4, 8, -4, -8]))
            imm24       = str(random.choice([0, 255, 0xFFFF]))
            imm_msr     = str(random.choice([0, 1, 255]))

            try:
                asm = template.format(
                    rd=rd, rn=rn, rm=rm, rs=rs, rdlo=rdlo, rdhi=rdhi,
                    reglist=reglist, cond=cond,
                    dp=chosen_dp, mov=chosen_mov, cmpop=chosen_cmp, mull=chosen_mull,
                    shift=shift,
                    imm8=imm8, shift_imm=shift_imm, imm12=imm12, imm8_hw=imm8_hw,
                    branch_imm=branch_imm, imm24=imm24, imm_msr=imm_msr,
                )
            except KeyError:
                continue

            try:
                encoding, k_count = ks.asm(asm)
            except Exception:
                continue

            if not encoding or k_count == 0:
                continue

            instr_val = sum(int(b) << (i * 8) for i, b in enumerate(encoding))
            expected = op_key_to_expected(op_key, chosen_dp, chosen_mov,
                                           chosen_cmp, chosen_mull)
            f.write(f"0x{instr_val:08x}\t{asm}\t{expected}\n")
            written += 1

    print(f"Wrote {args.output} ({written} instructions, seed={args.seed})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
