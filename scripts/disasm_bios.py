#!/usr/bin/env python3
"""Disassemble GBA BIOS at key addresses for debugging."""
import struct, sys

COND = ["eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","","nv"]
REGS = ["r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","sp","lr","pc"]
SHIFTS = ["lsl","lsr","asr","ror"]

def decode_arm(word, addr):
    """Very basic ARM disassembler sufficient for BIOS analysis."""
    cond = COND[(word >> 28) & 0xF]

    # B / BL
    if (word & 0x0E000000) == 0x0A000000:
        link = "bl" if (word >> 24) & 1 else "b"
        offset = word & 0x00FFFFFF
        if offset & 0x800000: offset -= 0x1000000
        target = addr + 8 + offset * 4
        return f"{link}{cond} 0x{target & 0xFFFFFFFF:08x}"

    # BX Rm
    if (word & 0x0FFFFFF0) == 0x012FFF10:
        rm = word & 0xF
        return f"bx{cond} {REGS[rm]}"

    # Data processing
    if (word & 0x0C000000) == 0x00000000:
        opcode = (word >> 21) & 0xF
        s = "s" if (word >> 20) & 1 else ""
        rn = (word >> 16) & 0xF
        rd = (word >> 12) & 0xF
        imm_flag = (word >> 25) & 1
        ops = ["and","eor","sub","rsb","add","adc","sbc","rsc",
               "tst","teq","cmp","cmn","orr","mov","bic","mvn"]
        name = ops[opcode]

        if imm_flag:
            imm8 = word & 0xFF
            rot = (word >> 8) & 0xF
            val = (imm8 >> (rot*2)) | (imm8 << (32 - rot*2)) if rot else imm8
            val &= 0xFFFFFFFF
            op2 = f"#0x{val:x}"
        else:
            rm = word & 0xF
            shift_type = (word >> 5) & 3
            if (word >> 4) & 1:
                rs = (word >> 8) & 0xF
                op2 = f"{REGS[rm]}, {SHIFTS[shift_type]} {REGS[rs]}"
            else:
                shift_amt = (word >> 7) & 0x1F
                if shift_amt == 0 and shift_type == 0:
                    op2 = REGS[rm]
                else:
                    op2 = f"{REGS[rm]}, {SHIFTS[shift_type]} #{shift_amt}"
            
        if opcode in (8,9,10,11):  # TST,TEQ,CMP,CMN
            return f"{name}{cond} {REGS[rn]}, {op2}"
        elif opcode in (13,15):  # MOV,MVN
            return f"{name}{s}{cond} {REGS[rd]}, {op2}"
        else:
            return f"{name}{s}{cond} {REGS[rd]}, {REGS[rn]}, {op2}"

    # LDR/STR
    if (word & 0x0C000000) == 0x04000000:
        l = "ldr" if (word >> 20) & 1 else "str"
        b = "b" if (word >> 22) & 1 else ""
        rd = (word >> 12) & 0xF
        rn = (word >> 16) & 0xF
        u = 1 if (word >> 23) & 1 else -1
        p = (word >> 24) & 1
        imm_flag = not ((word >> 25) & 1)  # note: inverted for LDR/STR
        if imm_flag:
            off = word & 0xFFF
            sign = "+" if u == 1 else "-"
            if off == 0:
                return f"{l}{b}{cond} {REGS[rd]}, [{REGS[rn]}]"
            elif p:
                return f"{l}{b}{cond} {REGS[rd]}, [{REGS[rn]}, #{sign}0x{off:x}]"
            else:
                return f"{l}{b}{cond} {REGS[rd]}, [{REGS[rn]}], #{sign}0x{off:x}"
        else:
            rm = word & 0xF
            sign = "+" if u == 1 else "-"
            return f"{l}{b}{cond} {REGS[rd]}, [{REGS[rn]}, {sign}{REGS[rm]}]"

    # LDRH/STRH/LDRSB/LDRSH
    if (word & 0x0E000090) == 0x00000090 and (word & 0x60):
        l = (word >> 20) & 1
        rd = (word >> 12) & 0xF
        rn = (word >> 16) & 0xF
        sh = (word >> 5) & 3
        suf = ["","h","sb","sh"][sh]
        name = f"ldr{suf}" if l else f"str{suf}"
        p = (word >> 24) & 1
        u = "+" if (word >> 23) & 1 else "-"
        if (word >> 22) & 1:  # immediate offset
            off = ((word >> 4) & 0xF0) | (word & 0xF)
            if p:
                return f"{name}{cond} {REGS[rd]}, [{REGS[rn]}, #{u}0x{off:x}]"
            else:
                return f"{name}{cond} {REGS[rd]}, [{REGS[rn]}], #{u}0x{off:x}"
        else:
            rm = word & 0xF
            return f"{name}{cond} {REGS[rd]}, [{REGS[rn]}, {u}{REGS[rm]}]"

    # LDM/STM
    if (word & 0x0E000000) == 0x08000000:
        l = "ldm" if (word >> 20) & 1 else "stm"
        rn = (word >> 16) & 0xF
        w = "!" if (word >> 21) & 1 else ""
        p = (word >> 24) & 1
        u = (word >> 23) & 1
        suf = {(0,0):"da",(0,1):"ia",(1,0):"db",(1,1):"ib"}[(p,u)]
        regs = [REGS[i] for i in range(16) if (word >> i) & 1]
        hat = "^" if (word >> 22) & 1 else ""
        return f"{l}{suf}{cond} {REGS[rn]}{w}, {{{', '.join(regs)}}}{hat}"

    # MRS
    if (word & 0x0FBF0FFF) == 0x010F0000:
        r = "spsr" if (word >> 22) & 1 else "cpsr"
        rd = (word >> 12) & 0xF
        return f"mrs{cond} {REGS[rd]}, {r}"

    # MSR
    if (word & 0x0FB0FFF0) == 0x0120F000:
        r = "spsr" if (word >> 22) & 1 else "cpsr"
        rm = word & 0xF
        fields = ""
        if (word >> 16) & 1: fields += "c"
        if (word >> 17) & 1: fields += "x"
        if (word >> 18) & 1: fields += "s"
        if (word >> 19) & 1: fields += "f"
        return f"msr{cond} {r}_{fields}, {REGS[rm]}"

    # MSR imm
    if (word & 0x0FB0F000) == 0x0320F000:
        r = "spsr" if (word >> 22) & 1 else "cpsr"
        imm8 = word & 0xFF
        rot = (word >> 8) & 0xF
        val = (imm8 >> (rot*2)) | (imm8 << (32 - rot*2)) if rot else imm8
        val &= 0xFFFFFFFF
        fields = ""
        if (word >> 16) & 1: fields += "c"
        if (word >> 17) & 1: fields += "x"
        if (word >> 18) & 1: fields += "s"
        if (word >> 19) & 1: fields += "f"
        return f"msr{cond} {r}_{fields}, #0x{val:x}"

    # MUL/MLA
    if (word & 0x0FC000F0) == 0x00000090:
        rd = (word >> 16) & 0xF
        rn = (word >> 12) & 0xF
        rs = (word >> 8) & 0xF
        rm = word & 0xF
        if (word >> 21) & 1:
            return f"mla{cond} {REGS[rd]}, {REGS[rm]}, {REGS[rs]}, {REGS[rn]}"
        else:
            return f"mul{cond} {REGS[rd]}, {REGS[rm]}, {REGS[rs]}"

    # SWI
    if (word & 0x0F000000) == 0x0F000000:
        comment = word & 0x00FFFFFF
        return f"swi{cond} #0x{comment:06x}"

    return f".word 0x{word:08x}  ; ???"


