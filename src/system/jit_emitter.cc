#include "system/jit_emitter.h"
#include "data/cpu_state.h"
#include "data/ir_node.h"
#include "system/memory_bus.h"
#include "util/debug_alloc_tag.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <string>
#include <xbyak/xbyak.h>

#ifndef LOGO_LINK_FORCE_SLOW_STORE
#define LOGO_LINK_FORCE_SLOW_STORE 0
#endif
#ifndef IRQ_WAITWORD_FORCE_SLOW_MEM
#define IRQ_WAITWORD_FORCE_SLOW_MEM 0
#endif

namespace {

constexpr size_t kFrameSize = 72;

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = static_cast<uint32_t>(-(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// Stack layout inside EmitBlock's frame (after sub rsp, kFrameSize):
//   [rsp +  0] rbx  (callee-saved, host ABI)
//   [rsp +  8] rbp  (callee-saved, host ABI)
//   [rsp + 16] r12  (callee-saved, host ABI — the original host r12, not the guest LR)
//   [rsp + 24] r13  (callee-saved, host ABI)
//   [rsp + 32] r14  (callee-saved, host ABI)
//   [rsp + 40] r15  (callee-saved, host ABI)
//   [rsp + 48] 32-bit scratch (EmitMsr, EmitExceptionReturn)
//   [rsp + 56] 32-bit scratch (EmitSwi old-CPSR)
//   [rsp + 64] r12 save slot for BeforeCall/AfterCall
//              r12d holds the current *guest* LR during block execution.
//              BeforeCall saves it here (NOT into CpuState::registers[14]) so
//              that SwapBankedRegisters — which legitimately reads and writes
//              registers[14] — cannot corrupt the live guest-LR value.
//              AfterCall restores from here.  Call sites that need the
//              post-swap LR (EmitSwi, EmitMsr, EmitExceptionReturn) still
//              reload r12d from registers[14] explicitly after AfterCall.
constexpr size_t kR12SaveOffset = 64;

// Guest-register → host-register index map.
// R0–R3   : r8d–r11d (indices 8–11)
// R4, R5  : memory-only; handled explicitly in gba() / LoadGbaToReg()
//           (sentinel 0 in table — never reached via Reg32 lookup)
// R6, R7  : r14d, r15d (indices 14, 15)
// R8–R12  : memory-only; gba() loads into eax (Reg32(0)) for these
// R13(SP) : ebp  (index 5)
// R14(LR) : r12d (index 12)  ← live in r12d throughout the whole block
// R15(PC) : ebx  (index 3)
static constexpr uint8_t kGbaToX86Idx[16] = {
    8, 9, 10, 11, 0, 0, 14, 15,
    0, 0, 0, 0, 0,
    5, 12, 3
};

}

static unsigned ModeToBankIndex(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case 0x11u: return 1;  
        case 0x12u: return 2;  
        case 0x13u: return 3;  
        case 0x17u: return 4;  
        case 0x1Bu: return 5;  
        default: return 0;     
    }
}

extern "C" void SwapBankedRegisters(CpuState* state, uint32_t old_mode, uint32_t new_mode) {
    const unsigned old_idx = ModeToBankIndex(old_mode);
    const unsigned new_idx = ModeToBankIndex(new_mode);
    if (old_idx == new_idx) return;

    state->bank_r13[old_idx] = state->registers[13];
    state->bank_r14[old_idx] = state->registers[14];

    constexpr unsigned kFiqIdx = 1;
    if (old_idx == kFiqIdx) {
        for (int i = 0; i < 5; ++i) state->bank_r8_r12[1][i] = state->registers[8 + i];
    } else {
        for (int i = 0; i < 5; ++i) state->bank_r8_r12[0][i] = state->registers[8 + i];
    }
    if (new_idx == kFiqIdx) {
        for (int i = 0; i < 5; ++i) state->registers[8 + i] = state->bank_r8_r12[1][i];
    } else {
        for (int i = 0; i < 5; ++i) state->registers[8 + i] = state->bank_r8_r12[0][i];
    }

    if (old_idx != 0) state->bank_spsr[old_idx] = state->spsr;
    if (new_idx != 0) state->spsr = state->bank_spsr[new_idx];

    state->registers[13] = state->bank_r13[new_idx];
    state->registers[14] = state->bank_r14[new_idx];
}

extern "C" uint32_t MaterializeCPSR(CpuState* state) {
    uint32_t n, z, c, v;
    uint32_t res = state->flag_res;
    uint32_t op1 = state->flag_op1;
    uint32_t op2 = state->flag_op2;

    switch (static_cast<LazyOp>(state->flag_op)) {
        case kLazyAdd:
            n = res >> 31;
            z = (res == 0);
            c = (res < op1);
            v = ((op1 ^ res) & (op2 ^ res)) >> 31;
            break;
        case kLazySub:
            n = res >> 31;
            z = (res == 0);
            c = (op1 >= op2);
            v = ((op1 ^ op2) & (op1 ^ res)) >> 31;
            break;
        case kLazyLogic:
            n = res >> 31;
            z = (res == 0);
            c = (state->cpsr >> 29) & 1;
            v = (state->cpsr >> 28) & 1;
            break;
        default:
            return state->cpsr;
    }
    state->cpsr = (state->cpsr & 0x0FFFFFFFu) | (n << 31) | (z << 30) | (c << 29) | (v << 28);
    state->flag_op = static_cast<uint32_t>(kLazyNone);
    return state->cpsr;
}

extern "C" uint32_t JitGetUserReg(CpuState* s, uint32_t reg) {
    uint32_t mode = s->cpsr & 0x1Fu;
    if (mode == 0x10 || mode == 0x1F || reg < 8 || reg == 15) return s->registers[reg];
    if (mode == 0x11 && reg >= 8 && reg <= 12) return s->bank_r8_r12[0][reg - 8];
    if (reg == 13) return s->bank_r13[0];
    if (reg == 14) return s->bank_r14[0];
    return s->registers[reg];
}

extern "C" void JitSetUserReg(CpuState* s, uint32_t reg, uint32_t val) {
    uint32_t mode = s->cpsr & 0x1Fu;
    if (mode == 0x10 || mode == 0x1F || reg < 8 || reg == 15) { s->registers[reg] = val; return; }
    if (mode == 0x11 && reg >= 8 && reg <= 12) { s->bank_r8_r12[0][reg - 8] = val; return; }
    if (reg == 13) { s->bank_r13[0] = val; return; }
    if (reg == 14) { s->bank_r14[0] = val; return; }
    s->registers[reg] = val;
}

extern "C" uint32_t JitRead32(CpuState* s, uint32_t addr) {
    return static_cast<MemoryBus*>(s->memory_bus)->Read32(addr, s->registers[15]);
}
extern "C" void JitWrite32(CpuState* s, uint32_t addr, uint32_t value) {
    static_cast<MemoryBus*>(s->memory_bus)->Write32(s, addr, value);
}
extern "C" uint16_t JitRead16(CpuState* s, uint32_t addr) {
    return static_cast<MemoryBus*>(s->memory_bus)->Read16(addr, s->registers[15]);
}
extern "C" void JitWrite16(CpuState* s, uint32_t addr, uint16_t value) {
    static_cast<MemoryBus*>(s->memory_bus)->Write16(s, addr, value);
}
extern "C" uint8_t JitRead8(CpuState* s, uint32_t addr) {
    return static_cast<MemoryBus*>(s->memory_bus)->Read8(addr, s->registers[15]);
}
extern "C" void JitWrite8(CpuState* s, uint32_t addr, uint8_t value) {
    static_cast<MemoryBus*>(s->memory_bus)->Write8(s, addr, value);
}

#ifdef B_DEBUG
static const char* GbaSwiName(uint32_t num) {
    static const char* names[] = {
        "SoftReset", "RegisterRamReset", "Halt", "Stop",
        "IntrWait", "VBlankIntrWait", "Div", "DivArm",
        "Sqrt", "ArcTan", "ArcTan2", "CpuSet", "CpuFastSet",
        "GetBiosChecksum", "BgAffineSet", "ObjAffineSet",
        "BitUnPack", "LZ77UnCompWram", "LZ77UnCompVram",
        "HuffUnComp", "RLUnCompWram", "RLUnCompVram",
    };
    if (num < sizeof(names)/sizeof(names[0])) return names[num];
    return "???";
}

extern "C" void DebugLogSwi(CpuState* state, uint32_t swi_imm, uint32_t link_addr) {
    static int swi_count = 0;
    if (++swi_count > 300) return;
    uint32_t num = swi_imm & 0xFF;
    std::printf("[SWI] #0x%02X %-20s link=0x%08X  r0=%08X r1=%08X r2=%08X r3=%08X\n",
           num, GbaSwiName(num), link_addr,
           state->registers[0], state->registers[1],
           state->registers[2], state->registers[3]);
}
#endif

namespace {
thread_local std::function<void(uint32_t block_pc, uint32_t target_pc)> s_crash_dump_callback;
}

void CodeEmitter::SetJitCrashDumpCallback(std::function<void(uint32_t, uint32_t)> fn) {
    s_crash_dump_callback = std::move(fn);
}

extern "C" void JitCrashJumpToIO(uint32_t target_pc, uint32_t block_pc) {
    std::fprintf(stderr, "JIT: illegal jump to I/O space 0x%08X from block 0x%08X\n", target_pc, block_pc);
    if (s_crash_dump_callback)
        s_crash_dump_callback(block_pc, target_pc);
    std::abort();
}

struct CodeEmitter::Impl : Xbyak::CodeGenerator {
    explicit Impl(size_t buffer_size) : Xbyak::CodeGenerator(buffer_size), capacity_bytes_(buffer_size) {
        setDefaultJmpNEAR(true); 
        static std::atomic<uint8_t> init_state{0};  // 0=uninit, 1=initializing, 2=ready
        uint8_t expected = 0;
        if (init_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            InitDispatchTable();
            init_state.store(2, std::memory_order_release);
        } else {
            while (init_state.load(std::memory_order_acquire) != 2) {}
        }
    }

    size_t capacity_bytes_{0};
    Xbyak::Label* current_exit_label_{};
    uint32_t block_pc_{0};
    const uint8_t* block_host_entry_{nullptr};
    bool capture_relocs_{false};
    std::vector<CodeEmitter::RelocEntry> emitted_relocs_{};
 
    using EmitterFunc = void (*)(Impl* jit, const IrNode& n);
    static constexpr size_t kOpCount = 64u;
    static EmitterFunc g_dispatch_table[kOpCount][2][2];
    static void InitDispatchTable();

    template <IrOp Op, bool UsePool, bool SetFlags>
    static void EmitALU(Impl* jit, const IrNode& n);
    template <bool UsePool, bool SetFlags>
    static void EmitBic(Impl* jit, const IrNode& n);
    template <bool UsePool, bool SetFlags>
    static void EmitMov(Impl* jit, const IrNode& n);
    template <bool UsePool, bool SetFlags>
    static void EmitMvn(Impl* jit, const IrNode& n);
    template <bool UsePool>
    static void EmitCmp(Impl* jit, const IrNode& n);
    template <bool UsePool>
    static void EmitCmn(Impl* jit, const IrNode& n);
    template <bool UsePool>
    static void EmitTst(Impl* jit, const IrNode& n);
    template <bool UsePool>
    static void EmitTeq(Impl* jit, const IrNode& n);
    template <bool SetFlags>
    static void EmitMul(Impl* jit, const IrNode& n);
    template <bool SetFlags>
    static void EmitUmull(Impl* jit, const IrNode& n);
    template <bool SetFlags>
    static void EmitUmlal(Impl* jit, const IrNode& n);
    template <bool SetFlags>
    static void EmitSmull(Impl* jit, const IrNode& n);
    template <bool SetFlags>
    static void EmitSmlal(Impl* jit, const IrNode& n);
    template <bool UsePool, bool SetFlags>
    static void EmitRsb(Impl* jit, const IrNode& n);
    static void EmitLoadLiteral(Impl* jit, const IrNode& n);
    static void EmitLoadFast32(Impl* jit, const IrNode& n);
    static void EmitLoadFast16(Impl* jit, const IrNode& n);
    static void EmitLoadFast8(Impl* jit, const IrNode& n);
    static void EmitStoreFast32(Impl* jit, const IrNode& n);
    static void EmitStoreFast16(Impl* jit, const IrNode& n);
    static void EmitStoreFast8(Impl* jit, const IrNode& n);
    static void EmitBranch(Impl* jit, const IrNode& n);
    static void EmitCall(Impl* jit, const IrNode& n);
    static void EmitBranchExchange(Impl* jit, const IrNode& n);
    static void EmitMrs(Impl* jit, const IrNode& n);
    static void EmitMsr(Impl* jit, const IrNode& n);
    static void EmitSwi(Impl* jit, const IrNode& n);
    static void EmitLoad32(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 32, false>(n); }
    static void EmitStore32(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<false, 32>(n); }
    static void EmitLoadUser32(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 32, false, true>(n); }
    static void EmitStoreUser32(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<false, 32, false, true>(n); }
    static void EmitLoad16(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 16, false>(n); }
    static void EmitStore16(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<false, 16>(n); }
    static void EmitLoad8(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 8, false>(n); }
    static void EmitStore8(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<false, 8>(n); }
    static void EmitLoadSigned8(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 8, true>(n); }
    static void EmitLoadSigned16(Impl* jit, const IrNode& n) { jit->EmitMemGeneric<true, 16, true>(n); }

