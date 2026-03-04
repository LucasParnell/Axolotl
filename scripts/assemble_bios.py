#!/usr/bin/env python3
"""
assemble_bios.py — Assemble GBA BIOS ARM/Thumb code sequences into a C++ header.

Uses the Keystone Engine to assemble ARM (ARMv4T) and Thumb assembly into
machine code, then emits a C++ header with named constexpr arrays that the
test suite can reference.

Sources for assembly:
  - GBATEK v3.06 (Martin Korth) — BIOS IRQ handler disassembly
    https://problemkaputt.de/gbatek-gba-interrupt-control.htm
  - Axolotl analysis/dis/con/arm_combined.dis — traced BIOS blocks
  - ARM Architecture Reference Manual (ARM DDI 0100E) — encoding verification

Usage:
    python3 scripts/assemble_bios.py > tests/bios_assembled.h
"""

import sys
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KS_MODE_THUMB

ks_arm   = Ks(KS_ARCH_ARM, KS_MODE_ARM)
ks_thumb = Ks(KS_ARCH_ARM, KS_MODE_THUMB)


def asm_arm(code: str, base: int = 0) -> list[int]:
    """Assemble ARM (32-bit) code. Returns list of uint32_t words."""
    encoding, count = ks_arm.asm(code, base)
    assert len(encoding) % 4 == 0, f"ARM code not word-aligned: {len(encoding)} bytes"
    words = []
    for i in range(0, len(encoding), 4):
        w = (encoding[i]
             | (encoding[i + 1] << 8)
             | (encoding[i + 2] << 16)
             | (encoding[i + 3] << 24))
        words.append(w)
    return words


def asm_thumb(code: str, base: int = 0) -> list[int]:
    """Assemble Thumb (16-bit) code. Returns list of uint16_t halfwords.
    BL instructions produce two halfwords (hi/lo pair)."""
    encoding, count = ks_thumb.asm(code, base)
    hwords = []
    for i in range(0, len(encoding), 2):
        h = encoding[i] | (encoding[i + 1] << 8)
        hwords.append(h)
    return hwords


# ═══════════════════════════════════════════════════════════════════════════════
#  BIOS Code Sequences — sourced from GBATEK and traced disassembly
# ═══════════════════════════════════════════════════════════════════════════════

SEQUENCES = []  # (name, mode, base_addr, asm_text, source_citation)


# ─── Reset Vector (address 0x00000000) ────────────────────────────────────────
# Source: analysis/dis/con/arm_combined.dis block 0x0-0x4
# Cross-ref: ARM exception vector table, reset vector at 0x00000000
SEQUENCES.append((
    "kBiosResetVector",
    "arm", 0x00000000,
    "b 0x68",
    "[GBATEK-IRQ] ARM exception vector table: reset vector branches to BIOS init.\n"
    "    // Verified: analysis/dis/con/arm_combined.dis block 0x0-0x4\n"
    "    // ARM-ARM: B instruction, 24-bit signed offset, PC-relative (PC+8+offset*4)"
))


# ─── IRQ Vector (address 0x00000018) ─────────────────────────────────────────
# Source: GBATEK "BIOS Interrupt handling" disassembly
# "00000018  b  128h  ;IRQ vector: jump to actual BIOS handler"
SEQUENCES.append((
    "kBiosIrqVector",
    "arm", 0x00000018,
    "b 0x128",
    "[GBATEK-IRQ] \"00000018  b  128h  ;IRQ vector: jump to actual BIOS handler\"\n"
    "    // ARM exception vector table: IRQ entry at 0x18 branches to 0x128"
))


# ─── BIOS IRQ Handler (address 0x00000128–0x0000013C) ────────────────────────
# Source: GBATEK "BIOS Interrupt handling" disassembly, verified against ARM-ARM
#   00000128  stmfd  r13!,{r0-r3,r12,r14}   ;save registers to SP_irq
#   0000012C  mov    r0, #0x4000000          ;ptr+4 to 03FFFFFC (mirror of 03007FFC)
#   00000130  add    r14, r15, #0            ;retadr for USER handler $+8=138h
#   00000134  ldr    r15, [r0, #-4]          ;jump to [03FFFFFC] USER handler
#   00000138  ldmfd  r13!,{r0-r3,r12,r14}   ;restore registers from SP_irq
#   0000013C  subs   r15, r14, #4            ;return from IRQ (PC=LR-4, CPSR=SPSR)

# We assemble each instruction individually at its correct address to get
# correct PC-relative encoding, then combine them.

# STMFD R13!, {R0-R3, R12, R14}  @ 0x128
SEQUENCES.append((
    "kBiosIrqHandler_STMFD",
    "arm", 0x00000128,
    "stmfd sp!, {r0-r3, r12, lr}",
    "[GBATEK-IRQ] \"00000128 stmfd r13!,{r0-r3,r12,r14}  ;save registers to SP_irq\"\n"
    "    // ARM-ARM §A4.1.98 STM: Store Multiple, FD = Full Descending (= DB with writeback)\n"
    "    // Saves R0,R1,R2,R3,R12,R14 to the IRQ stack pointed by SP_irq"
))

# MOV R0, #0x04000000  @ 0x12C
SEQUENCES.append((
    "kBiosIrqHandler_MOV_R0",
    "arm", 0x0000012C,
    "mov r0, #0x04000000",
    "[GBATEK-IRQ] \"0000012C mov r0, 4000000h  ;ptr+4 to 03FFFFFC\"\n"
    "    // ARM-ARM §A4.1.35 MOV: Rd = imm8 ROR (rot*2)\n"
    "    // Encoding: imm8=1, rot=3 → 1 ROR 6 = 0x04000000\n"
    "    // This sets up the base for LDR R15,[R0,#-4] = [0x03FFFFFC]"
))