def decode_thumb(hw, addr, data):
    """Very basic Thumb disassembler."""
    # Format 1: Move shifted register
    if (hw >> 13) == 0:
        op = (hw >> 11) & 3
        offset = (hw >> 6) & 0x1F
        rs = (hw >> 3) & 7
        rd = hw & 7
        ops = ["lsl","lsr","asr"]
        if op < 3:
            return f"{ops[op]} {REGS[rd]}, {REGS[rs]}, #{offset}"

    # Format 2: add/sub
    if (hw >> 11) == 0b00011:
        op = (hw >> 9) & 1
        i = (hw >> 10) & 1
        rn_or_nn = (hw >> 6) & 7
        rs = (hw >> 3) & 7
        rd = hw & 7
        name = "sub" if op else "add"
        if i:
            return f"{name} {REGS[rd]}, {REGS[rs]}, #{rn_or_nn}"
        else:
            return f"{name} {REGS[rd]}, {REGS[rs]}, {REGS[rn_or_nn]}"

    # Format 3: Mov/Cmp/Add/Sub immediate
    if (hw >> 13) == 1:
        op = (hw >> 11) & 3
        rd = (hw >> 8) & 7
        imm = hw & 0xFF
        ops = ["mov","cmp","add","sub"]
        return f"{ops[op]} {REGS[rd]}, #0x{imm:x}"

    # Format 4: ALU ops
    if (hw >> 10) == 0b010000:
        op = (hw >> 6) & 0xF
        rs = (hw >> 3) & 7
        rd = hw & 7
        ops = ["and","eor","lsl","lsr","asr","adc","sbc","ror",
               "tst","neg","cmp","cmn","orr","mul","bic","mvn"]
        return f"{ops[op]} {REGS[rd]}, {REGS[rs]}"

    # Format 5: Hi register ops / BX
    if (hw >> 10) == 0b010001:
        op = (hw >> 8) & 3
        h1 = (hw >> 7) & 1
        h2 = (hw >> 6) & 1
        rs = ((hw >> 3) & 7) | (h2 << 3)
        rd = (hw & 7) | (h1 << 3)
        if op == 3:
            return f"bx {REGS[rs]}"
        ops = ["add","cmp","mov"]
        return f"{ops[op]} {REGS[rd]}, {REGS[rs]}"

    # Format 6: PC-relative load
    if (hw >> 11) == 0b01001:
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        target = (addr + 4) & ~2
        target += imm
        # Read the word at target if possible
        if target < len(data):
            val = struct.unpack_from('<I', data, target)[0]
            return f"ldr {REGS[rd]}, [pc, #0x{imm:x}]  ; =0x{val:08x} (@0x{target:04x})"
        return f"ldr {REGS[rd]}, [pc, #0x{imm:x}]  ; @0x{target:04x}"

    # Format 7/8: Load/store with register offset
    if (hw >> 12) == 0b0101:
        op = (hw >> 9) & 7
        ro = (hw >> 6) & 7
        rb = (hw >> 3) & 7
        rd = hw & 7
        names = ["str","strh","strb","ldrsb","ldr","ldrh","ldrb","ldrsh"]
        return f"{names[op]} {REGS[rd]}, [{REGS[rb]}, {REGS[ro]}]"

    # Format 9: Load/store with immediate offset
    if (hw >> 13) == 0b011:
        b = (hw >> 12) & 1
        l = (hw >> 11) & 1
        off = (hw >> 6) & 0x1F
        rb = (hw >> 3) & 7
        rd = hw & 7
        if b:
            name = "ldrb" if l else "strb"
            return f"{name} {REGS[rd]}, [{REGS[rb]}, #0x{off:x}]"
        else:
            name = "ldr" if l else "str"
            return f"{name} {REGS[rd]}, [{REGS[rb]}, #0x{off*4:x}]"

    # Format 10: Load/store halfword
    if (hw >> 12) == 0b1000:
        l = (hw >> 11) & 1
        off = ((hw >> 6) & 0x1F) * 2
        rb = (hw >> 3) & 7
        rd = hw & 7
        name = "ldrh" if l else "strh"
        return f"{name} {REGS[rd]}, [{REGS[rb]}, #0x{off:x}]"

    # Format 11: SP-relative load/store
    if (hw >> 12) == 0b1001:
        l = (hw >> 11) & 1
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        name = "ldr" if l else "str"
        return f"{name} {REGS[rd]}, [sp, #0x{imm:x}]"

    # Format 12: Load address
    if (hw >> 12) == 0b1010:
        sp = (hw >> 11) & 1
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        src = "sp" if sp else "pc"
        return f"add {REGS[rd]}, {src}, #0x{imm:x}"

    # Format 13: Add offset to SP
    if (hw >> 8) == 0b10110000:
        s = (hw >> 7) & 1
        imm = (hw & 0x7F) * 4
        if s:
            return f"sub sp, #0x{imm:x}"
        else:
            return f"add sp, #0x{imm:x}"

    # Format 14: Push/Pop
    if (hw >> 12) == 0b1011 and ((hw >> 9) & 3) == 0b10:
        l = (hw >> 11) & 1
        r = (hw >> 8) & 1
        regs = [REGS[i] for i in range(8) if (hw >> i) & 1]
        if l:
            if r: regs.append("pc")
            return f"pop {{{', '.join(regs)}}}"
        else:
            if r: regs.append("lr")
            return f"push {{{', '.join(regs)}}}"

    # Format 15: Multiple load/store
    if (hw >> 12) == 0b1100:
        l = (hw >> 11) & 1
        rb = (hw >> 8) & 7
        regs = [REGS[i] for i in range(8) if (hw >> i) & 1]
        name = "ldmia" if l else "stmia"
        return f"{name} {REGS[rb]}!, {{{', '.join(regs)}}}"

    # Format 16: Conditional branch
    if (hw >> 12) == 0b1101:
        cond_idx = (hw >> 8) & 0xF
        if cond_idx == 0xF:  # SWI
            return f"swi #0x{hw & 0xFF:02x}"
        if cond_idx < 15:
            offset = hw & 0xFF
            if offset & 0x80: offset -= 0x100
            target = addr + 4 + offset * 2
            return f"b{COND[cond_idx]} 0x{target & 0xFFFFFFFF:08x}"

    # Format 17: SWI (already handled in format 16 as cond=0xF)

    # Format 18: Unconditional branch
    if (hw >> 11) == 0b11100:
        offset = hw & 0x7FF
        if offset & 0x400: offset -= 0x800
        target = addr + 4 + offset * 2
        return f"b 0x{target & 0xFFFFFFFF:08x}"

    # Format 19: Long branch with link
    if (hw >> 11) == 0b11110:
        offset_hi = hw & 0x7FF
        if offset_hi & 0x400: offset_hi -= 0x800
        # Need next halfword
        if addr + 2 < len(data):
            hw2 = struct.unpack_from('<H', data, addr + 2)[0]
            if (hw2 >> 11) == 0b11111:
                offset_lo = hw2 & 0x7FF
                target = addr + 4 + (offset_hi << 12) + (offset_lo << 1)
                return f"bl 0x{target & 0xFFFFFFFF:08x}"
        return f"bl_hi #0x{offset_hi:x}  ; (first half)"

    return f".hword 0x{hw:04x}  ; ???"