    Xbyak::Reg32 gba(uint32_t idx) {
        if (idx == 4) {
            mov(eax, dword[rdi + 16]); 
            return eax;
        }
        if (idx == 5) {
            mov(eax, dword[rdi + 20]); 
            return eax;
        }
        if (idx >= 8 && idx <= 12) mov(eax, dword[rdi + (idx * 4)]);
        return Xbyak::Reg32(kGbaToX86Idx[idx & 0xF]);
    }

    void StoreRd(uint32_t rd, Xbyak::Reg32 result_reg) {
        if (rd == 4) {
            mov(dword[rdi + 16], result_reg);
        } else if (rd == 5) {
            mov(dword[rdi + 20], result_reg);
        } else if (rd >= 8 && rd <= 12) {
            mov(dword[rdi + (rd * 4)], result_reg);
        } else {
            if (rd == 15) {
                // Align the branch target to the correct instruction boundary.
                // ARM DDI 0100E §A2.4.3 / §A4.1.2: branch targets written to R15 must be
                // aligned to 4 bytes (ARM) or 2 bytes (Thumb), with bit[0] CLEARED in both
                // cases (bit[0] does not encode Thumb state when writing PC directly — only
                // BX/BLX use bit[0] for interworking).
                //
                // The T-bit is tested from CPSR, not from result_reg bit[0].
                // pushf/popf were previously wrapping the `bt` here to "protect" x86 EFLAGS
                // for a subsequent lahf in EmitMaterializeCpsrFromX86Flags — but that function
                // is NEVER called when rd==15 (see every call site: `if (rd != 15)`).
                // The pushf/popf therefore protected nothing and wasted ~40 cycles per taken path.
                bt(dword[rdi + offsetof(CpuState, cpsr)], 5);  // CF = CPSR.T
                Xbyak::Label align_thumb, align_done;
                jc(align_thumb);
                and_(result_reg, ~3u);  // ARM mode: 4-byte aligned, bit[0] clear
                jmp(align_done);
                L(align_thumb);
                and_(result_reg, ~1u);  // Thumb mode: 2-byte aligned, bit[0] clear
                // Do NOT or_(result_reg, 1u) — ARM spec does not set bit[0] in PC for
                // direct writes; only BX/BLX use bit[0] of Rm to signal interworking.
                L(align_done);
            }
            mov(gba(rd), result_reg);
        }
    }

    // Load guest registers with special handling for high registers and PC.
    void LoadGbaToReg(const IrNode& n, uint32_t idx, Xbyak::Reg32 out) {
        if (idx == 4) { mov(out, dword[rdi + 16]); return; }
        if (idx == 5) { mov(out, dword[rdi + 20]); return; }
        if (idx < 8) { mov(out, Xbyak::Reg32(r8d.getIdx() + idx)); }
        else if (idx == 13) { mov(out, ebp); }
        else if (idx == 14) { mov(out, r12d); }
        else if (idx == 15) {
            mov(out, n.instr_pc); 
        } else {
            mov(out, dword[rdi + (idx * 4)]);
        }
    }

    Xbyak::Reg32 GetRnReg(const IrNode& n, uint32_t rn) {
        if (rn == 4) { LoadGbaToReg(n, 4, eax); return eax; }
        if (rn == 5) { LoadGbaToReg(n, 5, eax); return eax; }
        if (rn >= 8 && rn <= 12) { LoadGbaToReg(n, rn, eax); return eax; }
        if (rn == 15) { LoadGbaToReg(n, 15, eax); return eax; }
        return gba(rn); 
    }

    Xbyak::Reg32 GetRmReg(const IrNode& n, uint32_t rm) {
        if (rm == 4) { mov(edx, dword[rdi + 16]); return edx; }
        if (rm == 5) { mov(edx, dword[rdi + 20]); return edx; }
        if (rm >= 8 && rm <= 12) { mov(edx, dword[rdi + (rm * 4)]); return edx; }
        if (rm == 15) { LoadGbaToReg(n, 15, edx); return edx; }
        return gba(rm); 
    }

    void StoreRdIfHigh(uint32_t rd, Xbyak::Reg32 result_reg) {
        if (rd >= 8 && rd <= 12) mov(dword[rdi + (rd * 4)], result_reg);
    }

    void MovRaxSymbol(BlockCacheData::X86SymbolId id) {
        const size_t off = static_cast<size_t>(getCurr() - block_host_entry_);
        mov(rax, CodeEmitter::ResolveCacheSymbol(id));
        if (capture_relocs_) {
            CodeEmitter::RelocEntry e{};
            e.offset = static_cast<uint32_t>(off + 2u);  // mov rax, imm64 immediate starts at +2
            e.type = BlockCacheData::X86RelocType::kAbs64;
            e.symbol = id;
            emitted_relocs_.push_back(e);
        }
    }

    void BeforeCall() {
        // Spill all caller-saved guest registers into CpuState so the C helper
        // can read a coherent view.  Crucially, r12d (guest LR) is saved to the
        // dedicated stack slot at kR12SaveOffset rather than into registers[14].
        //
        // Why this matters: several C helpers (SwapBankedRegisters) legitimately
        // READ AND WRITE registers[14] as part of mode-switch banking.  If we
        // spilled r12d there first, SwapBankedRegisters would bank whatever LR
        // happened to be live at the call site — potentially a stale BL return
        // address — instead of the architecturally correct value the caller has
        // already placed there explicitly.  That stale value would then be
        // restored into r12d via AfterCall, making the next BX LR branch to the
        // wrong address and causing the dispatcher to loop on block 0x374.
        //
        // Call sites that invoke SwapBankedRegisters (EmitSwi, EmitMsr,
        // EmitExceptionReturn) are responsible for writing the correct
        // architectural LR into registers[14] *before* calling BeforeCall, and
        // for reloading r12d from registers[14] *after* AfterCall.
        mov(dword[rdi + (0 * 4)], r8d);
        mov(dword[rdi + (1 * 4)], r9d);
        mov(dword[rdi + (2 * 4)], r10d);
        mov(dword[rdi + (3 * 4)], r11d);
        mov(dword[rdi + (6 * 4)], r14d);
        mov(dword[rdi + (7 * 4)], r15d);
        mov(dword[rdi + (13 * 4)], ebp);
        mov(qword[rsp + kR12SaveOffset], r12);   // guest LR → private stack slot
        mov(dword[rdi + (15 * 4)], ebx);
    }

    void AfterCall() {
        // Restore rdi (clobbered by the SysV call ABI) and all spilled guest
        // registers.  r12d (guest LR) comes from the private stack slot written
        // by BeforeCall, not from registers[14].  This guarantees r12d is
        // unchanged by whatever the C helper did to registers[14] (e.g. the
        // mode-switch performed by SwapBankedRegisters).  Call sites that need
        // the post-swap LR value reload r12d from registers[14] themselves.
        mov(rdi, r13);
        mov(r8d, dword[rdi + (0 * 4)]);
        mov(r9d, dword[rdi + (1 * 4)]);
        mov(r10d, dword[rdi + (2 * 4)]);
        mov(r11d, dword[rdi + (3 * 4)]);
        mov(r14d, dword[rdi + (6 * 4)]);
        mov(r15d, dword[rdi + (7 * 4)]);
        mov(ebp, dword[rdi + (13 * 4)]);
        mov(r12, qword[rsp + kR12SaveOffset]);   // guest LR ← private stack slot
        mov(ebx, dword[rdi + (15 * 4)]);
    }

    void EmitLazySave(LazyOp op, Xbyak::Reg32 res_reg, Xbyak::Reg32 op1_reg, Xbyak::Reg32 op2_reg) {
        mov(dword[rdi + offsetof(CpuState, flag_op)], static_cast<uint32_t>(op));
        mov(dword[rdi + offsetof(CpuState, flag_res)], res_reg);
        mov(dword[rdi + offsetof(CpuState, flag_op1)], op1_reg);
        mov(dword[rdi + offsetof(CpuState, flag_op2)], op2_reg);
    }

    void EmitLazySave(LazyOp op, Xbyak::Reg32 res_reg, Xbyak::Reg32 op1_reg, uint32_t op2_imm) {
        mov(dword[rdi + offsetof(CpuState, flag_op)], static_cast<uint32_t>(op));
        mov(dword[rdi + offsetof(CpuState, flag_res)], res_reg);
        mov(dword[rdi + offsetof(CpuState, flag_op1)], op1_reg);
        mov(dword[rdi + offsetof(CpuState, flag_op2)], op2_imm);
    }

    void EmitLazySave(LazyOp op, Xbyak::Reg32 res_reg, uint32_t op1_imm, Xbyak::Reg32 op2_reg) {
        mov(dword[rdi + offsetof(CpuState, flag_op)], static_cast<uint32_t>(op));
        mov(dword[rdi + offsetof(CpuState, flag_res)], res_reg);
        mov(dword[rdi + offsetof(CpuState, flag_op1)], op1_imm);
        mov(dword[rdi + offsetof(CpuState, flag_op2)], op2_reg);
    }

    void EmitEarlyExitIfIOStore(uint32_t next_pc) {
        if (next_pc <= 1) return;  // 0 = not set, 1 = suppressed (STM stores)
        Xbyak::Label no_early_exit;
        cmp(ecx, 0x04000200u);
        jb(no_early_exit);
        cmp(ecx, 0x0400020Au);
        ja(no_early_exit);
        mov(ebx, next_pc);
        jmp(*current_exit_label_);
        L(no_early_exit);
    }

    void EmitCondFromCpsr(uint32_t cond, Xbyak::Label& skip) {
        mov(eax, dword[rdi + offsetof(CpuState, cpsr)]);
        Xbyak::Label fall_through;
        switch (cond) {
            case 0x0: test(eax, 1u << 30); je(skip); break;   
            case 0x1: test(eax, 1u << 30); jne(skip); break;  
            case 0x2: test(eax, 1u << 29); je(skip); break;     
            case 0x3: test(eax, 1u << 29); jne(skip); break;  
            case 0x4: test(eax, 1u << 31); je(skip); break;    
            case 0x5: test(eax, 1u << 31); jne(skip); break;  
            case 0x6: test(eax, 1u << 28); je(skip); break;    
            case 0x7: test(eax, 1u << 28); jne(skip); break;  
            case 0x8: mov(edx, eax); and_(edx, (1u << 29) | (1u << 30)); cmp(edx, 1u << 29); jne(skip); break; 
            case 0x9: test(eax, 1u << 30); jne(fall_through); test(eax, 1u << 29); jne(skip); L(fall_through); break; 
            case 0xA: mov(edx, eax); shr(edx, 28); mov(ecx, edx); shr(ecx, 3); xor_(edx, ecx); test(edx, 1); jne(skip); break; 
            case 0xB: mov(edx, eax); shr(edx, 28); mov(ecx, edx); shr(ecx, 3); xor_(edx, ecx); test(edx, 1); je(skip); break; 
            case 0xC: test(eax, 1u << 30); jne(skip); mov(edx, eax); shr(edx, 28); mov(ecx, edx); shr(ecx, 3); xor_(edx, ecx); test(edx, 1); jne(skip); break; 
            case 0xD: test(eax, 1u << 30); jne(fall_through); mov(edx, eax); shr(edx, 28); mov(ecx, edx); shr(ecx, 3); xor_(edx, ecx); test(edx, 1); je(skip); L(fall_through); break; 
            case 0xE: break;
            case 0xF: jmp(skip); break;
            default: break;
        }
    }