# ADD R14, R15, #0  @ 0x130
SEQUENCES.append((
    "kBiosIrqHandler_ADD_LR",
    "arm", 0x00000130,
    "add lr, pc, #0",
    "[GBATEK-IRQ] \"00000130 add r14, r15, #0  ;retadr for USER handler $+8=138h\"\n"
    "    // ARM-ARM §A4.1.3 ADD: LR = PC + 0 = 0x130 + 8 = 0x138\n"
    "    // ARM7TDMI 3-stage pipeline: PC reads as current_addr + 8 in ARM state"
))

# LDR R15, [R0, #-4]  @ 0x134
SEQUENCES.append((
    "kBiosIrqHandler_LDR_PC",
    "arm", 0x00000134,
    "ldr pc, [r0, #-4]",
    "[GBATEK-IRQ] \"00000134 ldr r15,[r0,-4h]  ;jump to [03FFFFFC] USER handler\"\n"
    "    // ARM-ARM §A4.1.23 LDR: PC = Memory[R0 - 4] = Memory[0x03FFFFFC]\n"
    "    // 0x03FFFFFC is the IWRAM mirror of 0x03007FFC (user IRQ handler pointer)"
))

# LDMFD R13!, {R0-R3, R12, R14}  @ 0x138
SEQUENCES.append((
    "kBiosIrqHandler_LDMFD",
    "arm", 0x00000138,
    "ldmfd sp!, {r0-r3, r12, lr}",
    "[GBATEK-IRQ] \"00000138 ldmfd r13!,{r0-r3,r12,r14}  ;restore from SP_irq\"\n"
    "    // ARM-ARM §A4.1.22 LDM: Load Multiple, FD = Full Descending (= IA with writeback)\n"
    "    // Restores the registers saved by the STMFD at 0x128"
))

# SUBS R15, R14, #4  @ 0x13C
SEQUENCES.append((
    "kBiosIrqHandler_SUBS_PC",
    "arm", 0x0000013C,
    "subs pc, lr, #4",
    "[GBATEK-IRQ] \"0000013C subs r15,r14,4h  ;return from IRQ (PC=LR-4, CPSR=SPSR)\"\n"
    "    // ARM-ARM §A4.1.106 SUB with S=1 and Rd=R15: PC = LR - 4, CPSR = SPSR_irq\n"
    "    // This atomically restores both the program counter and the saved flags"
))

# Full IRQ handler block (all 6 instructions assembled contiguously)
SEQUENCES.append((
    "kBiosIrqHandlerBlock",
    "arm", 0x00000128,
    """stmfd sp!, {r0-r3, r12, lr}
    mov r0, #0x04000000
    add lr, pc, #0
    ldr pc, [r0, #-4]
    ldmfd sp!, {r0-r3, r12, lr}
    subs pc, lr, #4""",
    "[GBATEK-IRQ] Full BIOS IRQ handler at 0x128-0x13C (6 ARM instructions)\n"
    "    // Source: https://problemkaputt.de/gbatek-gba-interrupt-control.htm\n"
    "    //   00000128  stmfd  r13!,{r0-r3,r12,r14}\n"
    "    //   0000012C  mov    r0, #0x4000000\n"
    "    //   00000130  add    r14, r15, #0\n"
    "    //   00000134  ldr    r15, [r0, #-4]\n"
    "    //   00000138  ldmfd  r13!,{r0-r3,r12,r14}\n"
    "    //   0000013C  subs   r15, r14, #4"
))


# ─── BIOS Init: Mode Setup (address 0x0000008C–0x0000009C) ───────────────────
# Source: analysis/dis/con/arm_combined.dis block 0x8c-0xa0
#   0x0000008c:  mov  r0, #0xdf
#   0x00000090:  msr  cpsr_fc, r0
#   0x00000094:  mov  r4, #0x4000000
#   0x00000098:  strb r4, [r4, #0x208]
#   0x0000009c:  bl   #0xe0
SEQUENCES.append((
    "kBiosInitModeSetup",
    "arm", 0x0000008C,
    """mov r0, #0xDF
    msr cpsr_fc, r0
    mov r4, #0x04000000
    strb r4, [r4, #0x208]
    bl 0xe0""",
    "[GBATEK-IRQ] BIOS init: switch to SYS mode with IRQ+FIQ disabled\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x8c-0xa0\n"
    "    // 0xDF = 0b11011111 = I=1,F=1,T=0,Mode=0x1F(SYS) [GBATEK-FLAG]\n"
    "    // STRB R4,[R4,#0x208]: writes 0x00 to IME (0x04000208), disabling IRQs\n"
    "    // BL 0xE0: calls the stack initialization routine"
))