def disasm_arm_range(data, start, end):
    print(f"\n{'='*60}")
    print(f"ARM disassembly: 0x{start:04X} - 0x{end:04X}")
    print(f"{'='*60}")
    for addr in range(start, end, 4):
        if addr + 4 > len(data): break
        word = struct.unpack_from('<I', data, addr)[0]
        inst = decode_arm(word, addr)
        print(f"  0x{addr:04x}: {word:08x}  {inst}")


def disasm_thumb_range(data, start, end):
    print(f"\n{'='*60}")
    print(f"THUMB disassembly: 0x{start:04X} - 0x{end:04X}")
    print(f"{'='*60}")
    addr = start
    while addr < end:
        if addr + 2 > len(data): break
        hw = struct.unpack_from('<H', data, addr)[0]
        inst = decode_thumb(hw, addr, data)
        # Check for BL (two halfwords)
        if (hw >> 11) == 0b11110 and addr + 4 <= len(data):
            hw2 = struct.unpack_from('<H', data, addr + 2)[0]
            if (hw2 >> 11) == 0b11111:
                print(f"  0x{addr:04x}: {hw:04x} {hw2:04x}  {inst}")
                addr += 4
                continue
        print(f"  0x{addr:04x}: {hw:04x}      {inst}")
        addr += 2


