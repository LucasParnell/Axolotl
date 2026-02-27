#!/usr/bin/env python3
"""
ARMv4T instruction fuzzer: generates random but valid ARM7TDMI instructions, 
assembles them with Keystone, and outputs encoding + assembly for manual inspection.
"""

import random
import os
import sys

# Comprehensive ARMv4T templates
ARMv4T_TEMPLATES = [
    # Data Processing
    "{dp}{cond} r{rd}, r{rn}, r{rm}",
    "{dp}{cond}S r{rd}, r{rn}, r{rm}",
    "{dp}{cond} r{rd}, r{rn}, #{imm8}",
    "{dp}{cond}S r{rd}, r{rn}, #{imm8}",
    "{dp}{cond} r{rd}, r{rn}, r{rm}, {shift} #{shift_imm}",
    "{dp}{cond} r{rd}, r{rn}, r{rm}, {shift} r{rs}",

    # Move/MVN
    "{mov}{cond} r{rd}, r{rm}",
    "{mov}{cond}S r{rd}, r{rm}",
    "{mov}{cond} r{rd}, #{imm8}",
    "{mov}{cond} r{rd}, r{rm}, {shift} #{shift_imm}",

    # Test/Compare
    "{cmp}{cond} r{rn}, r{rm}",
    "{cmp}{cond} r{rn}, #{imm8}",
    "{cmp}{cond} r{rn}, r{rm}, {shift} #{shift_imm}",

    # Multiply
    "MUL{cond} r{rd}, r{rm}, r{rs}",
    "MUL{cond}S r{rd}, r{rm}, r{rs}",
    "MLA{cond} r{rd}, r{rm}, r{rs}, r{rn}",
    "MLA{cond}S r{rd}, r{rm}, r{rs}, r{rn}",
    "{mull}{cond} r{rdlo}, r{rdhi}, r{rm}, r{rs}",
    "{mull}{cond}S r{rdlo}, r{rdhi}, r{rm}, r{rs}",

    # Single Data Transfer (Word/Byte)
    "{ldr}{cond} r{rd}, [r{rn}, #{imm12}]",
    "{ldr}{cond} r{rd}, [r{rn}, #-{imm12}]",
    "{ldr}{cond} r{rd}, [r{rn}, r{rm}]",
    "{ldr}{cond} r{rd}, [r{rn}, -r{rm}]",
    "{ldr}{cond} r{rd}, [r{rn}, r{rm}, {shift} #{shift_imm}]",
    "{ldr}{cond} r{rd}, [r{rn}], #{imm12}",
    "{ldr}{cond} r{rd}, [r{rn}, #{imm12}]!",

    # Halfword / Signed Data Transfer
    "{ldrh}{cond} r{rd}, [r{rn}, #{imm8_hw}]",
    "{ldrh}{cond} r{rd}, [r{rn}, #-{imm8_hw}]",
    "{ldrh}{cond} r{rd}, [r{rn}, r{rm}]",
    "{ldrh}{cond} r{rd}, [r{rn}], #{imm8_hw}",
    "{ldrh}{cond} r{rd}, [r{rn}, #{imm8_hw}]!",

    # Block Transfer
    "{ldm}{cond}{ldm_mode} r{rn}!, {{{reglist}}}",
    "{ldm}{cond}{ldm_mode} r{rn}, {{{reglist}}}",

    # Swap
    "{swp}{cond} r{rd}, r{rm}, [r{rn}]",

    # Branch
    "B{cond} #{branch_imm}",
    "BL{cond} #{branch_imm}",
    "BX{cond} r{rm}",

    # System / Status
    "SWI{cond} #{imm24}",
    "MRS{cond} r{rd}, CPSR",
    "MRS{cond} r{rd}, SPSR",
    "MSR{cond} CPSR_f, #{imm_msr}",
    "MSR{cond} CPSR_f, r{rm}"
]