# ─── BIOS Stack Init (address 0x000000E0–0x00000118) ─────────────────────────
# Source: analysis/dis/con/arm_combined.dis block 0xe0-0x11c
# This sets up SP_svc, SP_irq, SP_usr per GBATEK default memory map.
SEQUENCES.append((
    "kBiosStackInit",
    "arm", 0x000000E0,
    # Note: LDR from literal pool uses PC-relative. We use MOV/MOVT equivalents
    # or direct immediates where the assembler can encode them. For the literal
    # pool loads, we use the exact instructions from the disassembly.
    """mov r0, #0xD3
    msr cpsr_fc, r0
    ldr sp, [pc, #0xD0]
    mov lr, #0
    msr spsr_fc, lr
    mov r0, #0xD2
    msr cpsr_fc, r0
    ldr sp, [pc, #0xB8]
    mov lr, #0
    msr spsr_fc, lr
    mov r0, #0x5F
    msr cpsr_fc, r0
    ldr sp, [pc, #0xA0]
    add r0, pc, #1
    bx r0""",
    "[GBATEK-IRQ] BIOS stack initialization at 0xE0-0x118\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xe0-0x11c\n"
    "    // Sets up three CPU mode stacks per [GBATEK-IRQ] default memory layout:\n"
    "    //   0xD3 = SVC mode (I=1,F=1,Mode=0x13): SP_svc = [literal] = 0x03007FE0\n"
    "    //   0xD2 = IRQ mode (I=1,F=1,Mode=0x12): SP_irq = [literal] = 0x03007FA0\n"
    "    //   0x5F = SYS mode (I=0,F=1,Mode=0x1F): SP_usr = [literal] = 0x03007F00\n"
    "    // Then BX to Thumb code at 0x11D for the clear loop"
))


# ─── BIOS Startup Check (address 0x00000068–0x00000088) ──────────────────────
# Source: analysis/dis/con/arm_combined.dis block 0x68-0x8c
SEQUENCES.append((
    "kBiosStartupCheck",
    "arm", 0x00000068,
    """cmp lr, #0
    moveq lr, #4
    mov r12, #0x04000000
    ldrb r12, [r12, #0x300]
    teq r12, #1
    mrseq r12, cpsr
    orreq r12, r12, #0xC0
    msreq cpsr_fc, r12
    beq 0x1c""",
    "[GBATEK-SYS] BIOS startup: check POSTFLG and warm-boot path\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x68-0x8c\n"
    "    // CMP LR, #0: on first boot LR=0 (reset), set to 4 if so\n"
    "    // LDRB R12, [0x04000300]: read POSTFLG [GBATEK-SYS]\n"
    "    // TEQ R12, #1: if POSTFLG==1, this is a warm boot (SWI 00h SoftReset)\n"
    "    // If warm boot: set I+F bits in CPSR, branch to 0x1C (skip full init)"
))


# ─── BIOS Clear Loop (Thumb, address 0x0000011C–0x00000126) ──────────────────
# Source: analysis/dis/con/arm_combined.dis block 0x11c-0x126
SEQUENCES.append((
    "kBiosClearLoop",
    "thumb", 0x0000011C,
    """movs r0, #0
    ldr r1, [pc, #0x160]
    str r0, [r4, r1]
    adds r1, r1, #4
    blt 0x120""",
    "[GBATEK-RST] BIOS SoftReset clear loop (Thumb) at 0x11C-0x124\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x11c-0x126\n"
    "    // Clears 0x200 bytes at [R4+R1] (R4=0x04000000 base, R1 starts negative)\n"
    "    // [GBATEK-RST]: \"SWI 00h SoftReset: clears 200h bytes of RAM\""
))