    void EmitShift(const IrNode& n, Xbyak::Reg32 dst, Xbyak::Reg32 src) {
        if (dst.getIdx() != src.getIdx()) mov(dst, src);
        uint32_t shift_imm = n.fields.shift_imm & 0x1F;

        if (n.fields.shift_by_reg) {
            LoadGbaToReg(n, static_cast<uint32_t>(n.immediate), edx);
            mov(ecx, edx);

            Xbyak::Label skip_shift, end_shift;
            test(cl, cl);
            je(skip_shift); 

            switch (n.fields.shift_type & 3u) {
                case 0: { // LSL by register (shift amount = Rs[7:0])
                    Xbyak::Label lt32, eq32;
                    cmp(cl, 32);
                    jb(lt32);
                    je(eq32);
                    // >32: result = 0, carry = 0
                    xor_(dst, dst);
                    jmp(end_shift);
                    L(eq32);
                    // ==32: result = 0, carry = bit0 of original
                    bt(dst, 0);
                    mov(dst, 0);
                    jmp(end_shift);
                    L(lt32);
                    shl(dst, cl);
                    jmp(end_shift);
                }
                case 1: { // LSR by register
                    Xbyak::Label lt32, eq32;
                    cmp(cl, 32);
                    jb(lt32);
                    je(eq32);
                    // >32: result = 0, carry = 0
                    xor_(dst, dst);
                    jmp(end_shift);
                    L(eq32);
                    // ==32: result = 0, carry = bit31 of original
                    bt(dst, 31);
                    mov(dst, 0);
                    jmp(end_shift);
                    L(lt32);
                    shr(dst, cl);
                    jmp(end_shift);
                }
                case 2: { // ASR by register
                    Xbyak::Label lt32;
                    cmp(cl, 32);
                    jb(lt32);
                    // >=32: result = sign-extend, carry = bit31 of original
                    bt(dst, 31);
                    setc(sil);
                    sar(dst, 31);
                    movzx(esi, sil);
                    bt(esi, 0);
                    jmp(end_shift);
                    L(lt32);
                    sar(dst, cl);
                    jmp(end_shift);
                }
                case 3: { // ROR by register (rotate amount = Rs[7:0] mod 32)
                    Xbyak::Label do_ror;
                    and_(ecx, 31);
                    jnz(do_ror);
                    // mod 32 == 0: result unchanged, carry = bit31 of result (original bit31)
                    bt(dst, 31);
                    jmp(end_shift);
                    L(do_ror);
                    ror(dst, cl);
                    jmp(end_shift);
                }
            }

            L(skip_shift); 
            bt(dword[rdi + offsetof(CpuState, cpsr)], 29); 

            L(end_shift);

        } else if (shift_imm != 0) {
            switch (n.fields.shift_type) {
                case 0: shl(dst, shift_imm); break;
                case 1: shr(dst, shift_imm); break;
                case 2: sar(dst, shift_imm); break;
                case 3: ror(dst, shift_imm); break;
            }
        } else {
            switch (n.fields.shift_type) {
                case 0:
                    bt(dword[rdi + offsetof(CpuState, cpsr)], 29);
                    break;
                case 1:
                    bt(dst, 31);
                    mov(dst, 0);
                    break;
                case 2:
                    // ASR #0 encodes ASR #32: result = sign-extend, carry = bit31 of original
                    bt(dst, 31);
                    setc(sil);
                    sar(dst, 31);
                    movzx(esi, sil);
                    bt(esi, 0);
                    break;
                case 3: 
                    bt(dword[rdi + offsetof(CpuState, cpsr)], 29);
                    rcr(dst, 1);
                    break;
            }
        }
    }

    void EmitExceptionReturn() {
        Xbyak::Label skip_exc_ret_swap;
        mov(rdi, r13); 
        mov(eax, dword[rdi + offsetof(CpuState, spsr)]);
        mov(dword[rsp + 48], eax); 
        mov(esi, dword[rdi + offsetof(CpuState, cpsr)]);
        and_(esi, 0x1Fu);   
        mov(edx, eax);
        and_(edx, 0x1Fu);   
        cmp(esi, edx);
        je(skip_exc_ret_swap);

        mov(dword[r13 + offsetof(CpuState, registers) + 13 * 4], ebp);
        mov(dword[r13 + offsetof(CpuState, registers) + 14 * 4], r12d);

        BeforeCall();
        MovRaxSymbol(BlockCacheData::X86SymbolId::kSwapBankedRegisters);
        call(rax);
        AfterCall();

        mov(ebp, dword[r13 + offsetof(CpuState, registers) + 13 * 4]);
        mov(r12d, dword[r13 + offsetof(CpuState, registers) + 14 * 4]);
        L(skip_exc_ret_swap);

        mov(ecx, dword[rsp + 48]); 
        mov(dword[r13 + offsetof(CpuState, cpsr)], ecx);
        mov(dword[r13 + offsetof(CpuState, flag_op)], static_cast<uint32_t>(kLazyNone));
    }

    void EmitMaterializeCpsrFromX86Flags() {
        lahf();
        movzx(eax, ah);
        seto(dl);
        mov(ecx, dword[rdi + offsetof(CpuState, cpsr)]);
        and_(ecx, 0x0FFFFFFFu);
        mov(esi, eax);
        and_(esi, 0x80u);
        shl(esi, 24);
        or_(ecx, esi);
        mov(esi, eax);
        and_(esi, 0x40u);
        shl(esi, 24);
        or_(ecx, esi);
        // For SUB/SBB, x86 CF=borrow, while ARM CPSR.C is NOT-borrow.
        // Use the captured LAHF CF (AH bit0) so we don't depend on current flags.
        mov(esi, eax);
        and_(esi, 1u);
        xor_(esi, 1u);
        shl(esi, 29);
        or_(ecx, esi);
        movzx(esi, dl);
        shl(esi, 28);
        or_(ecx, esi);
        mov(dword[rdi + offsetof(CpuState, cpsr)], ecx);
        mov(dword[rdi + offsetof(CpuState, flag_op)], static_cast<uint32_t>(kLazyNone));
    }

    static constexpr uint32_t kEwramPageLo = 0x800;
    static constexpr uint32_t kEwramPageHi = 0x8FF;
    static constexpr uint32_t kIwramPageLo = 0xC00;
    static constexpr uint32_t kIwramPageHi = 0xFFF;
    static constexpr uint32_t kVramPageLo  = 0x1800;
    static constexpr uint32_t kVramPageHi  = 0x1BFF;
    static constexpr uint32_t kRomPageLo   = 0x2000;
    static constexpr uint32_t kRomPageHi   = 0x37FF;

    template <bool Load, int Bits, bool IsSigned = false, bool IsUserBank = false>
    void EmitMemGeneric(const IrNode& n) {
        LoadGbaToReg(n, n.fields.rn, ecx);
        if (n.fields.use_pool) {
            add(ecx, static_cast<int32_t>(n.immediate));
        } else {
            LoadGbaToReg(n, n.fields.rm, esi);
            if (n.fields.shift_by_reg) {
                push(rcx);
                EmitShift(n, esi, esi);
                pop(rcx);
            } else {
                EmitShift(n, esi, esi);
            }
            if (n.fields.u_bit) add(ecx, esi);
            else sub(ecx, esi);
        }
        mov(esi, ecx);  
        mov(eax, ecx);  
        shr(rax, 14);   

        Xbyak::Label fast_path, fast_path_load_pt, fast_path_check_rom, slow_path, done, skip_slow_store_call;
#if LOGO_LINK_FORCE_SLOW_STORE
        // Optional diagnostic guardrail: force helper path for known logo-link
        // fields, but only for stores.
        if constexpr (!Load) {
            cmp(ecx, 0x03003BB0u); je(slow_path);
            cmp(ecx, 0x03003BF0u); je(slow_path);
            cmp(ecx, 0x03003C30u); je(slow_path);
            cmp(ecx, 0x03003C70u); je(slow_path);
            cmp(ecx, 0x03003CB0u); je(slow_path);
            cmp(ecx, 0x03003CF0u); je(slow_path);
            cmp(ecx, 0x0300374Cu); je(slow_path);
            cmp(ecx, 0x0300379Cu); je(slow_path);
            cmp(ecx, 0x030037ECu); je(slow_path);
            cmp(ecx, 0x0300383Cu); je(slow_path);
            cmp(ecx, 0x0300388Cu); je(slow_path);
            cmp(ecx, 0x030038DCu); je(slow_path);
        }
#endif
#if IRQ_WAITWORD_FORCE_SLOW_MEM
        // Diagnostic: force helper path for IRQ bookkeeping words.
        cmp(ecx, 0x0300310Cu); je(slow_path);  // IntrWait/VBlankIntrWait wait-word
        cmp(ecx, 0x03007FF8u); je(slow_path);  // BIOS IRQ check flags
        cmp(ecx, 0x03007FFCu); je(slow_path);  // User IRQ handler pointer
#endif
        cmp(rax, kEwramPageLo);
        jb(slow_path);
        cmp(rax, kEwramPageHi);
        jbe(fast_path_load_pt);

        cmp(rax, kIwramPageLo);
        jb(slow_path);
        cmp(rax, kIwramPageHi);
        jbe(fast_path_load_pt);

        if constexpr (Load) {
            cmp(rax, kVramPageLo);
            jb(fast_path_check_rom);
            cmp(rax, kVramPageHi);
            jbe(fast_path_load_pt);
            L(fast_path_check_rom);

            cmp(rax, kRomPageLo);
            jb(slow_path);
            cmp(rax, kRomPageHi);
            jbe(fast_path_load_pt);
        }
        jmp(slow_path);

        L(fast_path_load_pt);
        mov(rdx, qword[rdi + offsetof(CpuState, page_table_ptr)]);
        mov(rdx, qword[rdx + rax * 8]);
        test(rdx, rdx);
        je(slow_path);

        L(fast_path);
        const uint32_t rd = n.fields.rd;
        auto enforce_pc_alignment = [&]() {
            if (rd == 15) {
                mov(edx, dword[rdi + offsetof(CpuState, cpsr)]);
                test(edx, 1u << 5);  
                Xbyak::Label align_thumb, align_done;
                jne(align_thumb);
                and_(eax, ~3u);  
                jmp(align_done);
                L(align_thumb);
                and_(eax, ~1u);  
                L(align_done);
            }
        };

        if constexpr (Load) {
            if constexpr (Bits == 32) {
                mov(esi, ecx);
                and_(esi, 3u);
                shl(esi, 3);  
                and_(ecx, ~3u);
                mov(eax, dword[rdx + rcx]);
                mov(ecx, esi);
                ror(eax, cl);
            } else if constexpr (Bits == 16) {
                // GBATEK: misaligned LDR uses rotated 32-bit read from (addr & ~3)
                // then ROR by (addr & 3) * 8 bits. LDRH/LDRSH/LDRB/LDRSB internally
                // use that same rotated word and then mask/sign-extend.
                mov(esi, ecx);
                and_(esi, 3u);
                shl(esi, 3);  
                and_(ecx, ~3u);
                mov(eax, dword[rdx + rcx]);
                mov(ecx, esi);
                ror(eax, cl);
                if constexpr (IsSigned) {
                    movsx(eax, ax);
                } else {
                    and_(eax, 0xFFFFu);
                }
            } else {
                if constexpr (IsSigned) movsx(eax, byte[rdx + rcx]);
                else movzx(eax, byte[rdx + rcx]);
            }

            if constexpr (IsUserBank) {
                push(rcx);  
                push(rax);  
                sub(rsp, 8);  
                BeforeCall();
                MovRaxSymbol(BlockCacheData::X86SymbolId::kJitSetUserReg);
                mov(rsi, rd);  
                mov(rdx, qword[rsp + 8]);  
                call(rax);
                AfterCall();
                add(rsp, 8);
                pop(rax);
                pop(rcx);
            } else {
                enforce_pc_alignment();
                StoreRd(rd, eax);
            }
        } else {
            if constexpr (IsUserBank) {
                push(rcx);  
                push(rdx);  
                BeforeCall();
                MovRaxSymbol(BlockCacheData::X86SymbolId::kJitGetUserReg);
                mov(rsi, rd);
                call(rax);  
                AfterCall();
                pop(rdx);
                pop(rcx);
            } else {
                LoadGbaToReg(n, rd, eax);
            }

            if constexpr (Bits == 32) {
                and_(ecx, ~3u);
                mov(dword[rdx + rcx], eax);
            } else if constexpr (Bits == 16) {
                and_(ecx, ~1u);
                mov(word[rdx + rcx], ax);
            } else {
                mov(byte[rdx + rcx], al);
            }
            EmitEarlyExitIfIOStore(n.link_value);
        }
        jmp(done);

        L(slow_path);
        if constexpr (Load) {
            size_t helper_offset = Bits == 32 ? offsetof(CpuState, read32_helper)
                : Bits == 16 ? offsetof(CpuState, read16_helper) : offsetof(CpuState, read8_helper);
            mov(rax, qword[rdi + helper_offset]);
            test(rax, rax);
            je(done);
            BeforeCall();
            call(rax);
            AfterCall();
            if constexpr (Bits < 32) {
                if constexpr (IsSigned) {
                    if constexpr (Bits == 16) movsx(eax, ax);
                    else movsx(eax, al);
                } else {
                    if constexpr (Bits == 16) movzx(eax, ax);
                    else movzx(eax, al);
                }
            }

            if constexpr (IsUserBank) {
                push(rcx);
                push(rax);
                sub(rsp, 8);  
                BeforeCall();
                MovRaxSymbol(BlockCacheData::X86SymbolId::kJitSetUserReg);
                mov(rsi, rd);
                mov(rdx, qword[rsp + 8]);
                call(rax);
                AfterCall();
                add(rsp, 8);
                pop(rax);
                pop(rcx);
            } else {
                enforce_pc_alignment();
                StoreRd(rd, eax);
            }
        } else {
            push(rcx);  
            sub(rsp, 8);  

            if constexpr (IsUserBank) {
                BeforeCall();
                MovRaxSymbol(BlockCacheData::X86SymbolId::kJitGetUserReg);
                mov(rsi, rd);
                call(rax);
                AfterCall();
                mov(edx, eax);  
                mov(rsi, qword[rsp + 8]);  
            } else {
                LoadGbaToReg(n, rd, edx);
            }

            size_t helper_offset = Bits == 32 ? offsetof(CpuState, write32_helper)
                : Bits == 16 ? offsetof(CpuState, write16_helper) : offsetof(CpuState, write8_helper);
            mov(rax, qword[rdi + helper_offset]);
            test(rax, rax);
            je(skip_slow_store_call);

            BeforeCall();
            call(rax);
            AfterCall();

            L(skip_slow_store_call);
            add(rsp, 8);
            pop(rcx);
            EmitEarlyExitIfIOStore(n.link_value);
        }
        L(done);
    }