def main():
    try:
        from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KsError
    except ImportError:
        print("keystone-engine required: pip install keystone-engine", file=sys.stderr)
        return 1

    ks = Ks(KS_ARCH_ARM, KS_MODE_ARM)
    os.makedirs("analysis/fuzzer_out", exist_ok=True)
    out_path = "analysis/fuzzer_out/fuzzer.txt"
    
    target_count = 100 # Increased to generate a fat file of tests
    max_attempts = 50000

    with open(out_path, "w") as f:
        count = 0
        for _ in range(max_attempts):
            if count >= target_count:
                break
                
            template = random.choice(ARMv4T_TEMPLATES)
            
            # 1. Base Registers (restricted to 0-12 to avoid unpredictable SP/PC behavior)
            rn = str(random.randint(0, 12))
            rd = str(random.randint(0, 12))
            rm = str(random.randint(0, 12))
            rs = str(random.randint(0, 12))
            
            # Multiply Long requires RdHi and RdLo to be distinct
            rdlo = str(random.randint(0, 5))
            rdhi = str(random.randint(6, 12))
            
            # 2. Block transfer register list (Ensure base register 'rn' isn't in the list)
            pool = set(range(13))
            if int(rn) in pool: 
                pool.remove(int(rn))
            regs = random.sample(list(pool), random.randint(2, 4))
            reglist = ", ".join("r" + str(x) for x in sorted(regs))

            # 3. Instruction attributes
            cond = random.choice(["", "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"])
            dp = random.choice(["ADD", "SUB", "RSB", "ADC", "SBC", "RSC", "AND", "EOR", "ORR", "BIC"])
            mov = random.choice(["MOV", "MVN"])
            cmp_op = random.choice(["CMP", "CMN", "TST", "TEQ"])
            mull = random.choice(["UMULL", "UMLAL", "SMULL", "SMLAL"])
            ldr = random.choice(["LDR", "STR", "LDRB", "STRB"])
            ldrh = random.choice(["LDRH", "STRH", "LDRSH", "LDRSB"])
            ldm = random.choice(["LDM", "STM"])
            ldm_mode = random.choice(["IA", "IB", "DA", "DB"])
            swp = random.choice(["SWP", "SWPB"])
            shift = random.choice(["LSL", "LSR", "ASR", "ROR"])
            
            # 4. Immediates
            imm8 = str(random.choice([0, 1, 4, 128, 255]))
            shift_imm = str(random.choice([1, 4, 15, 31]))
            imm12 = str(random.choice([0, 4, 1024, 4095]))
            imm8_hw = str(random.choice([0, 4, 128, 255]))
            branch_imm = str(random.choice([0, 4, 8, -4, -8]))
            imm24 = str(random.choice([0, 255, 0xFFFF]))
            imm_msr = str(random.choice([0, 1, 255]))

            # Format the template into final ARM Assembly
            try:
                asm = template.format(
                    rd=rd, rn=rn, rm=rm, rs=rs, rdlo=rdlo, rdhi=rdhi, reglist=reglist,
                    cond=cond, dp=dp, mov=mov, cmp=cmp_op, mull=mull, ldr=ldr, ldrh=ldrh,
                    ldm=ldm, ldm_mode=ldm_mode, swp=swp, shift=shift,
                    imm8=imm8, shift_imm=shift_imm, imm12=imm12, imm8_hw=imm8_hw,
                    branch_imm=branch_imm, imm24=imm24, imm_msr=imm_msr
                )
            except KeyError:
                continue

            # Assemble it via Keystone to ensure it's mathematically encodable
            try:
                encoding, k_count = ks.asm(asm)
            except KsError:
                # If Keystone rejects it (e.g. an impossible immediate rotation), skip it
                continue
                
            if not encoding or k_count == 0:
                continue

            # Output the Hex and the Assembly natively (IrOp removed)
            instr_val = sum(int(b) << (i * 8) for i, b in enumerate(encoding))
            line = f"0x{instr_val:08x}\t{asm}\n"
            f.write(line)
            count += 1

    print(f"Wrote {out_path} ({count} instructions)")
    return 0

if __name__ == "__main__":
    sys.exit(main())