# ─── BIOS Clear Loop Return (Thumb, address 0x00000126) ──────────────────────
SEQUENCES.append((
    "kBiosClearLoopReturn",
    "thumb", 0x00000126,
    "bx lr",
    "[ARM-ARM] BX LR: branch-exchange to return address\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x126-0x128\n"
    "    // Returns from the clear subroutine to the caller"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  RegisterRamReset (SWI 01h) — GBATEK-RST
#  Source: analysis/dis/con/arm_combined.dis blocks 0x9C2–0xAB2
#  [GBATEK-RST]: "SWI 01h (GBA) - RegisterRamReset"
#    R0 = ResetFlags (bit0-7 select regions to clear)
#    Always forces DISPCNT = 0x0080 (forced blank) during reset.
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Build I/O Base Address (Thumb, 0x09CA–0x09CE) ───────────────────────────
# [GBATEK-IO]: I/O registers start at 0x04000000
# ARM-ARM Thumb §A7.1.38 MOVS + §A7.1.25 LSLS: R4 = 4 << 24 = 0x04000000
# This pattern is used because Thumb immediates are limited to 8 bits.
SEQUENCES.append((
    "kBiosRegRamReset_BuildIOBase",
    "thumb", 0x000009CA,
    """movs r4, #4
    lsls r4, r4, #0x18""",
    "[GBATEK-RST] RegisterRamReset: build I/O base 0x04000000\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x9c2-0x9dc\n"
    "    // [GBATEK-IO] GBA I/O base = 0x04000000\n"
    "    // Thumb can't encode 0x04000000 as immediate, so uses MOVS+LSLS\n"
    "    // R4 = 4 << 24 = 0x04000000"
))

# ─── Force DISPCNT = 0x0080 (Thumb, 0x09D2–0x09D6) ──────────────────────────
# [GBATEK-RST]: RegisterRamReset "always sets DISPCNT=0080h (forced blank)"
# [GBATEK-LCD]: DISPCNT at 0x04000000, bit 7 = Forced Blank
# ARM-ARM Thumb §A7.1.38 MOVS + §A7.1.60 STRH
SEQUENCES.append((
    "kBiosRegRamReset_ForcedBlank",
    "thumb", 0x000009D2,
    """movs r1, #0x80
    strh r1, [r4]""",
    "[GBATEK-RST] RegisterRamReset: DISPCNT = 0x0080 (forced blank)\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x9c2-0x9dc\n"
    "    // [GBATEK-LCD] DISPCNT at 0x04000000, bit 7 = Forced Blank\n"
    "    // [GBATEK-RST] \"always sets DISPCNT=0080h\" during RegisterRamReset\n"
    "    // Requires R4 = 0x04000000 (built by kBiosRegRamReset_BuildIOBase)"
))

# ─── Test Bit 7 of Reset Flags (Thumb, 0x09D6–0x09DC) ───────────────────────
# [GBATEK-RST]: RegisterRamReset R0 bit 7 = "Registers (icon palette,..."
# ARM-ARM Thumb §A7.1.68 TST: CPSR.Z = !(R6 AND R7)
SEQUENCES.append((
    "kBiosRegRamReset_TestBit7",
    "thumb", 0x000009D6,
    """movs r6, #0x80
    tst r6, r7
    beq 0xa18""",
    "[GBATEK-RST] RegisterRamReset: test bit 7 of reset flags\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x9c2-0x9dc\n"
    "    // R7 = original R0 (flags parameter), R6 = 0x80 (bit 7 mask)\n"
    "    // [GBATEK-RST] bit 7 = 'Registers' (icon palette, etc.)\n"
    "    // If bit not set, skip to 0xA18 (next flag check)"
))

# ─── Flag Gate Subroutine (Thumb, 0x0AAC–0x0AB0) ────────────────────────────
# Called by RegisterRamReset for each memory region.
# Tests if flag bit in R6 is set in R7 (the original R0 flags).
# ARM-ARM Thumb §A7.1.68 TST + §A7.1.7 BNE
SEQUENCES.append((
    "kBiosRegRamReset_FlagGate",
    "thumb", 0x00000AAC,
    """tst r6, r7
    bne 0xAB2""",
    "[GBATEK-RST] RegisterRamReset: flag gate subroutine\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xaac-0xab0\n"
    "    // Tests if bit in R6 is set in R7 (flags parameter)\n"
    "    // If set (NE), branches to 0xAB2 (proceed with clear)\n"
    "    // If not set (EQ), falls through to BX LR (skip this region)"
))

# ─── Flag Gate Return (Thumb, 0x0AB0) ────────────────────────────────────────
SEQUENCES.append((
    "kBiosRegRamReset_GateReturn",
    "thumb", 0x00000AB0,
    "bx lr",
    "[GBATEK-RST] RegisterRamReset: gate return (skip unselected region)\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xab0-0xab2\n"
    "    // If the flag bit was not set, return to caller without clearing"
))

# ─── RegisterRamReset Epilogue (Thumb, 0x0AA4–0x0AAC) ────────────────────────
# Standard Thumb function epilogue: restore callee-saved regs + return
# [ARM-ARM] Thumb §A7.1.44 POP: loads regs from stack, increments SP
SEQUENCES.append((
    "kBiosRegRamReset_Epilogue",
    "thumb", 0x00000AA4,
    """add sp, #4
    pop {r4, r5, r6, r7}
    pop {r3}
    bx r3""",
    "[GBATEK-RST] RegisterRamReset: function epilogue\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xaa4-0xaac\n"
    "    // Restores callee-saved registers and returns via R3\n"
    "    // Uses POP {R3}; BX R3 pattern (Thumb can't POP to PC on ARMv4T)"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  SoundBias (SWI 19h) — GBATEK-SND
#  Source: analysis/dis/con/arm_combined.dis blocks 0x800–0x82C
#  [GBATEK-SND]: "SWI 19h (GBA) - SoundBias"
#    R0 = Bias level (0=disable, 1-FFFFh=enable with delay)
#    Reads/writes SOUNDBIAS register at 0x04000088
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Build Step Value (Thumb, 0x0800–0x0804) ─────────────────────────────────
# [GBATEK-SND]: SoundBias builds step = 0x200 for the bias level increment
# ARM-ARM Thumb §A7.1.38 MOVS + §A7.1.25 LSLS: R1 = 2 << 8 = 0x200
SEQUENCES.append((
    "kBiosSoundBias_BuildStep",
    "thumb", 0x00000800,
    """movs r1, #2
    lsls r1, r1, #8""",
    "[GBATEK-SND] SoundBias: build step value 0x200\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x800-0x814\n"
    "    // R1 = 2 << 8 = 0x200 (bias increment step)\n"
    "    // [GBATEK-SND] SOUNDBIAS register at 0x04000088"
))

# ─── Move Step to IP (Thumb, 0x0804) ─────────────────────────────────────────
# ARM-ARM Thumb §A7.1.35 MOV (high registers): IP = R1
# This saves the step value in IP (R12) for later use in the loop.
SEQUENCES.append((
    "kBiosSoundBias_SaveStep",
    "thumb", 0x00000804,
    "mov ip, r1",
    "[GBATEK-SND] SoundBias: save step to IP\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x800-0x814\n"
    "    // MOV IP, R1: saves 0x200 step value in R12 for loop comparison"
))

# ─── SoundBias Return (Thumb, 0x082C) ────────────────────────────────────────
SEQUENCES.append((
    "kBiosSoundBias_Return",
    "thumb", 0x0000082C,
    "bx lr",
    "[GBATEK-SND] SoundBias: function return\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x82c-0x82e"
))

# ─── SoundBias Check Direction (Thumb, 0x0810–0x0814) ────────────────────────
# [GBATEK-SND]: If R0 == 0, decrease bias; otherwise increase
# ARM-ARM Thumb §A7.1.12 CMP + §A7.1.7 BEQ
SEQUENCES.append((
    "kBiosSoundBias_CheckDir",
    "thumb", 0x00000810,
    """cmp r0, #0
    beq 0x81C""",
    "[GBATEK-SND] SoundBias: check direction (increase vs decrease)\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x800-0x814\n"
    "    // R0 = delay count parameter from caller\n"
    "    // If R0 == 0, branch to decrease path at 0x81C"
))

# ─── SoundBias Compare Step (Thumb, 0x0814–0x0818) ───────────────────────────
# ARM-ARM Thumb §A7.1.12 CMP (high regs): compare current vs max step
SEQUENCES.append((
    "kBiosSoundBias_CmpStep",
    "thumb", 0x00000814,
    """cmp r1, ip
    bge 0x82C""",
    "[GBATEK-SND] SoundBias: compare bias level vs max step\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x814-0x818\n"
    "    // If R1 >= IP (0x200), bias is at max, return via 0x82C"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  GetBiosChecksum (SWI 0Dh) — GBATEK-RST
#  Source: analysis/dis/con/arm_combined.dis blocks 0x5A4–0x6C0
#  [GBATEK-RST]: "SWI 0Dh (GBA) - GetBiosChecksum"
#    Returns 0xBAAE187F in R0 (checksum of BIOS ROM)
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Build ROM Address (Thumb, 0x05A6–0x05B2) ────────────────────────────────
# Builds address R0 = 0x0800009D and length R1 = 0x1B for checksum calculation
# [GBATEK-MEM]: Game Pak ROM starts at 0x08000000
SEQUENCES.append((
    "kBiosGetChecksumSetup",
    "thumb", 0x000005A6,
    """movs r6, #8
    lsls r6, r6, #0x18
    movs r5, #0x9E
    adds r5, r5, r6
    subs r0, r5, #1
    movs r1, #0x1B""",
    "[GBATEK-RST] GetBiosChecksum: build ROM address parameters\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x5a4-0x5b6\n"
    "    // R6 = 8 << 24 = 0x08000000 (ROM base)\n"
    "    // R5 = 0x08000000 + 0x9E = 0x0800009E\n"
    "    // R0 = R5 - 1 = 0x0800009D (start address for checksum)\n"
    "    // R1 = 0x1B (iteration count)\n"
    "    // [GBATEK-RST] Expected result: 0xBAAE187F"
))

# ─── Checksum Inner Loop (Thumb, 0x06B8–0x06C0) ─────────────────────────────
# XOR-based checksum with byte rotation: EOR, LSL by 8, decrement counter
# ARM-ARM Thumb §A7.1.18 EORS + §A7.1.25 LSLS + §A7.1.65 SUBS + §A7.1.7 BGT
SEQUENCES.append((
    "kBiosChecksumInnerLoop",
    "thumb", 0x000006B8,
    """eors r3, r2
    lsls r2, r2, #8
    subs r5, r5, #1
    bgt 0x6B8""",
    "[GBATEK-RST] GetBiosChecksum: inner XOR loop\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x6b8-0x6c0\n"
    "    // R3 ^= R2 (accumulate checksum)\n"
    "    // R2 <<= 8 (shift byte for next XOR position)\n"
    "    // R5 -= 1; if R5 > 0, repeat\n"
    "    // This processes one byte across 4 XOR positions (4 iterations)"
))

# ─── Checksum Entry: Prologue + First Byte Load (Thumb, 0x06AC–0x06B8) ──────
# Loads first byte and sets up the inner loop
SEQUENCES.append((
    "kBiosChecksumLoopEntry",
    "thumb", 0x000006AC,
    """push {r4, r5, lr}
    movs r4, #3
    movs r3, #0
    ldrb r2, [r0]
    rors r3, r4
    movs r5, #4""",
    "[GBATEK-RST] GetBiosChecksum: loop entry + first byte load\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x6ac-0x6c0\n"
    "    // PUSH callee-saved regs; R4 = 3 (ROR amount)\n"
    "    // R3 = 0 (accumulator); LDRB R2, [R0] (load first byte)\n"
    "    // ROR R3 by R4=3 bits (rotate accumulator)\n"
    "    // R5 = 4 (inner loop count: 4 XOR iterations per byte)"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  Boot Sequence — GBATEK-SYS
#  Source: analysis/dis/con/arm_combined.dis blocks 0x1928–0x1962
#  The main BIOS boot routine after hardware init.
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Write POSTFLG = 1 (Thumb, 0x1944–0x1948) ────────────────────────────────
# [GBATEK-SYS]: POSTFLG at 0x04000300
#   Bit 0: First Boot Flag (0=First, 1=Further)
#   "After initial reset, the BIOS initializes the register to 01h"
SEQUENCES.append((
    "kBiosBootPOSTFLG_Write",
    "thumb", 0x00001944,
    """movs r5, #1
    strb r5, [r0]""",
    "[GBATEK-SYS] Boot: write POSTFLG = 1 (first boot complete)\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x1942-0x194e\n"
    "    // R0 = 0x04000300 (loaded by LDR at 0x1942)\n"
    "    // [GBATEK-SYS] POSTFLG bit 0: 0=First boot, 1=Further\n"
    "    // \"After initial reset, the BIOS initializes the register to 01h\""
))

# ─── Set R0 = 0xFF for RegisterRamReset (Thumb, 0x1938) ──────────────────────
# [GBATEK-RST]: R0 = 0xFF means reset ALL regions
SEQUENCES.append((
    "kBiosBootRamResetArg",
    "thumb", 0x00001938,
    "movs r0, #0xFF",
    "[GBATEK-RST] Boot: set R0=0xFF (all reset flags) for RegisterRamReset\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x1928-0x1942\n"
    "    // [GBATEK-RST] R0 flags: bit0=EWRAM, bit1=IWRAM, bit2=Palette,\n"
    "    //   bit3=VRAM, bit4=OAM, bit5=SIO, bit6=Sound, bit7=Registers\n"
    "    // 0xFF = all bits set = reset everything"
))

# ─── Set R0 = 1 for SoundBias call (Thumb, 0x1948) ───────────────────────────
# [GBATEK-SND]: SoundBias R0=1 means default bias with short delay
SEQUENCES.append((
    "kBiosBootSoundBiasArg",
    "thumb", 0x00001948,
    "movs r0, #1",
    "[GBATEK-SND] Boot: set R0=1 (default) before calling SoundBias\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x1942-0x194e\n"
    "    // Called after writing POSTFLG: BL 0x800 (SoundBias)"
))

# ─── Write IE = VBlank (Thumb, 0x1954) ───────────────────────────────────────
# [GBATEK-IRQ]: IE at 0x04000200
#   Bit 0: LCD V-Blank interrupt enable
SEQUENCES.append((
    "kBiosBootIE_Write",
    "thumb", 0x00001954,
    "strh r5, [r6]",
    "[GBATEK-IRQ] Boot: write IE = 0x0001 (VBlank interrupt enable)\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0x194e-0x195e\n"
    "    // R5 = 1 (from POSTFLG write), R6 = 0x04000200 (IE register)\n"
    "    // [GBATEK-IRQ] IE bit 0 = LCD V-Blank, enabling VBlank IRQ"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  CpuSet / CpuFastSet Helper — GBATEK-MEM
#  Source: analysis/dis/con/arm_combined.dis blocks 0xB9C–0xBBC
#  Thumb-to-ARM state switch bridge used by memory copy functions.
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Thumb-to-ARM Bridge (Thumb, 0x0B9C–0x0BA2) ─────────────────────────────
# Switches from Thumb to ARM state for the CpuSet inner loop.
# [ARM-ARM] Thumb §A7.1.5 ADR: R3 = align(PC,4) + imm
# [ARM-ARM] Thumb §A7.1.35 MOV (high): IP = R4 (save fill mode flag)
# [ARM-ARM] §A4.1.7 BX: state switch, T = R3[0]
SEQUENCES.append((
    "kBiosThumbToArmBridge",
    "thumb", 0x00000B9C,
    """add r3, pc, #4
    mov ip, r4
    bx r3""",
    "[GBATEK-MEM] CpuSet helper: Thumb-to-ARM state switch\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xb9c-0xba2\n"
    "    // R3 = align(PC,4) + 4 = 0xBA0 + 4 = 0xBA4 (ARM code)\n"
    "    // IP = R4 (save fill mode flag for ARM code)\n"
    "    // BX R3: switch to ARM state at 0xBA4\n"
    "    // [ARM-ARM] BX: T bit = Rm[0]; since 0xBA4[0]=0, enters ARM state"
))

# ─── CpuSet Fill Mode Check (ARM, 0x0BA4–0x0BAC) ────────────────────────────
# [GBATEK-MEM]: CpuSet R2 bit 24 = Fill Flag (0=Copy, 1=Fill)
# ARM-ARM §A4.1.14 CMP + §A4.1.5 B: if IP==0, skip fill and return
SEQUENCES.append((
    "kBiosCpuSetFillCheck",
    "arm", 0x00000BA4,
    """cmp ip, #0
    beq 0xBBC""",
    "[GBATEK-MEM] CpuSet: fill mode check\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xba4-0xbac\n"
    "    // IP = fill flag (from Thumb bridge R4)\n"
    "    // If IP == 0, this is copy mode → BEQ to return at 0xBBC\n"
    "    // [GBATEK-MEM] CpuSet R2 bit 24: 0=Copy, 1=Fill"
))

# ─── CpuSet Return (ARM, 0x0BBC) ─────────────────────────────────────────────
SEQUENCES.append((
    "kBiosCpuSetReturn",
    "arm", 0x00000BBC,
    "bx lr",
    "[GBATEK-MEM] CpuSet: ARM return via BX LR\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xbbc-0xbc0\n"
    "    // Returns to Thumb caller (LR[0]=1 from Thumb BL)"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  SWI Instruction Encodings — ARM-ARM §A4.1.107 / Thumb §A7.1.66
#  [GBATEK-SWI]: "SWI nn*10000h" in ARM mode, "SWI nn" in Thumb mode
#  The BIOS handler reads the SWI number from the instruction at LR-4 (ARM)
#  or LR-2 (Thumb), using bits [23:16] for ARM and bits [7:0] for Thumb.
# ═══════════════════════════════════════════════════════════════════════════════

# ARM SWI encodings: 0xEF000000 | (nn << 16)
SEQUENCES.append((
    "kBiosSwiArm_SoftReset",
    "arm", 0x00000000,
    "swi #0x000000",
    "[GBATEK-SWI] ARM SWI 00h: SoftReset\n"
    "    // [ARM-ARM] §A4.1.107 SWI: cond=AL(0xE), bits[23:0] = comment field\n"
    "    // [GBATEK-SWI] ARM: handler reads bits[23:16] as function number\n"
    "    // Encoding: 0xEF000000 | (0x00 << 16) = 0xEF000000"
))

SEQUENCES.append((
    "kBiosSwiArm_RegisterRamReset",
    "arm", 0x00000000,
    "swi #0x010000",
    "[GBATEK-SWI] ARM SWI 01h: RegisterRamReset\n"
    "    // Encoding: 0xEF000000 | (0x01 << 16) = 0xEF010000"
))

SEQUENCES.append((
    "kBiosSwiArm_Halt",
    "arm", 0x00000000,
    "swi #0x020000",
    "[GBATEK-SWI] ARM SWI 02h: Halt\n"
    "    // [GBATEK-HALT] Writes 0x00 to HALTCNT (0x04000301)\n"
    "    // Encoding: 0xEF000000 | (0x02 << 16) = 0xEF020000"
))

SEQUENCES.append((
    "kBiosSwiArm_VBlankIntrWait",
    "arm", 0x00000000,
    "swi #0x050000",
    "[GBATEK-SWI] ARM SWI 05h: VBlankIntrWait\n"
    "    // [GBATEK-HALT] Sets R0=1, R1=1, then calls IntrWait\n"
    "    // Encoding: 0xEF000000 | (0x05 << 16) = 0xEF050000"
))

SEQUENCES.append((
    "kBiosSwiArm_Div",
    "arm", 0x00000000,
    "swi #0x060000",
    "[GBATEK-SWI] ARM SWI 06h: Div\n"
    "    // [GBATEK-MATH] R0=Number, R1=Denom → R0=Quot, R1=Rem, R3=|Quot|\n"
    "    // Encoding: 0xEF000000 | (0x06 << 16) = 0xEF060000"
))

SEQUENCES.append((
    "kBiosSwiArm_CpuSet",
    "arm", 0x00000000,
    "swi #0x0B0000",
    "[GBATEK-SWI] ARM SWI 0Bh: CpuSet\n"
    "    // [GBATEK-MEM] R0=Source, R1=Dest, R2=Length/Mode\n"
    "    // Encoding: 0xEF000000 | (0x0B << 16) = 0xEF0B0000"
))

SEQUENCES.append((
    "kBiosSwiArm_GetBiosChecksum",
    "arm", 0x00000000,
    "swi #0x0D0000",
    "[GBATEK-SWI] ARM SWI 0Dh: GetBiosChecksum\n"
    "    // [GBATEK-RST] Returns 0xBAAE187F in R0\n"
    "    // Encoding: 0xEF000000 | (0x0D << 16) = 0xEF0D0000"
))

SEQUENCES.append((
    "kBiosSwiArm_SoundBias",
    "arm", 0x00000000,
    "swi #0x190000",
    "[GBATEK-SWI] ARM SWI 19h: SoundBias\n"
    "    // [GBATEK-SND] R0=Bias level (0=disable, 1=default)\n"
    "    // Encoding: 0xEF000000 | (0x19 << 16) = 0xEF190000"
))

SEQUENCES.append((
    "kBiosSwiArm_CustomHalt",
    "arm", 0x00000000,
    "swi #0x270000",
    "[GBATEK-SWI] ARM SWI 27h: CustomHalt (undocumented)\n"
    "    // [GBATEK-HALT] R2[7:0] written to HALTCNT: 0x00=Halt, 0x80=Stop\n"
    "    // Encoding: 0xEF000000 | (0x27 << 16) = 0xEF270000"
))

# Thumb SWI encodings: 0xDF00 | nn
SEQUENCES.append((
    "kBiosSwiThumb_SoftReset",
    "thumb", 0x00000000,
    "swi #0x00",
    "[GBATEK-SWI] Thumb SWI 00h: SoftReset\n"
    "    // [ARM-ARM] Thumb §A7.1.66 SWI: bits[7:0] = comment field\n"
    "    // [GBATEK-SWI] Thumb: handler reads bits[7:0] as function number\n"
    "    // Encoding: 0xDF00"
))

SEQUENCES.append((
    "kBiosSwiThumb_RegisterRamReset",
    "thumb", 0x00000000,
    "swi #0x01",
    "[GBATEK-SWI] Thumb SWI 01h: RegisterRamReset\n"
    "    // Encoding: 0xDF01"
))

SEQUENCES.append((
    "kBiosSwiThumb_Halt",
    "thumb", 0x00000000,
    "swi #0x02",
    "[GBATEK-SWI] Thumb SWI 02h: Halt\n"
    "    // Encoding: 0xDF02"
))

SEQUENCES.append((
    "kBiosSwiThumb_VBlankIntrWait",
    "thumb", 0x00000000,
    "swi #0x05",
    "[GBATEK-SWI] Thumb SWI 05h: VBlankIntrWait\n"
    "    // Encoding: 0xDF05"
))

SEQUENCES.append((
    "kBiosSwiThumb_Div",
    "thumb", 0x00000000,
    "swi #0x06",
    "[GBATEK-SWI] Thumb SWI 06h: Div\n"
    "    // Encoding: 0xDF06"
))

SEQUENCES.append((
    "kBiosSwiThumb_CpuSet",
    "thumb", 0x00000000,
    "swi #0x0B",
    "[GBATEK-SWI] Thumb SWI 0Bh: CpuSet\n"
    "    // Encoding: 0xDF0B"
))

SEQUENCES.append((
    "kBiosSwiThumb_GetBiosChecksum",
    "thumb", 0x00000000,
    "swi #0x0D",
    "[GBATEK-SWI] Thumb SWI 0Dh: GetBiosChecksum\n"
    "    // Encoding: 0xDF0D"
))

SEQUENCES.append((
    "kBiosSwiThumb_SoundBias",
    "thumb", 0x00000000,
    "swi #0x19",
    "[GBATEK-SWI] Thumb SWI 19h: SoundBias\n"
    "    // Encoding: 0xDF19"
))

SEQUENCES.append((
    "kBiosSwiThumb_CustomHalt",
    "thumb", 0x00000000,
    "swi #0x27",
    "[GBATEK-SWI] Thumb SWI 27h: CustomHalt\n"
    "    // Encoding: 0xDF27"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  Post-Init Cart Header Jump (ARM, 0x00A0–0x00B0)
#  Source: analysis/dis/con/arm_combined.dis block 0xa0-0xb4
#  After stack init, sets up return address and jumps to boot sequence.
# ═══════════════════════════════════════════════════════════════════════════════

# ─── Post-Init: Save Cart Header Pointer + Jump (ARM, 0x00A0–0x00B0) ────────
# [ARM-ARM] §A4.1.3 ADD PC-rel: R0 = PC+8+0x258 = 0xA0+8+0x258 = 0x300
# [GBATEK-SYS]: 0x04000300 = POSTFLG region
SEQUENCES.append((
    "kBiosPostInitCartJump",
    "arm", 0x000000A0,
    """add r0, pc, #0x258
    str r0, [sp, #0xFC]
    ldr r0, [pc, #0x1CC]
    add lr, pc, #0
    bx r0""",
    "[GBATEK-SYS] Post-init: save handler pointer and jump to boot\n"
    "    // Source: analysis/dis/con/arm_combined.dis block 0xa0-0xb4\n"
    "    // ADD R0, PC, #0x258: R0 = 0xA0+8+0x258 = 0x300 (POSTFLG area)\n"
    "    // STR R0, [SP, #0xFC]: save to stack for later use\n"
    "    // LDR R0, [PC, #0x1CC]: load boot sequence addr (literal=0x1929)\n"
    "    // ADD LR, PC, #0: set return address = 0xAC+8 = 0xB4\n"
    "    // BX R0: jump to Thumb boot code at 0x1928"
))


# ═══════════════════════════════════════════════════════════════════════════════
#  Generate Header
# ═══════════════════════════════════════════════════════════════════════════════

def emit_header():
    print("// bios_assembled.h — Auto-generated by scripts/assemble_bios.py")
    print("// DO NOT EDIT MANUALLY. Re-generate with:")
    print("//   cd /path/to/Axolotl && scripts/venv/bin/python3 scripts/assemble_bios.py > tests/bios_assembled.h")
    print("//")
    print("// Sources:")
    print("//   [GBATEK]   Martin Korth, \"GBATEK — GBA/NDS Technical Info\", v3.06")
    print("//              https://problemkaputt.de/gbatek.htm")
    print("//   [GBATEK-IRQ] https://problemkaputt.de/gbatek-gba-interrupt-control.htm")
    print("//   [GBATEK-SYS] https://problemkaputt.de/gbatek-gba-system-control.htm")
    print("//   [GBATEK-RST] https://problemkaputt.de/gbatek-bios-reset-functions.htm")
    print("//   [GBATEK-FLAG] https://problemkaputt.de/gbatek-arm-cpu-flags-condition-field-cond.htm")
    print("//   [GBATEK-LCD] https://problemkaputt.de/gbatek-gba-lcd-io-display-control.htm")
    print("//   [GBATEK-SND] https://problemkaputt.de/gbatek-gba-sound-control-registers.htm")
    print("//   [GBATEK-MEM] https://problemkaputt.de/gbatek-gba-memory-map.htm")
    print("//   [GBATEK-SWI] https://problemkaputt.de/gbatek-gba-bios-functions.htm")
    print("//   [GBATEK-HALT] https://problemkaputt.de/gbatek-bios-halt-functions.htm")
    print("//   [GBATEK-MATH] https://problemkaputt.de/gbatek-bios-arithmetic-functions.htm")
    print("//   [ARM-ARM]  ARM Ltd., \"ARM Architecture Reference Manual\" (ARM DDI 0100E)")
    print("//   analysis/dis/con/arm_combined.dis — Axolotl traced BIOS blocks")
    print("//")
    print("#pragma once")
    print("#include <cstdint>")
    print("#include <cstddef>")
    print()

    for name, mode, base, asm_text, citation in SEQUENCES:
        # Assemble
        if mode == "arm":
            words = asm_arm(asm_text, base)
            ctype = "uint32_t"
            fmt = "0x{:08X}"
        else:
            words = asm_thumb(asm_text, base)
            ctype = "uint16_t"
            fmt = "0x{:04X}"

        # Emit citation comment
        print(f"// {citation}")

        # Show the assembly alongside each word
        clean_lines = [l.strip() for l in asm_text.strip().splitlines() if l.strip()]

        if mode == "arm":
            # 1 word per instruction
            print(f"constexpr {ctype} {name}[] = {{")
            for i, w in enumerate(words):
                addr = base + i * 4
                if i < len(clean_lines):
                    asm_comment = clean_lines[i]
                else:
                    asm_comment = ""
                hex_str = fmt.format(w)
                print(f"    {hex_str},  // 0x{addr:08X}: {asm_comment}")
            print("};")
        else:
            # Thumb: some instructions are 2 halfwords (BL). Map by address.
            print(f"constexpr {ctype} {name}[] = {{")
            addr = base
            line_idx = 0
            for i, h in enumerate(words):
                hex_str = fmt.format(h)
                if line_idx < len(clean_lines):
                    asm_comment = clean_lines[line_idx]
                else:
                    asm_comment = ""
                print(f"    {hex_str},  // 0x{addr:08X}: {asm_comment}")
                addr += 2
                # For 16-bit Thumb, each line is typically one halfword,
                # except BL which is two halfwords. Advance line on first halfword.
                line_idx += 1
            print("};")

        print(f"constexpr size_t {name}_count = sizeof({name}) / sizeof({name}[0]);")
        print(f"constexpr uint32_t {name}_base = 0x{base:08X};")
        print()


if __name__ == "__main__":
    emit_header()