    template <bool Load, int Bits>
    void EmitFastmem(const IrNode& n) {
        LoadGbaToReg(n, n.fields.rn, ecx);
        if (n.fields.use_pool) {
            add(ecx, static_cast<int32_t>(n.immediate));
        } else {
            LoadGbaToReg(n, n.fields.rm, esi);
            if (n.fields.shift_by_reg) {
                push(rcx);
                EmitShift(n, esi, esi);
                pop(rcx);
            } else {
                EmitShift(n, esi, esi);
            }
            if (n.fields.u_bit) {
                add(ecx, esi);
            } else {
                sub(ecx, esi);
            }
        }
        mov(eax, ecx);  
        shr(rax, 14);
        mov(rdx, qword[rdi + offsetof(CpuState, page_table_ptr)]);
        mov(rdx, qword[rdx + rax * 8]);
        const uint32_t rd = n.fields.rd;
        auto enforce_pc_alignment_fast = [&]() {
            if (rd == 15) {
                mov(edx, dword[rdi + offsetof(CpuState, cpsr)]);
                test(edx, 1u << 5);  
                Xbyak::Label align_thumb_fast, align_done_fast;
                jne(align_thumb_fast);
                and_(eax, ~3u);  
                jmp(align_done_fast);
                L(align_thumb_fast);
                and_(eax, ~1u);  
                L(align_done_fast);
            }
        };
        
        if constexpr (Load) {
            if constexpr (Bits == 32) {
                mov(esi, ecx);
                and_(esi, 3u);
                shl(esi, 3);  
                and_(ecx, ~3u);
                mov(eax, dword[rdx + rcx]);
                mov(ecx, esi);
                ror(eax, cl);
                enforce_pc_alignment_fast();
                StoreRd(rd, eax);
            } else if constexpr (Bits == 16) {
                // Match generic path: misaligned halfword loads are derived from
                // a rotated 32-bit word, then masked/zero-extended.
                mov(esi, ecx);
                and_(esi, 3u);
                shl(esi, 3);  
                and_(ecx, ~3u);
                mov(eax, dword[rdx + rcx]);
                mov(ecx, esi);
                ror(eax, cl);
                and_(eax, 0xFFFFu);
                enforce_pc_alignment_fast();
                StoreRd(rd, eax);
            } else if constexpr (Bits == 8) {
                movzx(eax, byte[rdx + rcx]);
                enforce_pc_alignment_fast();
                StoreRd(rd, eax);
            }
        } else {
            if constexpr (Bits == 32) {
                LoadGbaToReg(n, rd, esi);
                mov(dword[rdx + rcx], esi);
            } else if constexpr (Bits == 16) {
                and_(ecx, ~1u);  
                LoadGbaToReg(n, rd, esi);
                mov(word[rdx + rcx], si);
            } else if constexpr (Bits == 8) {
                LoadGbaToReg(n, rd, esi);
                mov(byte[rdx + rcx], sil);
            }
            EmitEarlyExitIfIOStore(n.link_value);
        }
    }

    // Materialize flags before instructions that require precise CPSR bits.
    void TranslateNode(const IrNode& n) {
        bool needs_materialize = false;
        if (n.fields.cond != 0xE) {
            needs_materialize = true;
        } else {
            const IrOp ir_op = static_cast<IrOp>(n.fields.opcode);
            const bool is_logical_flag_setter = n.fields.set_flags && (
                ir_op == IrOp::kAnd || ir_op == IrOp::kEor || ir_op == IrOp::kOrr ||
                ir_op == IrOp::kBic || ir_op == IrOp::kMov || ir_op == IrOp::kMvn ||
                ir_op == IrOp::kTst || ir_op == IrOp::kTeq || ir_op == IrOp::kMul ||
                ir_op == IrOp::kUmull || ir_op == IrOp::kUmlal || ir_op == IrOp::kSmull || ir_op == IrOp::kSmlal
            );
            const bool is_rrx = (n.fields.shift_type == 3 && n.fields.shift_imm == 0 && !n.fields.shift_by_reg);
            if (is_logical_flag_setter || is_rrx) {
                needs_materialize = true;
            }
        }

        Xbyak::Label skip;
        if (needs_materialize) {
            Xbyak::Label skip_materialize;
            cmp(dword[rdi + offsetof(CpuState, flag_op)], static_cast<uint32_t>(kLazyNone));
            je(skip_materialize);

            MovRaxSymbol(BlockCacheData::X86SymbolId::kMaterializeCpsr);
            BeforeCall();
            call(rax);
            AfterCall();

            L(skip_materialize);
        }

        if (n.fields.cond != 0xE) {
            EmitCondFromCpsr(n.fields.cond, skip);
        }

        const uint32_t op = static_cast<uint32_t>(n.fields.opcode);
        const uint32_t use_pool = n.fields.use_pool ? 1u : 0u;
        const uint32_t set_flags = n.fields.set_flags ? 1u : 0u;
        EmitterFunc func = g_dispatch_table[op][use_pool][set_flags];
        if (func) func(this, n);

        if (n.fields.cond != 0xE) {
            L(skip);
            const IrOp opcode = static_cast<IrOp>(op);
            if ((opcode == IrOp::kBranch || opcode == IrOp::kCall ||
                 opcode == IrOp::kBranchExchange || opcode == IrOp::kSwi ||
                 n.fields.rd == 15) && n.link_value != 0) {
                mov(ebx, static_cast<uint32_t>(n.link_value));
            }
        }
    }

    void* EmitBlockInternal(ArenaAllocator* arena, uint32_t pc, uint32_t block_cycles, bool capture_relocs) {
        align(16);
        void* host_entry = const_cast<uint8_t*>(getCurr());
        block_pc_ = pc;
        block_host_entry_ = static_cast<const uint8_t*>(host_entry);
        capture_relocs_ = capture_relocs;
        emitted_relocs_.clear();

        Xbyak::Label block_exit;
        current_exit_label_ = &block_exit;

        sub(rsp, kFrameSize);
        mov(qword[rsp + 0], rbx);
        mov(qword[rsp + 8], rbp);
        mov(qword[rsp + 16], r12);
        mov(qword[rsp + 24], r13);
        mov(qword[rsp + 32], r14);
        mov(qword[rsp + 40], r15);
        mov(r13, rdi);  

        mov(r8d, dword[rdi + (0 * 4)]);
        mov(r9d, dword[rdi + (1 * 4)]);
        mov(r10d, dword[rdi + (2 * 4)]);
        mov(r11d, dword[rdi + (3 * 4)]);
        mov(r14d, dword[rdi + (6 * 4)]);
        mov(r15d, dword[rdi + (7 * 4)]);
        mov(ebx, dword[rdi + (15 * 4)]);
        mov(ebp, dword[rdi + (13 * 4)]);
        mov(r12d, dword[rdi + (14 * 4)]);

        IrNode* nodes = static_cast<IrNode*>(arena->GetBasePointer());
        size_t count = arena->BytesUsed() / sizeof(IrNode);
        
        for (size_t i = 0; i < count; ++i) TranslateNode(nodes[i]);

        L(block_exit);

        if (block_cycles > 0) {
            add(dword[r13 + offsetof(CpuState, cycle_counter)],
                static_cast<uint32_t>(block_cycles));
        }

        {
            Xbyak::Label skip_io_check;
            cmp(ebx, 0x04000000u);
            jb(skip_io_check);
            cmp(ebx, 0x04000400u);
            jae(skip_io_check);
            mov(edi, ebx);                                    
            mov(esi, static_cast<uint32_t>(block_pc_));       
            MovRaxSymbol(BlockCacheData::X86SymbolId::kJitCrashJumpToIo);
            call(rax);
            L(skip_io_check);
        }

        mov(dword[rdi + (0 * 4)], r8d);
        mov(dword[rdi + (1 * 4)], r9d);
        mov(dword[rdi + (2 * 4)], r10d);
        mov(dword[rdi + (3 * 4)], r11d);
        mov(dword[rdi + (6 * 4)], r14d);
        mov(dword[rdi + (7 * 4)], r15d);
        mov(dword[rdi + (15 * 4)], ebx);
        mov(dword[rdi + (13 * 4)], ebp);
        mov(dword[rdi + (14 * 4)], r12d);

        mov(rbx, qword[rsp + 0]);
        mov(rbp, qword[rsp + 8]);
        mov(r12, qword[rsp + 16]);
        mov(r13, qword[rsp + 24]);
        mov(r14, qword[rsp + 32]);
        mov(r15, qword[rsp + 40]);
        add(rsp, kFrameSize);
        ret();

        current_exit_label_ = nullptr;
        last_emitted_size_ = static_cast<size_t>(getCurr() - static_cast<const uint8_t*>(host_entry));
        capture_relocs_ = false;
        return host_entry;
    }

    void* EmitBlock(ArenaAllocator* arena, uint32_t pc, uint32_t block_cycles) {
        return EmitBlockInternal(arena, pc, block_cycles, false);
    }

    CodeEmitter::EmittedBlockArtifact EmitBlockWithRelocs(ArenaAllocator* arena,
                                                          uint32_t pc,
                                                          uint32_t block_cycles,
                                                          bool is_thumb,
                                                          uint32_t block_len) {
        CodeEmitter::EmittedBlockArtifact art{};
        art.pc = pc;
        art.is_thumb = is_thumb;
        art.block_cycles = block_cycles;
        art.block_len = block_len;
        art.host_code = EmitBlockInternal(arena, pc, block_cycles, true);
        art.x86_size = last_emitted_size_;
        art.relocs = emitted_relocs_;
        const uint8_t* p = static_cast<const uint8_t*>(art.host_code);
        art.x86_bytes.assign(p, p + art.x86_size);
        art.x86_crc32 = Crc32(art.x86_bytes.data(), art.x86_bytes.size());
        return art;
    }

    size_t last_emitted_size_{0};
};

CodeEmitter::Impl::EmitterFunc CodeEmitter::Impl::g_dispatch_table[CodeEmitter::Impl::kOpCount][2][2];

template <IrOp Op, bool UsePool, bool SetFlags>
void CodeEmitter::Impl::EmitALU(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rn = n.fields.rn, rm = n.fields.rm;

    if constexpr (Op == IrOp::kAdc || Op == IrOp::kSbc || Op == IrOp::kRsc) {
        jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kMaterializeCpsr);
        jit->BeforeCall();
        jit->call(jit->rax);
        jit->AfterCall();
        jit->bt(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], 29u);

        auto rd_reg = jit->gba(rd);
        auto rn_reg = jit->GetRnReg(n, rn);