def main():
    with open("out/res/gba_bios.bin", "rb") as f:
        data = f.read()

    print("GBA BIOS Disassembly - Key Sections")
    print(f"Total size: {len(data)} bytes (0x{len(data):04X})")

    # 1. Exception vector table
    disasm_arm_range(data, 0x000, 0x020)

    # 2. SWI handler at 0x140
    print("\n### SWI HANDLER ###")
    disasm_arm_range(data, 0x140, 0x1A0)

    # 3. IRQ handler at 0x128
    print("\n### IRQ HANDLER ###")
    disasm_arm_range(data, 0x128, 0x140)

    # 4. Boot code at 0x68
    print("\n### BOOT/RESET HANDLER ###")
    disasm_arm_range(data, 0x068, 0x0E0)

    # 5. Loop at 0xC04 (ARM) - the first stuck loop
    print("\n### STUCK LOOP at 0xC04 (ARM) ###")
    disasm_arm_range(data, 0xB80, 0xC80)

    # 6. Code around 0x1774 (Thumb) - the second stuck loop
    print("\n### STUCK LOOP at 0x1774 (THUMB) ###")
    disasm_thumb_range(data, 0x1740, 0x17C0)

    # 7. Check if there's a BL or SWI pattern in boot area (0x68-0x300)
    print("\n### SCANNING FOR SWI INSTRUCTIONS IN BIOS ###")
    # ARM SWI: condition | 0b1111 | 24-bit comment
    swi_count = 0
    for addr in range(0, len(data) - 3, 4):
        word = struct.unpack_from('<I', data, addr)[0]
        if (word & 0x0F000000) == 0x0F000000:
            cond = COND[(word >> 28) & 0xF]
            comment = word & 0x00FFFFFF
            print(f"  ARM SWI at 0x{addr:04x}: swi{cond} #0x{comment:06x}")
            swi_count += 1

    # Thumb SWI: 0xDF__
    for addr in range(0, len(data) - 1, 2):
        hw = struct.unpack_from('<H', data, addr)[0]
        if (hw & 0xFF00) == 0xDF00:
            comment = hw & 0xFF
            print(f"  THUMB SWI at 0x{addr:04x}: swi #0x{comment:02x}")
            swi_count += 1

    print(f"\n  Total SWI instructions found in BIOS: {swi_count}")

    # 8. Scan for BL instructions in boot area to see what it calls
    print("\n### BL (Branch-with-Link) CALLS FROM BOOT CODE (0x68-0x300) ###")
    # ARM BL: 0xEB______
    for addr in range(0x68, 0x300, 4):
        word = struct.unpack_from('<I', data, addr)[0]
        if (word & 0x0F000000) == 0x0B000000:
            offset = word & 0x00FFFFFF
            if offset & 0x800000: offset -= 0x1000000
            target = addr + 8 + offset * 4
            cond = COND[(word >> 28) & 0xF]
            print(f"  ARM BL at 0x{addr:04x}: bl{cond} 0x{target & 0xFFFFFFFF:08x}")

    # Check more of the boot region that might be Thumb
    print("\n### BL CALLS FROM EXTENDED BOOT (0x300-0x1000) - ARM ###")
    for addr in range(0x300, 0x1000, 4):
        word = struct.unpack_from('<I', data, addr)[0]
        if (word & 0x0F000000) == 0x0B000000:
            offset = word & 0x00FFFFFF
            if offset & 0x800000: offset -= 0x1000000
            target = addr + 8 + offset * 4
            cond = COND[(word >> 28) & 0xF]
            print(f"  ARM BL at 0x{addr:04x}: bl{cond} 0x{target & 0xFFFFFFFF:08x}")

    # 9. Check what's at well-known BIOS routine addresses
    # Known routine addresses from various disassemblies:
    print("\n### BOOT CODE AFTER INIT - 0xE0-0x140 ###")
    disasm_arm_range(data, 0x0E0, 0x140)

    print("\n### CODE AT 0x1A0-0x200 ###")
    disasm_arm_range(data, 0x1A0, 0x200)

if __name__ == "__main__":
    main()