        if constexpr (Op == IrOp::kAdc) {
            if (rd >= 8 && rd <= 12) {
                if constexpr (SetFlags) jit->mov(jit->ecx, rn_reg); 
                jit->mov(jit->eax, rn_reg);
                if constexpr (UsePool) jit->adc(jit->eax, static_cast<uint32_t>(n.immediate));
                else jit->adc(jit->eax, jit->GetRmReg(n, rm));

                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, jit->eax);
                if constexpr (SetFlags) {
                    if (rd != 15) {
                        if constexpr (UsePool) jit->EmitLazySave(kLazyAdd, jit->eax, jit->ecx, static_cast<uint32_t>(n.immediate));
                        else jit->EmitLazySave(kLazyAdd, jit->eax, jit->ecx, jit->GetRmReg(n, rm));
                    }
                
                }
            } else {

                if constexpr (SetFlags) jit->mov(jit->ecx, rn_reg);
                jit->mov(rd_reg, rn_reg);
                if constexpr (UsePool) jit->adc(rd_reg, static_cast<uint32_t>(n.immediate));
                else jit->adc(rd_reg, jit->GetRmReg(n, rm));
                
                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, rd_reg);
                if constexpr (SetFlags) {
                    if (rd != 15) {
                        if constexpr (UsePool) jit->EmitLazySave(kLazyAdd, rd_reg, jit->ecx, static_cast<uint32_t>(n.immediate));
                        else jit->EmitLazySave(kLazyAdd, rd_reg, jit->ecx, jit->GetRmReg(n, rm));
                    }
                }
            }
        } else if constexpr (Op == IrOp::kSbc) {
            jit->cmc();
            if (rd >= 8 && rd <= 12) {
                jit->mov(jit->eax, rn_reg);
                if constexpr (UsePool) jit->sbb(jit->eax, static_cast<uint32_t>(n.immediate));
                else jit->sbb(jit->eax, jit->GetRmReg(n, rm));
                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, jit->eax);
                if constexpr (SetFlags) { if (rd != 15) jit->EmitMaterializeCpsrFromX86Flags(); }
            } else {
                jit->mov(rd_reg, rn_reg);
                if constexpr (UsePool) jit->sbb(rd_reg, static_cast<uint32_t>(n.immediate));
                else jit->sbb(rd_reg, jit->GetRmReg(n, rm));
                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, rd_reg);
                if constexpr (SetFlags) { if (rd != 15) jit->EmitMaterializeCpsrFromX86Flags(); }
            }
        } else {
            jit->cmc();
            if (rd >= 8 && rd <= 12) {
                if constexpr (UsePool) {
                    jit->mov(jit->eax, static_cast<uint32_t>(n.immediate));
                    jit->sbb(jit->eax, rn_reg);
                } else {
                    jit->mov(jit->eax, jit->GetRmReg(n, rm));
                    jit->sbb(jit->eax, rn_reg);
                }
                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, jit->eax);
                if constexpr (SetFlags) { if (rd != 15) jit->EmitMaterializeCpsrFromX86Flags(); }
            } else {
                if constexpr (UsePool) {
                    jit->mov(rd_reg, static_cast<uint32_t>(n.immediate));
                    jit->sbb(rd_reg, rn_reg);
                } else {
                    jit->mov(rd_reg, jit->GetRmReg(n, rm));
                    jit->sbb(rd_reg, rn_reg);
                }
                if (rd == 15 && SetFlags) jit->EmitExceptionReturn();
                jit->StoreRd(rd, rd_reg);
                if constexpr (SetFlags) { if (rd != 15) jit->EmitMaterializeCpsrFromX86Flags(); }
            }
        }
        return;
    }

    auto rd_reg = jit->gba(rd);
    auto rn_reg = jit->GetRnReg(n, rn);

    if constexpr (UsePool) {
        uint32_t imm = static_cast<uint32_t>(n.immediate);
        if constexpr (Op == IrOp::kAdd) {
            if (rn == 15) {
                if (rd >= 8 && rd <= 12) {
                    jit->mov(jit->eax, imm);
                    if constexpr (SetFlags) {
                        if (rd == 15) jit->EmitExceptionReturn();
                        else jit->EmitLazySave(kLazyAdd, jit->eax, jit->eax, 0u);
                    }
                    jit->StoreRd(rd, jit->eax);
                } else if (rd == 4) {
                    jit->mov(jit->eax, imm);
                    if constexpr (SetFlags) jit->EmitLazySave(kLazyAdd, jit->eax, jit->eax, 0u);
                    jit->mov(jit->dword[jit->rdi + 16], jit->eax);
                } else {
                    jit->mov(rd_reg, imm);
                    if constexpr (SetFlags) {
                        if (rd == 15) jit->EmitExceptionReturn();
                        else jit->EmitLazySave(kLazyAdd, rd_reg, rd_reg, 0u);
                    }
                    jit->StoreRd(rd, rd_reg);
                }
                return;
            }
        }
        if (rd >= 8 && rd <= 12) {
            if constexpr (SetFlags) jit->mov(jit->ecx, rn_reg); 
            jit->mov(jit->eax, rn_reg);
            if constexpr (Op == IrOp::kAdd) jit->add(jit->eax, imm);
            else if constexpr (Op == IrOp::kSub) jit->sub(jit->eax, imm);
            else if constexpr (Op == IrOp::kAnd) jit->and_(jit->eax, imm);
            else if constexpr (Op == IrOp::kOrr) jit->or_(jit->eax, imm);
            else if constexpr (Op == IrOp::kEor) jit->xor_(jit->eax, imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if constexpr (Op == IrOp::kAnd || Op == IrOp::kOrr || Op == IrOp::kEor) {
                        if (imm > 0xFF) {
                            uint32_t carry = (imm >> 31) & 1;
                            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                            jit->and_(jit->edx, ~(1u << 29));
                            if (carry) jit->or_(jit->edx, (1u << 29));
                            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                        }
                    }
                    constexpr LazyOp lazy = (Op == IrOp::kAdd) ? kLazyAdd : (Op == IrOp::kSub) ? kLazySub : kLazyLogic;
                    jit->EmitLazySave(lazy, jit->eax, jit->ecx, imm);
                }
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            if constexpr (SetFlags) jit->mov(jit->ecx, rn_reg); 
            jit->mov(rd_reg, rn_reg);
            if constexpr (Op == IrOp::kAdd) jit->add(rd_reg, imm);
            else if constexpr (Op == IrOp::kSub) jit->sub(rd_reg, imm);
            else if constexpr (Op == IrOp::kAnd) jit->and_(rd_reg, imm);
            else if constexpr (Op == IrOp::kOrr) jit->or_(rd_reg, imm);
            else if constexpr (Op == IrOp::kEor) jit->xor_(rd_reg, imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if constexpr (Op == IrOp::kAnd || Op == IrOp::kOrr || Op == IrOp::kEor) {
                        if (imm > 0xFF) {
                            uint32_t carry = (imm >> 31) & 1;
                            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                            jit->and_(jit->edx, ~(1u << 29));
                            if (carry) jit->or_(jit->edx, (1u << 29));
                            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                        }
                    }
                    constexpr LazyOp lazy = (Op == IrOp::kAdd) ? kLazyAdd : (Op == IrOp::kSub) ? kLazySub : kLazyLogic;
                    jit->EmitLazySave(lazy, rd_reg, jit->ecx, imm);
                }
            }
            jit->StoreRd(rd, rd_reg);
        }
    } else {
        if (n.fields.shift_by_reg) {
            jit->mov(jit->dword[jit->rsp + 48], rn_reg);
        } else {
            jit->mov(jit->ecx, rn_reg);
            rn_reg = jit->ecx;
        }

        auto rm_raw = jit->GetRmReg(n, rm);
        jit->mov(jit->eax, rm_raw);

        if constexpr (SetFlags && (Op == IrOp::kAnd || Op == IrOp::kOrr || Op == IrOp::kEor)) {
            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->bt(jit->edx, 29);
        }

        jit->EmitShift(n, jit->eax, jit->eax);

        if constexpr (SetFlags && (Op == IrOp::kAnd || Op == IrOp::kOrr || Op == IrOp::kEor)) {
            jit->setc(jit->sil); 
            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->and_(jit->edx, ~(1u << 29));
            jit->movzx(jit->esi, jit->sil);
            jit->shl(jit->esi, 29);
            jit->or_(jit->edx, jit->esi);
            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
        }

        auto rm_shifted = jit->eax;

        if (n.fields.shift_by_reg) {
            jit->mov(jit->ecx, jit->dword[jit->rsp + 48]);
            rn_reg = jit->ecx;
        }

        if (rd >= 8 && rd <= 12) {
            jit->mov(jit->edx, rn_reg);
            if constexpr (Op == IrOp::kAdd) jit->add(jit->edx, rm_shifted);
            else if constexpr (Op == IrOp::kSub) jit->sub(jit->edx, rm_shifted);
            else if constexpr (Op == IrOp::kAnd) jit->and_(jit->edx, rm_shifted);
            else if constexpr (Op == IrOp::kOrr) jit->or_(jit->edx, rm_shifted);
            else if constexpr (Op == IrOp::kEor) jit->xor_(jit->edx, rm_shifted);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    constexpr LazyOp lazy = (Op == IrOp::kAdd) ? kLazyAdd : (Op == IrOp::kSub) ? kLazySub : kLazyLogic;
                    jit->EmitLazySave(lazy, jit->edx, jit->ecx, rm_shifted);
                }
            }
            jit->StoreRd(rd, jit->edx);
        } else {
            if (rd_reg.getIdx() != jit->eax.getIdx()) {
                jit->mov(rd_reg, rn_reg);
                if constexpr (Op == IrOp::kAdd) jit->add(rd_reg, rm_shifted);
                else if constexpr (Op == IrOp::kSub) jit->sub(rd_reg, rm_shifted);
                else if constexpr (Op == IrOp::kAnd) jit->and_(rd_reg, rm_shifted);
                else if constexpr (Op == IrOp::kOrr) jit->or_(rd_reg, rm_shifted);
                else if constexpr (Op == IrOp::kEor) jit->xor_(rd_reg, rm_shifted);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else {
                        constexpr LazyOp lazy = (Op == IrOp::kAdd) ? kLazyAdd : (Op == IrOp::kSub) ? kLazySub : kLazyLogic;
                        jit->EmitLazySave(lazy, rd_reg, jit->ecx, rm_shifted);
                    }
                }
                jit->StoreRd(rd, rd_reg);
            } else {
                jit->mov(jit->edx, rn_reg);
                if constexpr (Op == IrOp::kAdd) jit->add(jit->edx, rm_shifted);
                else if constexpr (Op == IrOp::kSub) jit->sub(jit->edx, rm_shifted);
                else if constexpr (Op == IrOp::kAnd) jit->and_(jit->edx, rm_shifted);
                else if constexpr (Op == IrOp::kOrr) jit->or_(jit->edx, rm_shifted);
                else if constexpr (Op == IrOp::kEor) jit->xor_(jit->edx, rm_shifted);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else {
                        constexpr LazyOp lazy = (Op == IrOp::kAdd) ? kLazyAdd : (Op == IrOp::kSub) ? kLazySub : kLazyLogic;
                        jit->EmitLazySave(lazy, jit->edx, jit->ecx, rm_shifted);
                    }
                }
                jit->StoreRd(rd, jit->edx);
            }
        }
    }
}

template <bool UsePool, bool SetFlags>
void CodeEmitter::Impl::EmitBic(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rn = n.fields.rn, rm = n.fields.rm;
    uint32_t imm = static_cast<uint32_t>(n.immediate);
    if constexpr (UsePool) {
        auto rn_reg = jit->GetRnReg(n, rn);
        if (rd >= 8 && rd <= 12) {
            jit->mov(jit->eax, rn_reg);
            jit->and_(jit->eax, ~imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, jit->eax, jit->eax, ~imm);
                }
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            auto rd_reg = jit->gba(rd);
            if (rd_reg.getIdx() != rn_reg.getIdx()) jit->mov(rd_reg, rn_reg);
            jit->and_(rd_reg, ~imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, ~imm);
                }
            }
            jit->StoreRd(rd, rd_reg);
        }
    } else {
        if (n.fields.shift_by_reg) {
            auto rn_reg = jit->GetRnReg(n, rn);
            jit->mov(jit->dword[jit->rsp + 48], rn_reg);
        } else {
            jit->LoadGbaToReg(n, rn, jit->ecx); 
        }

        auto rm_raw = jit->GetRmReg(n, rm);
        jit->mov(jit->eax, rm_raw);

        if constexpr (SetFlags) {
            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->bt(jit->edx, 29);
        }
        jit->EmitShift(n, jit->eax, jit->eax);
        
        if constexpr (SetFlags) {
            jit->setc(jit->sil); 
            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->and_(jit->edx, ~(1u << 29));
            jit->movzx(jit->esi, jit->sil);
            jit->shl(jit->esi, 29);
            jit->or_(jit->edx, jit->esi);
            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
        }

        jit->not_(jit->eax);

        Xbyak::Reg32 rn_reg;
        if (n.fields.shift_by_reg) {
            jit->mov(jit->ecx, jit->dword[jit->rsp + 48]);
            rn_reg = jit->ecx;
        } else {
            rn_reg = jit->ecx;
        }

        if (rd >= 8 && rd <= 12) {
            jit->mov(jit->edx, rn_reg);
            jit->and_(jit->edx, jit->eax);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
            }
            jit->StoreRd(rd, jit->edx);
        } else {
            auto rd_reg = jit->gba(rd);
            if (rd_reg.getIdx() != jit->eax.getIdx()) {
                jit->mov(rd_reg, rn_reg);
                jit->and_(rd_reg, jit->eax);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, jit->eax);
                }
                jit->StoreRd(rd, rd_reg);
            } else {
                jit->mov(jit->edx, rn_reg);
                jit->and_(jit->edx, jit->eax);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
                }
                jit->StoreRd(rd, jit->edx);
            }
        }
    }
}

template <bool UsePool, bool SetFlags>
void CodeEmitter::Impl::EmitMov(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rm = n.fields.rm;
    uint32_t imm = static_cast<uint32_t>(n.immediate);
    if constexpr (UsePool) {
        if (rd == 4 || rd == 5 || (rd >= 8 && rd <= 12)) {
            jit->mov(jit->eax, imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, jit->eax, jit->eax, imm);
                }
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            auto rd_reg = jit->gba(rd);
            jit->mov(rd_reg, imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, imm);
                }
            }
            jit->StoreRd(rd, rd_reg);
        }
    } else {
        Xbyak::Reg32 rm_reg;
        if (rm == 15 && n.fields.shift_by_reg) {
            jit->mov(jit->eax, n.instr_pc + 4); 
            rm_reg = jit->eax;
        } else {
            rm_reg = jit->GetRmReg(n, rm);
        }

        auto extract_and_save_carry = [&]() {
            if constexpr (SetFlags) {
                jit->setc(jit->sil); 
                jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                jit->and_(jit->edx, ~(1u << 29));
                jit->movzx(jit->esi, jit->sil);
                jit->shl(jit->esi, 29);
                jit->or_(jit->edx, jit->esi);
                jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
            }
        };

        if (rd >= 8 && rd <= 12) {
            jit->mov(jit->eax, rm_reg);
            jit->EmitShift(n, jit->eax, jit->eax);
            extract_and_save_carry();
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazyLogic, jit->eax, jit->eax, rm_reg);
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            auto rd_reg = jit->gba(rd);
            jit->EmitShift(n, rd_reg, rm_reg);
            extract_and_save_carry();
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, rm_reg);
            }
            jit->StoreRd(rd, rd_reg);
        }
    }
}

template <bool UsePool, bool SetFlags>
void CodeEmitter::Impl::EmitMvn(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rm = n.fields.rm;
    uint32_t imm = static_cast<uint32_t>(n.immediate);
    if constexpr (UsePool) {
        if (rd == 4 || rd == 5 || (rd >= 8 && rd <= 12)) {
            jit->mov(jit->eax, ~imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, jit->eax, jit->eax, ~imm);
                }
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            auto rd_reg = jit->gba(rd);
            jit->mov(rd_reg, ~imm);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else {
                    // Preserve ARM carry semantics for rotated immediates.
                    if (imm > 0xFF) {
                        uint32_t carry = (imm >> 31) & 1;
                        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
                        jit->and_(jit->edx, ~(1u << 29));
                        if (carry) jit->or_(jit->edx, (1u << 29));
                        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
                    }
                    jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, ~imm);
                }
            }
            jit->StoreRd(rd, rd_reg);
        }
    } else {
        Xbyak::Reg32 rm_reg;
        if (rm == 15 && n.fields.shift_by_reg) {
            jit->mov(jit->eax, n.instr_pc + 4); 
            rm_reg = jit->eax;
        } else {
            rm_reg = jit->GetRmReg(n, rm);
        }

        jit->mov(jit->eax, rm_reg);
        jit->EmitShift(n, jit->eax, jit->eax);
        
        if constexpr (SetFlags) {
            jit->setc(jit->sil); // Safe CF extraction
            jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->and_(jit->edx, ~(1u << 29));
            jit->movzx(jit->esi, jit->sil);
            jit->shl(jit->esi, 29);
            jit->or_(jit->edx, jit->esi);
            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);
        }
        
        jit->not_(jit->eax);

        if (rd == 4 || rd == 5 || (rd >= 8 && rd <= 12)) {
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazyLogic, jit->eax, jit->eax, rm_reg);
            }
            jit->StoreRd(rd, jit->eax);
        } else {
            auto rd_reg = jit->gba(rd);
            jit->mov(rd_reg, jit->eax);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazyLogic, rd_reg, rd_reg, rm_reg);
            }
            jit->StoreRd(rd, rd_reg);
        }
    }
}

template <bool UsePool>
void CodeEmitter::Impl::EmitTst(Impl* jit, const IrNode& n) {
    const uint32_t rn = n.fields.rn, rm = n.fields.rm;
    if constexpr (UsePool) {
        uint32_t imm = static_cast<uint32_t>(n.immediate);
        auto rn_reg = jit->GetRnReg(n, rn);
        jit->mov(jit->edx, rn_reg);
        jit->and_(jit->edx, imm);
                    // Preserve ARM carry semantics for rotated immediates.
        if (imm > 0xFF) {
            uint32_t carry = (imm >> 31) & 1;
            jit->mov(jit->eax, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->and_(jit->eax, ~(1u << 29));
            if (carry) jit->or_(jit->eax, (1u << 29));
            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->eax);
        }
        jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, imm);
    } else {
        auto rm_raw = jit->GetRmReg(n, rm);
        jit->mov(jit->eax, rm_raw);
        jit->EmitShift(n, jit->eax, jit->eax);

        jit->setc(jit->sil); 
        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
        jit->and_(jit->edx, ~(1u << 29));
        jit->movzx(jit->esi, jit->sil);
        jit->shl(jit->esi, 29);
        jit->or_(jit->edx, jit->esi);
        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);

        jit->mov(jit->dword[jit->rsp + 48], jit->eax);
        jit->LoadGbaToReg(n, rn, jit->ecx);
        jit->mov(jit->eax, jit->ecx);
        jit->mov(jit->edx, jit->dword[jit->rsp + 48]);
        jit->and_(jit->eax, jit->edx);
        jit->EmitLazySave(kLazyLogic, jit->eax, jit->ecx, jit->edx);
    }
}

template <bool UsePool>
void CodeEmitter::Impl::EmitTeq(Impl* jit, const IrNode& n) {
    const uint32_t rn = n.fields.rn, rm = n.fields.rm;
    if constexpr (UsePool) {
        uint32_t imm = static_cast<uint32_t>(n.immediate);
        auto rn_reg = jit->GetRnReg(n, rn);
        jit->mov(jit->edx, rn_reg);
        jit->xor_(jit->edx, imm);
                    // Preserve ARM carry semantics for rotated immediates.
        if (imm > 0xFF) {
            uint32_t carry = (imm >> 31) & 1;
            jit->mov(jit->eax, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
            jit->and_(jit->eax, ~(1u << 29));
            if (carry) jit->or_(jit->eax, (1u << 29));
            jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->eax);
        }
        jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, imm);
    } else {
        auto rm_raw = jit->GetRmReg(n, rm);
        jit->mov(jit->eax, rm_raw);
        jit->EmitShift(n, jit->eax, jit->eax);

        jit->setc(jit->sil); 
        jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
        jit->and_(jit->edx, ~(1u << 29));
        jit->movzx(jit->esi, jit->sil);
        jit->shl(jit->esi, 29);
        jit->or_(jit->edx, jit->esi);
        jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);

        jit->mov(jit->dword[jit->rsp + 48], jit->eax);
        jit->LoadGbaToReg(n, rn, jit->ecx);
        jit->mov(jit->eax, jit->ecx);
        jit->mov(jit->edx, jit->dword[jit->rsp + 48]);
        jit->xor_(jit->eax, jit->edx);
        jit->EmitLazySave(kLazyLogic, jit->eax, jit->ecx, jit->edx);
    }
}

template <bool UsePool>
void CodeEmitter::Impl::EmitCmp(Impl* jit, const IrNode& n) {
    const uint32_t rn = n.fields.rn, rm = n.fields.rm;
    if constexpr (UsePool) {
        jit->LoadGbaToReg(n, rn, jit->ecx);
        jit->mov(jit->edx, jit->ecx);
        jit->sub(jit->edx, static_cast<uint32_t>(n.immediate));
        jit->EmitLazySave(kLazySub, jit->edx, jit->ecx, static_cast<uint32_t>(n.immediate));
    } else {
        if (n.fields.shift_by_reg) {
            auto rm_raw = jit->GetRmReg(n, rm);
            jit->mov(jit->eax, rm_raw);
            jit->EmitShift(n, jit->eax, jit->eax);
            jit->mov(jit->dword[jit->rsp + 48], jit->eax);

            jit->LoadGbaToReg(n, rn, jit->ecx);
            jit->mov(jit->eax, jit->ecx);
            jit->mov(jit->edx, jit->dword[jit->rsp + 48]);
            jit->sub(jit->eax, jit->edx);
            jit->EmitLazySave(kLazySub, jit->eax, jit->ecx, jit->edx);
        } else {
            jit->LoadGbaToReg(n, rn, jit->ecx);
            auto rm_raw = jit->GetRmReg(n, rm);
            jit->mov(jit->eax, rm_raw);
            jit->EmitShift(n, jit->eax, jit->eax);
            jit->mov(jit->edx, jit->ecx);
            jit->sub(jit->edx, jit->eax);
            jit->EmitLazySave(kLazySub, jit->edx, jit->ecx, jit->eax);
        }
    }
}

template <bool UsePool>
void CodeEmitter::Impl::EmitCmn(Impl* jit, const IrNode& n) {
    const uint32_t rn = n.fields.rn, rm = n.fields.rm;
    if constexpr (UsePool) {
        jit->LoadGbaToReg(n, rn, jit->ecx);
        jit->mov(jit->eax, jit->ecx);
        jit->add(jit->eax, static_cast<uint32_t>(n.immediate));
        jit->EmitLazySave(kLazyAdd, jit->eax, jit->ecx, static_cast<uint32_t>(n.immediate));
    } else {
        if (n.fields.shift_by_reg) {
            auto rm_raw = jit->GetRmReg(n, rm);
            jit->mov(jit->eax, rm_raw);
            jit->EmitShift(n, jit->eax, jit->eax);
            jit->mov(jit->dword[jit->rsp + 48], jit->eax);

            jit->LoadGbaToReg(n, rn, jit->ecx);
            jit->mov(jit->eax, jit->ecx);
            jit->mov(jit->edx, jit->dword[jit->rsp + 48]);
            jit->add(jit->eax, jit->edx);
            jit->EmitLazySave(kLazyAdd, jit->eax, jit->ecx, jit->edx);
        } else {
            jit->LoadGbaToReg(n, rn, jit->ecx);
            auto rm_raw = jit->GetRmReg(n, rm);
            jit->mov(jit->eax, rm_raw);
            jit->EmitShift(n, jit->eax, jit->eax);
            jit->mov(jit->edx, jit->ecx);
            jit->add(jit->edx, jit->eax);
            jit->EmitLazySave(kLazyAdd, jit->edx, jit->ecx, jit->eax);
        }
    }
}

template <bool SetFlags>
void CodeEmitter::Impl::EmitMul(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rn = n.fields.rn, rm = n.fields.rm;
    auto rn_reg = jit->GetRnReg(n, rn);
    auto rm_reg = jit->GetRmReg(n, rm);
    jit->mov(jit->eax, rn_reg);
    jit->imul(jit->eax, rm_reg);
    jit->StoreRd(rd, jit->eax);
    if constexpr (SetFlags) jit->EmitLazySave(kLazyLogic, jit->eax, rn_reg, rm_reg);
}

template <bool SetFlags>
void CodeEmitter::Impl::EmitUmull(Impl* jit, const IrNode& n) {
    jit->LoadGbaToReg(n, static_cast<uint32_t>(n.immediate), jit->ecx); 
    jit->mov(jit->eax, jit->GetRmReg(n, n.fields.rm));
    jit->mul(jit->ecx); 
    jit->StoreRd(n.fields.rd, jit->eax); 
    jit->StoreRd(n.fields.rn, jit->edx); 
    if constexpr (SetFlags) jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
}

template <bool SetFlags>
void CodeEmitter::Impl::EmitUmlal(Impl* jit, const IrNode& n) {
    jit->LoadGbaToReg(n, static_cast<uint32_t>(n.immediate), jit->ecx);
    jit->mov(jit->eax, jit->GetRmReg(n, n.fields.rm));
    jit->mul(jit->ecx);
    jit->LoadGbaToReg(n, n.fields.rd, jit->ecx);
    jit->add(jit->eax, jit->ecx);
    jit->StoreRd(n.fields.rd, jit->eax);
    jit->LoadGbaToReg(n, n.fields.rn, jit->ecx);
    jit->adc(jit->edx, jit->ecx);
    jit->StoreRd(n.fields.rn, jit->edx);
    if constexpr (SetFlags) jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
}

template <bool SetFlags>
void CodeEmitter::Impl::EmitSmull(Impl* jit, const IrNode& n) {
    jit->LoadGbaToReg(n, static_cast<uint32_t>(n.immediate), jit->ecx);
    jit->mov(jit->eax, jit->GetRmReg(n, n.fields.rm));
    jit->imul(jit->ecx); 
    jit->StoreRd(n.fields.rd, jit->eax);
    jit->StoreRd(n.fields.rn, jit->edx);
    if constexpr (SetFlags) jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
}

template <bool SetFlags>
void CodeEmitter::Impl::EmitSmlal(Impl* jit, const IrNode& n) {
    jit->LoadGbaToReg(n, static_cast<uint32_t>(n.immediate), jit->ecx);
    jit->mov(jit->eax, jit->GetRmReg(n, n.fields.rm));
    jit->imul(jit->ecx);
    jit->LoadGbaToReg(n, n.fields.rd, jit->ecx);
    jit->add(jit->eax, jit->ecx);
    jit->StoreRd(n.fields.rd, jit->eax);
    jit->LoadGbaToReg(n, n.fields.rn, jit->ecx);
    jit->adc(jit->edx, jit->ecx);
    jit->StoreRd(n.fields.rn, jit->edx);
    if constexpr (SetFlags) jit->EmitLazySave(kLazyLogic, jit->edx, jit->edx, jit->eax);
}

template <bool UsePool, bool SetFlags>
void CodeEmitter::Impl::EmitRsb(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd, rn = n.fields.rn, rm = n.fields.rm;
    auto rn_reg = jit->GetRnReg(n, rn);

    if constexpr (UsePool) {
        // Keep Rn in a temp first; Rd may alias Rn (e.g. RSBS r0, r0, #0).
        // If we write Rd before consuming Rn, we would compute imm-imm instead of imm-oldRn.
        jit->mov(jit->edx, rn_reg);
        jit->mov(jit->eax, static_cast<uint32_t>(n.immediate));
        jit->sub(jit->eax, jit->edx);
        if constexpr (SetFlags) {
            if (rd == 15) jit->EmitExceptionReturn();
            else jit->EmitLazySave(kLazySub, jit->eax, static_cast<uint32_t>(n.immediate), jit->edx);
        }
        jit->StoreRd(rd, jit->eax);
    } else {
        if (n.fields.shift_by_reg) {
            jit->mov(jit->dword[jit->rsp + 48], rn_reg);
        } else {
            jit->mov(jit->ecx, rn_reg);
            rn_reg = jit->ecx;
        }

        auto rm_raw = jit->GetRmReg(n, rm);
        jit->mov(jit->eax, rm_raw);
        jit->EmitShift(n, jit->eax, jit->eax);

        auto rm_shifted = jit->eax;

        if (n.fields.shift_by_reg) {
            jit->mov(jit->ecx, jit->dword[jit->rsp + 48]);
            rn_reg = jit->ecx;
        }

        if (rd >= 8 && rd <= 12) {
            jit->mov(jit->edx, rm_shifted);
            jit->sub(jit->edx, rn_reg);
            if constexpr (SetFlags) {
                if (rd == 15) jit->EmitExceptionReturn();
                else jit->EmitLazySave(kLazySub, jit->edx, rm_shifted, rn_reg);
            }
            jit->StoreRd(rd, jit->edx);
        } else {
            auto rd_reg = jit->gba(rd);
            if (rd_reg.getIdx() != jit->eax.getIdx()) {
                jit->mov(rd_reg, rm_shifted);
                jit->sub(rd_reg, rn_reg);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else jit->EmitLazySave(kLazySub, rd_reg, rm_shifted, rn_reg);
                }
                jit->StoreRd(rd, rd_reg);
            } else {
                jit->mov(jit->edx, rm_shifted);
                jit->sub(jit->edx, rn_reg);
                if constexpr (SetFlags) {
                    if (rd == 15) jit->EmitExceptionReturn();
                    else jit->EmitLazySave(kLazySub, jit->edx, rm_shifted, rn_reg);
                }
                jit->StoreRd(rd, jit->edx);
            }
        }
    }
}

void CodeEmitter::Impl::EmitLoadLiteral(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd;
    uint32_t imm = static_cast<uint32_t>(n.immediate);
    jit->mov(jit->eax, imm);
    jit->StoreRd(rd, jit->eax);
}

void CodeEmitter::Impl::EmitLoadFast32(Impl* jit, const IrNode& n) { jit->EmitFastmem<true, 32>(n); }
void CodeEmitter::Impl::EmitLoadFast16(Impl* jit, const IrNode& n) { jit->EmitFastmem<true, 16>(n); }
void CodeEmitter::Impl::EmitLoadFast8(Impl* jit, const IrNode& n) { jit->EmitFastmem<true, 8>(n); }
void CodeEmitter::Impl::EmitStoreFast32(Impl* jit, const IrNode& n) { jit->EmitFastmem<false, 32>(n); }
void CodeEmitter::Impl::EmitStoreFast16(Impl* jit, const IrNode& n) { jit->EmitFastmem<false, 16>(n); }
void CodeEmitter::Impl::EmitStoreFast8(Impl* jit, const IrNode& n) { jit->EmitFastmem<false, 8>(n); }

void CodeEmitter::Impl::EmitBranch(Impl* jit, const IrNode& n) {
    jit->mov(jit->ebx, static_cast<uint32_t>(n.immediate));
    jit->jmp(*jit->current_exit_label_);
}
void CodeEmitter::Impl::EmitCall(Impl* jit, const IrNode& n) {
    jit->mov(jit->r12d, static_cast<uint32_t>(n.link_value)); 
    jit->mov(jit->ebx, static_cast<uint32_t>(n.immediate));   
    jit->jmp(*jit->current_exit_label_);
}
void CodeEmitter::Impl::EmitBranchExchange(Impl* jit, const IrNode& n) {
    jit->LoadGbaToReg(n, n.fields.rm, jit->eax);

    jit->mov(jit->edx, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
    jit->and_(jit->edx, ~(1u << 5));
    jit->mov(jit->ecx, jit->eax);
    jit->and_(jit->ecx, 1u);
    jit->shl(jit->ecx, 5);
    jit->or_(jit->edx, jit->ecx);
    jit->mov(jit->dword[jit->rdi + offsetof(CpuState, cpsr)], jit->edx);

    Xbyak::Label bx_thumb, bx_done;
    jit->test(jit->ecx, jit->ecx); 
    jit->jne(bx_thumb);

    jit->and_(jit->eax, ~3u); 
    jit->jmp(bx_done);

    jit->L(bx_thumb);
    // ARM DDI 0100E §A4.1.5: "If bit[0] of Rm is set, set the T bit in the CPSR
    // and load the address into PC with bit[0] cleared."
    // The T-bit was already written into CPSR above (line 1782).  Here we only
    // need to produce the correctly-aligned branch target for the dispatcher.
    // DO NOT set bit[0] back — the dispatcher also masks it, but doing so here
    // is an explicit spec violation that would corrupt PC if ever read mid-block.
    jit->and_(jit->eax, ~1u);  // Thumb branch target = Rm & ~1

    jit->L(bx_done);
    jit->mov(jit->ebx, jit->eax);
    jit->jmp(*jit->current_exit_label_);
}

void CodeEmitter::Impl::EmitMrs(Impl* jit, const IrNode& n) {
    const uint32_t rd = n.fields.rd;
    const uint32_t psr_select = static_cast<uint32_t>(n.immediate) & 1;
    if (psr_select == 0) {
        jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kMaterializeCpsr);
        jit->BeforeCall();
        jit->call(jit->rax);
        jit->AfterCall();
    }
    size_t offset = psr_select ? offsetof(CpuState, spsr) : offsetof(CpuState, cpsr);
    jit->mov(jit->eax, jit->dword[jit->rdi + offset]);
    jit->StoreRd(rd, jit->eax);
}

void CodeEmitter::Impl::EmitMsr(Impl* jit, const IrNode& n) {
    const uint32_t psr_select = static_cast<uint32_t>(n.immediate) & 1;
    size_t offset = psr_select ? offsetof(CpuState, spsr) : offsetof(CpuState, cpsr);
    const uint32_t field_mask = static_cast<uint32_t>(n.fields.rn) & 0xFu;  // c=1, x=2, s=4, f=8
    uint32_t write_mask = 0;
    if (field_mask & 1u) write_mask |= 0x000000FFu;  // control [7:0]
    if (field_mask & 2u) write_mask |= 0x0000FF00u;  // extension [15:8]
    if (field_mask & 4u) write_mask |= 0x00FF0000u;  // status [23:16]
    if (field_mask & 8u) write_mask |= 0xFF000000u;  // flags [31:24]
    if (write_mask == 0) return;

    // For CPSR, the T bit (bit 5) cannot be changed via MSR (use BX for ARM/Thumb switching).
    if (psr_select == 0) write_mask &= ~(1u << 5);
    const uint32_t keep_mask = ~write_mask;

    jit->mov(jit->rdi, jit->r13);
    if (psr_select == 0) {
        // MSR may preserve parts of CPSR; ensure lazy flags are materialized first.
        jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kMaterializeCpsr);
        jit->BeforeCall();
        jit->call(jit->rax);
        jit->AfterCall();
    }

    // Operand: immediate vs register form.
    if (n.fields.use_pool) {
        jit->mov(jit->eax, static_cast<uint32_t>(n.link_value));
    } else {
        jit->LoadGbaToReg(n, n.fields.rm, jit->eax);
    }

    jit->mov(jit->edx, jit->dword[jit->r13 + offset]);
    jit->mov(jit->esi, jit->edx);
    jit->and_(jit->esi, 0x1Fu); 
    jit->and_(jit->edx, keep_mask);
    jit->and_(jit->eax, write_mask);

    jit->or_(jit->eax, jit->edx);

    jit->mov(jit->dword[jit->rsp + 48], jit->eax); 
    jit->mov(jit->ecx, jit->eax);
    jit->and_(jit->ecx, 0x1Fu); 

    if (psr_select == 0) {
        Xbyak::Label skip_swap;
        jit->cmp(jit->esi, jit->ecx);
        jit->je(skip_swap);
        jit->mov(jit->dword[jit->r13 + offsetof(CpuState, registers) + 13 * 4], jit->ebp);
        jit->mov(jit->dword[jit->r13 + offsetof(CpuState, registers) + 14 * 4], jit->r12d);
        jit->mov(jit->rdi, jit->r13);
        jit->mov(jit->edx, jit->ecx); 

        jit->BeforeCall();
        jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kSwapBankedRegisters);
        jit->call(jit->rax);
        jit->AfterCall();

        jit->mov(jit->ebp, jit->dword[jit->r13 + offsetof(CpuState, registers) + 13 * 4]);
        jit->mov(jit->r12d, jit->dword[jit->r13 + offsetof(CpuState, registers) + 14 * 4]);
        jit->L(skip_swap);
    }

    jit->mov(jit->eax, jit->dword[jit->rsp + 48]);
    jit->mov(jit->dword[jit->r13 + offset], jit->eax);
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, flag_op)], static_cast<uint32_t>(kLazyNone));
}

void CodeEmitter::Impl::EmitSwi(Impl* jit, const IrNode& n) {
#ifdef B_DEBUG
    // Debug: log SWI number and registers at runtime
    jit->BeforeCall();
    jit->mov(jit->esi, static_cast<uint32_t>(n.immediate));
    jit->mov(jit->edx, static_cast<uint32_t>(n.link_value));
    jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kDebugLogSwi);
    jit->call(jit->rax);
    jit->AfterCall();
#endif
    jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kMaterializeCpsr);
    jit->BeforeCall();
    jit->call(jit->rax);
    jit->AfterCall();

    jit->mov(jit->eax, jit->dword[jit->rdi + offsetof(CpuState, cpsr)]);
    jit->mov(jit->dword[jit->rsp + 56], jit->eax); 
    jit->mov(jit->esi, jit->eax);
    jit->and_(jit->esi, 0x1Fu); 

    // Clear T and I while preserving F and upper CPSR bits.
    jit->and_(jit->eax, ~0xBFu);
    jit->or_(jit->eax, 0x93u);

    jit->mov(jit->dword[jit->rsp + 48], jit->eax); 

    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, registers) + 13 * 4], jit->ebp);
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, registers) + 14 * 4], jit->r12d);
    jit->mov(jit->rdi, jit->r13);
    jit->mov(jit->edx, 0x13u); 

    jit->BeforeCall();
    jit->MovRaxSymbol(BlockCacheData::X86SymbolId::kSwapBankedRegisters);
    jit->call(jit->rax);
    jit->AfterCall();

    jit->mov(jit->eax, jit->dword[jit->rsp + 56]);
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, spsr)], jit->eax);

    jit->mov(jit->ecx, jit->dword[jit->rsp + 48]); 
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, cpsr)], jit->ecx);
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, flag_op)], static_cast<uint32_t>(kLazyNone));

    jit->mov(jit->eax, static_cast<uint32_t>(n.link_value));
    jit->mov(jit->dword[jit->r13 + offsetof(CpuState, registers) + 14 * 4], jit->eax);
    jit->mov(jit->ebp, jit->dword[jit->r13 + offsetof(CpuState, registers) + 13 * 4]);
    jit->mov(jit->r12d, jit->dword[jit->r13 + offsetof(CpuState, registers) + 14 * 4]);

    jit->mov(jit->ebx, 0x08u);
    jit->jmp(*jit->current_exit_label_);
}

#define REGISTER_ALU(op) \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][0][0] = &Impl::EmitALU<IrOp::op, false, false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][0][1] = &Impl::EmitALU<IrOp::op, false, true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][1][0] = &Impl::EmitALU<IrOp::op, true, false>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][1][1] = &Impl::EmitALU<IrOp::op, true, true>
#define REGISTER_BIC() \
    g_dispatch_table[static_cast<size_t>(IrOp::kBic)][0][0] = &Impl::EmitBic<false, false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kBic)][0][1] = &Impl::EmitBic<false, true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kBic)][1][0] = &Impl::EmitBic<true, false>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kBic)][1][1] = &Impl::EmitBic<true, true>
#define REGISTER_MOV() \
    g_dispatch_table[static_cast<size_t>(IrOp::kMov)][0][0] = &Impl::EmitMov<false, false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kMov)][0][1] = &Impl::EmitMov<false, true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kMov)][1][0] = &Impl::EmitMov<true, false>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kMov)][1][1] = &Impl::EmitMov<true, true>
#define REGISTER_MVN() \
    g_dispatch_table[static_cast<size_t>(IrOp::kMvn)][0][0] = &Impl::EmitMvn<false, false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kMvn)][0][1] = &Impl::EmitMvn<false, true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kMvn)][1][0] = &Impl::EmitMvn<true, false>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kMvn)][1][1] = &Impl::EmitMvn<true, true>
#define REGISTER_CMP() \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmp)][0][0] = &Impl::EmitCmp<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmp)][0][1] = &Impl::EmitCmp<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmp)][1][0] = &Impl::EmitCmp<true>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmp)][1][1] = &Impl::EmitCmp<true>
#define REGISTER_CMN() \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmn)][0][0] = &Impl::EmitCmn<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmn)][0][1] = &Impl::EmitCmn<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmn)][1][0] = &Impl::EmitCmn<true>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kCmn)][1][1] = &Impl::EmitCmn<true>
#define REGISTER_TST() \
    g_dispatch_table[static_cast<size_t>(IrOp::kTst)][0][0] = &Impl::EmitTst<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTst)][0][1] = &Impl::EmitTst<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTst)][1][0] = &Impl::EmitTst<true>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTst)][1][1] = &Impl::EmitTst<true>
#define REGISTER_TEQ() \
    g_dispatch_table[static_cast<size_t>(IrOp::kTeq)][0][0] = &Impl::EmitTeq<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTeq)][0][1] = &Impl::EmitTeq<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTeq)][1][0] = &Impl::EmitTeq<true>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kTeq)][1][1] = &Impl::EmitTeq<true>
#define REGISTER_MUL() \
    g_dispatch_table[static_cast<size_t>(IrOp::kMul)][0][0] = &Impl::EmitMul<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kMul)][0][1] = &Impl::EmitMul<true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kMul)][1][0] = &Impl::EmitMul<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kMul)][1][1] = &Impl::EmitMul<true>
#define REGISTER_LONG_MUL(op, func) \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][0][0] = &Impl::func<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][0][1] = &Impl::func<true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][1][0] = &Impl::func<false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::op)][1][1] = &Impl::func<true>
#define REGISTER_RSB() \
    g_dispatch_table[static_cast<size_t>(IrOp::kRsb)][0][0] = &Impl::EmitRsb<false, false>; \
    g_dispatch_table[static_cast<size_t>(IrOp::kRsb)][0][1] = &Impl::EmitRsb<false, true>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kRsb)][1][0] = &Impl::EmitRsb<true, false>;  \
    g_dispatch_table[static_cast<size_t>(IrOp::kRsb)][1][1] = &Impl::EmitRsb<true, true>

void CodeEmitter::Impl::InitDispatchTable() {
    REGISTER_ALU(kAdd);
    REGISTER_ALU(kSub);
    REGISTER_ALU(kAdc);
    REGISTER_ALU(kSbc);
    REGISTER_ALU(kRsc);
    REGISTER_ALU(kAnd);
    REGISTER_ALU(kOrr);
    REGISTER_ALU(kEor);
    REGISTER_BIC();
    REGISTER_MOV();
    REGISTER_MVN();
    REGISTER_CMP();
    REGISTER_CMN();
    REGISTER_TST();
    REGISTER_TEQ();
    REGISTER_MUL();
    REGISTER_LONG_MUL(kUmull, EmitUmull);
    REGISTER_LONG_MUL(kUmlal, EmitUmlal);
    REGISTER_LONG_MUL(kSmull, EmitSmull);
    REGISTER_LONG_MUL(kSmlal, EmitSmlal);
    REGISTER_RSB();

    auto set4 = [](IrOp op, EmitterFunc f) {
        size_t i = static_cast<size_t>(op);
        g_dispatch_table[i][0][0] = g_dispatch_table[i][0][1] =
        g_dispatch_table[i][1][0] = g_dispatch_table[i][1][1] = f;
    };
    set4(IrOp::kLoadLiteral, &Impl::EmitLoadLiteral);
    set4(IrOp::kLoadFast32, &Impl::EmitLoadFast32);
    set4(IrOp::kLoadFast16, &Impl::EmitLoadFast16);
    set4(IrOp::kLoadFast8, &Impl::EmitLoadFast8);
    set4(IrOp::kStoreFast32, &Impl::EmitStoreFast32);
    set4(IrOp::kStoreFast16, &Impl::EmitStoreFast16);
    set4(IrOp::kStoreFast8, &Impl::EmitStoreFast8);
    set4(IrOp::kBranch, &Impl::EmitBranch);
    set4(IrOp::kCall, &Impl::EmitCall);
    set4(IrOp::kBranchExchange, &Impl::EmitBranchExchange);
    set4(IrOp::kMrs, &Impl::EmitMrs);
    set4(IrOp::kMsr, &Impl::EmitMsr);
    set4(IrOp::kSwi, &Impl::EmitSwi);
    set4(IrOp::kLoad32, &Impl::EmitLoad32);
    set4(IrOp::kStore32, &Impl::EmitStore32);
    set4(IrOp::kLoad16, &Impl::EmitLoad16);
    set4(IrOp::kStore16, &Impl::EmitStore16);
    set4(IrOp::kLoad8, &Impl::EmitLoad8);
    set4(IrOp::kStore8, &Impl::EmitStore8);
    set4(IrOp::kLoadSigned8, &Impl::EmitLoadSigned8);
    set4(IrOp::kLoadSigned16, &Impl::EmitLoadSigned16);
    set4(IrOp::kLoadIO, &Impl::EmitLoad32); 
    set4(IrOp::kStoreIO, &Impl::EmitStore32);
    set4(IrOp::kLoadUser32, &Impl::EmitLoadUser32);
    set4(IrOp::kStoreUser32, &Impl::EmitStoreUser32);
}

#undef REGISTER_ALU
#undef REGISTER_BIC
#undef REGISTER_MOV
#undef REGISTER_MVN
#undef REGISTER_CMP
#undef REGISTER_CMN
#undef REGISTER_TST
#undef REGISTER_TEQ
#undef REGISTER_MUL
#undef REGISTER_LONG_MUL
#undef REGISTER_RSB

CodeEmitter::CodeEmitter(size_t buffer_size) : impl_(AXOLOTL_ALLOC_TAG_NEW("CodeEmitter::Impl", new Impl(buffer_size))) {}

CodeEmitter::~CodeEmitter() { AXOLOTL_ALLOC_TAG_DELETE("CodeEmitter::Impl", impl_); }

void* CodeEmitter::EmitBlock(ArenaAllocator* arena, uint32_t pc, uint32_t block_cycles) {
    return impl_->EmitBlock(arena, pc, block_cycles);
}

CodeEmitter::EmittedBlockArtifact CodeEmitter::EmitBlockWithRelocs(ArenaAllocator* arena,
                                                                   uint32_t pc,
                                                                   uint32_t block_cycles,
                                                                   bool is_thumb,
                                                                   uint32_t block_len) {
    return impl_->EmitBlockWithRelocs(arena, pc, block_cycles, is_thumb, block_len);
}

size_t CodeEmitter::GetLastEmittedBlockSize() const {
    return impl_->last_emitted_size_;
}

size_t CodeEmitter::GetBytesUsed() const {
    return impl_->getSize();
}

size_t CodeEmitter::GetCapacityBytes() const {
    return impl_->capacity_bytes_;
}

void CodeEmitter::SetupCpuStateForJit(CpuState* state, MemoryBus* bus) {
    state->page_table_ptr = bus->GetPageTablePtr();
    state->memory_bus = bus;
    state->read32_helper = reinterpret_cast<void*>(&JitRead32);
    state->write32_helper = reinterpret_cast<void*>(&JitWrite32);
    state->read16_helper = reinterpret_cast<void*>(&JitRead16);
    state->write16_helper = reinterpret_cast<void*>(&JitWrite16);
    state->read8_helper = reinterpret_cast<void*>(&JitRead8);
    state->write8_helper = reinterpret_cast<void*>(&JitWrite8);
    for (int i = 0; i < 6; ++i) {
        state->bank_r13[i] = 0;
        state->bank_r14[i] = 0;
        state->bank_spsr[i] = 0;
    }
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 5; ++j)
            state->bank_r8_r12[i][j] = 0;
}

uintptr_t CodeEmitter::ResolveCacheSymbol(BlockCacheData::X86SymbolId id) {
    switch (id) {
        case BlockCacheData::X86SymbolId::kJitRead32:
            return reinterpret_cast<uintptr_t>(&JitRead32);
        case BlockCacheData::X86SymbolId::kJitWrite32:
            return reinterpret_cast<uintptr_t>(&JitWrite32);
        case BlockCacheData::X86SymbolId::kJitRead16:
            return reinterpret_cast<uintptr_t>(&JitRead16);
        case BlockCacheData::X86SymbolId::kJitWrite16:
            return reinterpret_cast<uintptr_t>(&JitWrite16);
        case BlockCacheData::X86SymbolId::kJitRead8:
            return reinterpret_cast<uintptr_t>(&JitRead8);
        case BlockCacheData::X86SymbolId::kJitWrite8:
            return reinterpret_cast<uintptr_t>(&JitWrite8);
        case BlockCacheData::X86SymbolId::kMaterializeCpsr:
            return reinterpret_cast<uintptr_t>(&MaterializeCPSR);
        case BlockCacheData::X86SymbolId::kSwapBankedRegisters:
            return reinterpret_cast<uintptr_t>(&SwapBankedRegisters);
        case BlockCacheData::X86SymbolId::kJitCrashJumpToIo:
            return reinterpret_cast<uintptr_t>(&JitCrashJumpToIO);
        case BlockCacheData::X86SymbolId::kJitGetUserReg:
            return reinterpret_cast<uintptr_t>(&JitGetUserReg);
        case BlockCacheData::X86SymbolId::kJitSetUserReg:
            return reinterpret_cast<uintptr_t>(&JitSetUserReg);
        case BlockCacheData::X86SymbolId::kDebugLogSwi:
#ifdef B_DEBUG
            return reinterpret_cast<uintptr_t>(&DebugLogSwi);
#else
            return 0;
#endif
        default:
            return 0;
    }
}

std::string CodeEmitter::HostFeatureMaskString() {
    std::string mask;
#if defined(__x86_64__) || defined(_M_X64)
    mask += "x64";
#else
    mask += "other";
#endif
#if defined(__SSE2__) || defined(_M_X64) || (_M_IX86_FP >= 2)
    mask += "|sse2";
#endif
#if defined(__SSE4_1__)
    mask += "|sse41";
#endif
#if defined(__AVX__)
    mask += "|avx";
#endif
#if defined(__AVX2__)
    mask += "|avx2";
#endif
    return mask;
}
