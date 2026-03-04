#include "system/jit_lifter.h"
#include "system/gba_timing.h"
#include "system/memory_bus.h"
#include <array>
#include <bit>

namespace {

constexpr std::array<uint8_t, static_cast<size_t>(IrOp::kCount)> BuildExecBaseOverhead() {
    std::array<uint8_t, static_cast<size_t>(IrOp::kCount)> overhead{};

    // Multiply family.
    overhead[static_cast<size_t>(IrOp::kMul)] = 4u;
    overhead[static_cast<size_t>(IrOp::kSmull)] = 5u;
    overhead[static_cast<size_t>(IrOp::kUmull)] = 5u;
    overhead[static_cast<size_t>(IrOp::kSmlal)] = 6u;
    overhead[static_cast<size_t>(IrOp::kUmlal)] = 6u;

    // Load family: 1N + 1I.
    overhead[static_cast<size_t>(IrOp::kLoad32)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoad16)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoad8)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadSigned8)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadSigned16)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadFast32)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadFast16)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadFast8)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadLiteral)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadIO)] = 2u;
    overhead[static_cast<size_t>(IrOp::kLoadUser32)] = 2u;

    // Store family: 1N.
    overhead[static_cast<size_t>(IrOp::kStore32)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStore16)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStore8)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStoreFast32)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStoreFast16)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStoreFast8)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStoreIO)] = 1u;
    overhead[static_cast<size_t>(IrOp::kStoreUser32)] = 1u;

    return overhead;
}

constexpr auto kExecBaseOverhead = BuildExecBaseOverhead();

constexpr bool IsShiftByRegAluOp(IrOp op) {
    switch (op) {
        case IrOp::kAdd: case IrOp::kSub: case IrOp::kAdc: case IrOp::kSbc:
        case IrOp::kRsc: case IrOp::kAnd: case IrOp::kOrr: case IrOp::kEor:
        case IrOp::kBic: case IrOp::kMov: case IrOp::kMvn: case IrOp::kRsb:
        case IrOp::kCmp: case IrOp::kCmn: case IrOp::kTst: case IrOp::kTeq:
            return true;
        default:
            return false;
    }
}

constexpr bool IsStoreOp(IrOp op) {
    switch (op) {
        case IrOp::kStore32: case IrOp::kStore16: case IrOp::kStore8:
        case IrOp::kStoreFast32: case IrOp::kStoreFast16: case IrOp::kStoreFast8:
        case IrOp::kStoreIO: case IrOp::kStoreUser32:
            return true;
        default:
            return false;
    }
}

// Conservative x86 size estimate per IR node used to cap block complexity before emission.
// This is intentionally pessimistic for memory/IO paths to avoid Xbyak overflows.
constexpr uint32_t EstimateNodeX86Bytes(const IrNode& n) {
    const IrOp op = static_cast<IrOp>(n.fields.opcode);
    switch (op) {
        case IrOp::kLoad32:
        case IrOp::kLoad16:
        case IrOp::kLoad8:
        case IrOp::kLoadSigned8:
        case IrOp::kLoadSigned16:
        case IrOp::kStore32:
        case IrOp::kStore16:
        case IrOp::kStore8:
        case IrOp::kLoadUser32:
        case IrOp::kStoreUser32:
            return 448u;
        case IrOp::kLoadIO:
        case IrOp::kStoreIO:
            return 512u;
        case IrOp::kMul:
        case IrOp::kSmull:
        case IrOp::kUmull:
        case IrOp::kSmlal:
        case IrOp::kUmlal:
            return 192u;
        case IrOp::kBranch:
        case IrOp::kCall:
        case IrOp::kBranchExchange:
        case IrOp::kSwi:
            return 128u;
        default:
            return n.fields.cond == 0xE ? 96u : 144u;
    }
}

template <bool IsThumb>
inline uint32_t FetchCostS(uint32_t pc) {
    return GbaTiming::FetchS(pc >> 24, IsThumb);
}

template <bool IsThumb>
inline uint32_t BranchRefillCost() {
    return GbaTiming::FetchS(0x00, IsThumb) + GbaTiming::FetchN(0x00, IsThumb);
}

template <bool IsThumb>
uint32_t ExecOverhead(const IrNode& n) {
    const IrOp op = static_cast<IrOp>(n.fields.opcode);

    if (op == IrOp::kBranch || op == IrOp::kCall || op == IrOp::kBranchExchange || op == IrOp::kSwi) {
        return BranchRefillCost<IsThumb>();
    }

    if (IsShiftByRegAluOp(op) && n.fields.shift_by_reg) return 1u;

    const size_t idx = static_cast<size_t>(op);
    return idx < kExecBaseOverhead.size() ? kExecBaseOverhead[idx] : 0u;
}

}  // namespace

[[nodiscard]] static constexpr uint32_t RotateRight(uint32_t val, uint32_t rotation) {
    if (rotation == 0) return val;
    return (val >> rotation) | (val << (32 - rotation));
}

IrBuilder::IrBuilder(MemoryBus* bus, ArenaAllocator* arena) 
    : bus_(bus), arena_(arena) {}

template <bool IsThumb>
void IrBuilder::BuildBlock(uint32_t start_pc) {
    arena_->Reset();
    targets_.clear();
    literal_pool_.reset();
    block_cycles_ = 0;

    // Non-sequential penalty for the first instruction fetch (entering a new block).
    const uint32_t start_region = start_pc >> 24;
    block_cycles_ += GbaTiming::FetchN(start_region, IsThumb);
    if (block_cycles_ > GbaTiming::FetchS(start_region, IsThumb))
        block_cycles_ -= GbaTiming::FetchS(start_region, IsThumb);

    for (int i = 0; i < 16; ++i) known_reg_valid_[i] = false;

    uint32_t pc = start_pc;
    bool is_terminal = false;
    // Keep enough margin from the dispatcher's hard 64KB cap.
    constexpr uint32_t kEstimatedX86BlockBudget = 48u * 1024u;
    uint32_t estimated_x86_bytes = 0;

    while (!is_terminal) {
        IrNode& n = *arena_->Alloc<IrNode>();
        n.raw_bits = 0;   // Zero all bitfields (arena memory is uninitialized)
        n.immediate = 0;
        n.link_value = 0;

        // Provide the exact architectural Pipeline PC to the emitter.
        n.instr_pc = pc + (IsThumb ? 4 : 8);

        if constexpr (IsThumb) {
            uint16_t instr = bus_->Read16(pc, pc);
            DecodeThumb(pc, instr, n, is_terminal);
            if (IsStoreOp(static_cast<IrOp>(n.fields.opcode)) && n.link_value == 0) n.link_value = (pc + 2) | 1u;
            pc += 2;
        } else {
            uint32_t instr = bus_->Read32(pc, pc);
            DecodeArm(pc, instr, n, is_terminal);
            if (IsStoreOp(static_cast<IrOp>(n.fields.opcode)) && n.link_value == 0) n.link_value = pc + 4;
            pc += 4;
        }
        estimated_x86_bytes += EstimateNodeX86Bytes(n);

        // Accumulate cycles for this instruction.
        // Fetch cost (S cycle; N for first was added above) + execution overhead.
        block_cycles_ += FetchCostS<IsThumb>(pc - (IsThumb ? 2u : 4u));
        block_cycles_ += ExecOverhead<IsThumb>(n);
        // If this instruction writes R15 (e.g. ALU→PC), pay pipeline refill (branch/call/bx/swi already in ExecOverhead).
        if (n.fields.rd == 15) {
            const IrOp op = static_cast<IrOp>(n.fields.opcode);
            if (op != IrOp::kBranch && op != IrOp::kCall &&
                op != IrOp::kBranchExchange && op != IrOp::kSwi) {
                block_cycles_ += GbaTiming::FetchS(start_region, IsThumb)
                             + GbaTiming::FetchN(start_region, IsThumb);
            }
        }

        if (__builtin_expect(arena_->BytesUsed() >= kMaxBlockNodes * sizeof(IrNode), 0)) {
            if (!is_terminal) {
                IrNode& term = *arena_->Alloc<IrNode>();
                term.immediate = IsThumb ? (pc | 1u) : pc;
                term.raw_bits = 0;
                term.fields.opcode = IrOp::kBranch;
                term.fields.cond = 0xE;
                // Force Thumb bit when Thumb so decoder stays in Thumb for the next block.
                targets_.push_back({IsThumb ? (pc | 1u) : pc, IsThumb});
                is_terminal = true;
            }
        }

        if (!is_terminal && __builtin_expect(estimated_x86_bytes >= kEstimatedX86BlockBudget, 0)) {
            IrNode& term = *arena_->Alloc<IrNode>();
            term.immediate = IsThumb ? (pc | 1u) : pc;
            term.raw_bits = 0;
            term.fields.opcode = IrOp::kBranch;
            term.fields.cond = 0xE;
            targets_.push_back({IsThumb ? (pc | 1u) : pc, IsThumb});
            is_terminal = true;
        }
    }
    block_len_ = pc - start_pc;
}

void IrBuilder::DecodeArm(uint32_t pc, uint32_t instr, IrNode& n, bool& is_terminal) {
    n.fields.cond = (instr >> 28) & 0xF;
    uint32_t cluster = (instr >> 25) & 0x7;

    if (__builtin_expect(cluster <= 1, 1)) {
        // Multiply (MUL, MLA): 0000 00AS dddd nnnn ssss 1001 mmmm
        if ((instr & 0x0FC000F0) == 0x00000090) {
            bool accumulate = (instr >> 21) & 1;
            uint8_t rd = (instr >> 16) & 0xF;
            uint8_t rn = (instr >> 12) & 0xF;
            uint8_t rs = (instr >> 8) & 0xF;
            uint8_t rm = instr & 0xF;

            if (accumulate) {
                n.fields.opcode = IrOp::kMul;
                n.fields.set_flags = 0;
                n.fields.rd = rd;
                n.fields.rn = rs;
                n.fields.rm = rm;
                n.immediate = 0;

                IrNode& n2 = *arena_->Alloc<IrNode>();
                n2.immediate = 0;
                n2.raw_bits = 0;
                n2.fields.opcode = IrOp::kAdd;
                n2.fields.set_flags = (instr >> 20) & 1;
                n2.fields.cond = n.fields.cond;
                n2.fields.rd = rd;
                n2.fields.rn = rd;
                n2.fields.rm = rn;
            } else {
                n.fields.opcode = IrOp::kMul;
                n.fields.set_flags = (instr >> 20) & 1;
                n.fields.rd = rd;
                n.fields.rn = rs;
                n.fields.rm = rm;
                n.immediate = 0;
            }
            known_reg_valid_[rd] = false;
            return;
        }
        
        // MULL/MLAL: 0000 1UAS dHi dLo ssss 1001 mmmm
        if ((instr & 0x0F8000F0) == 0x00800090) {
            bool is_signed = (instr >> 22) & 1;
            bool accumulate = (instr >> 21) & 1;
            if (is_signed) n.fields.opcode = accumulate ? IrOp::kSmlal : IrOp::kSmull;
            else           n.fields.opcode = accumulate ? IrOp::kUmlal : IrOp::kUmull;
            
            n.fields.set_flags = (instr >> 20) & 1;
            n.fields.rd = (instr >> 12) & 0xF;  // RdLo (bits 15:12)
            n.fields.rn = (instr >> 16) & 0xF;  // RdHi (bits 19:16)
            n.fields.rm = instr & 0xF;
            n.immediate = (instr >> 8) & 0xF; // Rs
            known_reg_valid_[n.fields.rd] = false;
            known_reg_valid_[n.fields.rn] = false;
            return;
        }

        // SWP/SWPB: 0001 0B00 nnnn dddd 0000 1001 mmmm
        if ((instr & 0x0FB000F0) == 0x01000090) {
            bool is_byte = (instr >> 22) & 1;
            uint8_t rn = (instr >> 16) & 0xF;
            uint8_t rd = (instr >> 12) & 0xF;
            uint8_t rm = instr & 0xF;

            // Load: Rd = [Rn] (zero-offset immediate addressing)
            n.fields.opcode = is_byte ? IrOp::kLoad8 : IrOp::kLoad32;
            n.fields.rn = rn;
            n.fields.rd = rd;
            n.fields.use_pool = 1;  // immediate offset path
            n.immediate = 0;

            // Store: [Rn] = Rm
            IrNode& n2 = *arena_->Alloc<IrNode>();
            n2.immediate = 0;
            n2.raw_bits = 0;
            n2.link_value = pc + 4;  // next instruction PC for IO-store early exit
            n2.instr_pc = n.instr_pc;
            n2.fields.opcode = is_byte ? IrOp::kStore8 : IrOp::kStore32;
            n2.fields.cond = n.fields.cond;
            n2.fields.rn = rn;
            n2.fields.rd = rm;
            n2.fields.use_pool = 1;  // immediate offset path
            known_reg_valid_[rd] = false;
            return;
        }

        // LDRH/STRH/LDRSB/LDRSH: 000P UIMW Lnnn dddd iiii 1SH1 iiii
        // [ARM-ARM §A5.3]: Miscellaneous Loads and Stores.
        // P=0 → post-indexed (address=Rn, then Rn±=offset).
        // P=1,W=0 → offset (address=Rn±offset, Rn unchanged).
        // P=1,W=1 → pre-indexed (address=Rn±offset, Rn=address).
        if ((instr & 0x0E000090) == 0x00000090) {
            bool load  = (instr >> 20) & 1;
            bool sign  = (instr >> 6) & 1;
            bool half  = (instr >> 5) & 1;
            bool u_bit = (instr >> 23) & 1;
            bool p_bit = (instr >> 24) & 1;
            bool w_bit = (instr >> 21) & 1;
            bool is_imm = (instr >> 22) & 1;

            if (load) {
                if (!sign && half) n.fields.opcode = IrOp::kLoad16;
                else if (sign && !half) n.fields.opcode = IrOp::kLoadSigned8;
                else if (sign && half)  n.fields.opcode = IrOp::kLoadSigned16;
                else n.fields.opcode = IrOp::kOther;
            } else {
                n.fields.opcode = IrOp::kStore16;
            }

            n.fields.rn = (instr >> 16) & 0xF;
            n.fields.rd = (instr >> 12) & 0xF;

            // Decode offset and save for writeback.
            uint32_t wb_imm = 0;
            uint32_t wb_rm  = 0;
            if (is_imm) {
                wb_imm = ((instr >> 8) & 0xF) << 4 | (instr & 0xF);
                if (p_bit) {
                    // Pre-indexed / offset: apply to load/store address
                    n.immediate = static_cast<int32_t>(u_bit ? wb_imm : -static_cast<int32_t>(wb_imm));
                    n.fields.use_pool = 1;
                } else {
                    // Post-indexed: address = Rn
                    n.immediate = 0;
                    n.fields.use_pool = 1;
                }
            } else {
                wb_rm = instr & 0xF;
                if (p_bit) {
                    n.fields.rm = wb_rm;
                    n.fields.use_pool = 0;
                    n.fields.u_bit = u_bit ? 1 : 0;
                } else {
                    // Post-indexed: address = Rn
                    n.immediate = 0;
                    n.fields.use_pool = 1;
                }
            }
            
            // ARM-ARM §B6.1.3 same rule for miscellaneous loads (LDRH/LDRSB/LDRSH).
            if (load && (n.fields.rd == n.fields.rn) && (w_bit || !p_bit) && n.fields.rn != 15) {
                n.fields.opcode = u_bit ? IrOp::kAdd : IrOp::kSub;
                n.fields.set_flags = 0;
                n.fields.use_pool = 0;
                if (is_imm) {
                    n.immediate = wb_imm;
                    n.fields.use_pool = 1;
                } else {
                    n.fields.rm = wb_rm;
                }
                known_reg_valid_[n.fields.rd] = false;
                return;
            }

            if (load) known_reg_valid_[n.fields.rd] = false;

            // Emit writeback node: Rn = Rn ± offset
            // [ARM-ARM §A5.3]: P=0 always writes back; P=1,W=1 writes back.
            if ((w_bit || !p_bit) && n.fields.rn != 15 && !(load && n.fields.rd == n.fields.rn)) {
                IrNode& n2 = *arena_->Alloc<IrNode>();
                n2.immediate = 0;
                n2.raw_bits = 0;
                n2.fields.cond = n.fields.cond;
                n2.fields.rd = n.fields.rn;
                n2.fields.rn = n.fields.rn;
                n2.fields.opcode = u_bit ? IrOp::kAdd : IrOp::kSub;
                n2.fields.set_flags = 0;
                if (is_imm) {
                    n2.immediate = wb_imm;
                    n2.fields.use_pool = 1;
                } else {
                    n2.fields.rm = wb_rm;
                    n2.fields.use_pool = 0;
                }
                known_reg_valid_[n.fields.rn] = false;
            }
            return;
        }

        // BX before MSR (mask overlap).
        if ((instr & 0x0FFFFFF0) == 0x012FFF10) {
            n.fields.opcode = IrOp::kBranchExchange;
            n.fields.rm = instr & 0xF;
            is_terminal = true;
            if (n.fields.cond != 0xE) targets_.push_back({pc + 4, false});
            return;
        }

        // MRS: 0001 0R00 1111 dddd 0000 0000 0000
        if ((instr & 0x0FBF0FFF) == 0x010F0000) {
            n.fields.opcode = IrOp::kMrs;
            n.fields.rd = (instr >> 12) & 0xF;
            n.immediate = (instr >> 22) & 1;
            known_reg_valid_[n.fields.rd] = false;
            return;
        }

        // MSR immediate: 0011 0R10 ffff 1111 rrrr iiii iiii (rot in 11:8, imm8 in 7:0)
        if ((instr & 0x0FB0F000) == 0x0320F000) {
            n.fields.opcode = IrOp::kMsr;
            n.immediate = (instr >> 22) & 1;
            n.fields.rn = (instr >> 16) & 0xF;  // mask (c=1, x=2, s=4, f=8)
            uint32_t rot = (instr >> 8) & 0xF;
            uint32_t imm8 = instr & 0xFF;
            n.link_value = RotateRight(imm8, rot * 2);
            n.fields.use_pool = 1;  // signals immediate form to emitter
            return;
        }

        // MSR register: 0001 0R10 ffff 1111 0000 0000 0000 rrrr
        if ((instr & 0x0FB0F000) == 0x0120F000) {
            n.fields.opcode = IrOp::kMsr;
            n.immediate = (instr >> 22) & 1;
            n.fields.rn = (instr >> 16) & 0xF;  // mask (c=1, x=2, s=4, f=8)
            n.fields.rm = instr & 0xF;
            n.fields.use_pool = 0;
            return;
        }

        // Data processing
        uint8_t op = (instr >> 21) & 0xF;
        switch (op) {
            case 0x0: n.fields.opcode = IrOp::kAnd; break;
            case 0x1: n.fields.opcode = IrOp::kEor; break;
            case 0x2: n.fields.opcode = IrOp::kSub; break;
            case 0x3: n.fields.opcode = IrOp::kRsb; break;
            case 0x4: n.fields.opcode = IrOp::kAdd; break;
            case 0x5: n.fields.opcode = IrOp::kAdc; break;
            case 0x6: n.fields.opcode = IrOp::kSbc; break;
            case 0x7: n.fields.opcode = IrOp::kRsc; break;
            case 0x8: n.fields.opcode = IrOp::kTst; break;
            case 0x9: n.fields.opcode = IrOp::kTeq; break;
            case 0xA: n.fields.opcode = IrOp::kCmp; break;
            case 0xB: n.fields.opcode = IrOp::kCmn; break;
            case 0xC: n.fields.opcode = IrOp::kOrr; break;
            case 0xD: n.fields.opcode = IrOp::kMov; break;
            case 0xE: n.fields.opcode = IrOp::kBic; break;
            case 0xF: n.fields.opcode = IrOp::kMvn; break;
        }

        n.fields.set_flags = (instr >> 20) & 1;
        n.fields.rn = (instr >> 16) & 0xF;
        n.fields.rd = (instr >> 12) & 0xF;

        if ((instr >> 25) & 1) {
            uint32_t imm = instr & 0xFF;
            uint32_t rot = ((instr >> 8) & 0xF) * 2;
            n.immediate = RotateRight(imm, rot);
            n.fields.use_pool = 1;
            // ARM DDI 0100E §A2.4.3: R15 as a source operand reads as pc+8 (pipeline fetch address).
            //
            // For kAdd ONLY: bake the final value (pc+8+imm) directly into n.immediate.
            // The emitter has a special case for kAdd+UsePool+rn==15 (lines ~1133-1154) that
            // emits just `mov rd, imm` — it never reads the Rn register at all.  The baked
            // constant is used directly, which is correct and efficient.
            //
            // For ALL OTHER ops (kSub, kCmp, kTst, kAnd, kOrr, kEor, kBic, etc.) do NOT bake.
            // Those emitter paths do: GetRnReg(n, 15) → LoadGbaToReg(n, 15, ...) → n.instr_pc = pc+8.
            // If n.immediate were also baked to pc+8+imm, the computation would become:
            //   (pc+8) OP (pc+8+imm)   instead of the correct   (pc+8) OP imm.
            //
            // Leave n.immediate as the raw rotated immediate for non-ADD; the emitter loads
            // rn=15 via instr_pc and applies the op against the raw imm value.
            if (n.fields.rn == 15 && n.fields.opcode == IrOp::kAdd)
                n.immediate = pc + 8 + static_cast<uint32_t>(n.immediate);

            if (n.fields.opcode == IrOp::kMov) {
                known_reg_val_[n.fields.rd] = n.immediate;
                known_reg_valid_[n.fields.rd] = true;
            } else {
                known_reg_valid_[n.fields.rd] = false;
            }
        } else { 
            n.fields.rm = instr & 0xF;
            n.fields.shift_type = (instr >> 5) & 0x3;
            n.fields.shift_by_reg = (instr >> 4) & 1;
            if (n.fields.shift_by_reg) {
                n.immediate = (instr >> 8) & 0xF; 
            } else {
                n.fields.shift_imm = (instr >> 7) & 0x1F;
            }
            known_reg_valid_[n.fields.rd] = false;
        }

        if (n.fields.rd == 15) {
            is_terminal = true;
            if (n.fields.cond != 0xE) {
                n.link_value = pc + 4;
                targets_.push_back({pc + 4, false});
            }
        }
        return;
    }

    // Branch
    if (cluster == 0b101) {
        bool is_link = (instr >> 24) & 1;
        n.fields.opcode = is_link ? IrOp::kCall : IrOp::kBranch;

        int32_t offset = (instr & 0x00FFFFFF);
        if (offset & 0x00800000) offset |= 0xFF000000;

        n.immediate = pc + 8 + (offset << 2);
        if (is_link)
            n.link_value = pc + 4;  // ARM: return address = next instruction
        else if (n.fields.cond != 0xE)
            n.link_value = pc + 4;  // fall-through for not-taken conditional B

        targets_.push_back({n.immediate, false});

        is_terminal = true;  // ALL branches terminate the block
        if (n.fields.cond != 0xE)
            targets_.push_back({pc + 4, false});  // conditional: also record fall-through
        return;
    }

    // LDM/STM: lowest reg at lowest addr, step +4.
    if (cluster == 0b100) {
        bool load = (instr >> 20) & 1;
        uint8_t rn = (instr >> 16) & 0xF;
        uint16_t list = instr & 0xFFFF;
        bool p = (instr >> 24) & 1;  // Pre/Post indexing
        bool u = (instr >> 23) & 1;  // Up/Down
        bool w = (instr >> 21) & 1;  // Writeback
        bool s = (instr >> 22) & 1;  // PSR & force user bit

        int real_reg_count = std::popcount(static_cast<uint32_t>(list));
        int reg_count = real_reg_count == 0 ? 16 : real_reg_count;
        int writeback_bytes = reg_count * 4;

        // ARMv4 quirk: empty register list -> acts like 16 regs for base updates, but only transfers R15
        if (real_reg_count == 0) {
            list = 1u << 15;  // R15 only
        }

        // ARMv4 (ARM7TDMI): LDM with base in the register list suppresses
        // writeback — the loaded value is the final value of Rn.
        // Ref: GBATEK "Strange Effects on Invalid Rlist's": "no writeback (LDM/ARMv4)"
        if (load && (list & (1 << rn))) {
            w = false;
        }

        // Determine if this is a User Bank Transfer.
        // STM with S-bit is always a user transfer. LDM is only a user transfer if PC is NOT in the list.
        bool is_user_bank = false;
        if (s) {
            if (!load || !(list & (1 << 15))) {
                is_user_bank = true;
            }
        }

        // Disable fast paths for User Bank transfers to keep the JIT paths simple (they are extremely rare).
        bool use_fast = false;
        if (!is_user_bank && known_reg_valid_[rn]) {
            uint8_t region = known_reg_val_[rn] >> 24;
            if (region == 0x02 || region == 0x03) use_fast = true;
        }

        IrOp load_op = use_fast ? IrOp::kLoadFast32 : (is_user_bank ? IrOp::kLoadUser32 : IrOp::kLoad32);
        IrOp store_op = is_user_bank ? IrOp::kStoreUser32 : IrOp::kStore32;

        int32_t lowest_offset = 0;
        if (u) {
            lowest_offset = p ? 4 : 0;   // IA: 0; IB: +4
        } else {
            lowest_offset = p ? -(reg_count * 4) : -(reg_count * 4) + 4;  // DB / DA
        }

        int idx = 0;
        for (int r = 0; r < 16; ++r) {
            if (!(list & (1 << r))) continue;
            int32_t current_offset = lowest_offset + (idx * 4);
            IrOp mem_op = load ? load_op : store_op;

            IrNode* node;
            if (idx == 0) {
                node = &n;
            } else {
                node = arena_->Alloc<IrNode>();
            }
            node->immediate = static_cast<uint32_t>(current_offset);
            node->raw_bits = 0;
            node->fields.opcode = mem_op;
            node->fields.cond = (instr >> 28) & 0xF;
            node->fields.rd = r;
            node->fields.rn = rn;
            node->fields.use_pool = 1;

            if (load && r == 15 && ((instr >> 28) & 0xF) != 0xE) node->link_value = pc + 4;
            // Suppress early exit for STM stores: link_value=1 is a sentinel
            // that EmitEarlyExitIfIOStore ignores.  Without this, an IO-range
            // store would abort the block mid-transfer before writeback,
            // causing infinite loops (e.g. BIOS CpuFastSet zeroing 0x04000200).
            if (!load) node->link_value = 1;

            if (load) known_reg_valid_[r] = false;
            ++idx;
        }

        if (w) {
            IrNode& nw = *arena_->Alloc<IrNode>();
            nw.immediate = writeback_bytes;
            nw.raw_bits = 0;
            nw.fields.opcode = u ? IrOp::kAdd : IrOp::kSub;
            nw.fields.cond = (instr >> 28) & 0xF;
            nw.fields.rd = rn;
            nw.fields.rn = rn;
            nw.fields.use_pool = 1;
            known_reg_valid_[rn] = false;
        }

        if (load && (list & (1 << 15))) {
            // Exception Return quirk: LDM {..., PC}^ restores CPSR from SPSR
            if (s) {
                IrNode& exc = *arena_->Alloc<IrNode>();
                exc.immediate = 0;
                exc.raw_bits = 0;
                exc.fields.opcode = IrOp::kSub;  // SUBS PC, PC, #0 perfectly triggers EmitExceptionReturn
                exc.fields.cond = (instr >> 28) & 0xF;
                exc.fields.rd = 15;
                exc.fields.rn = 15;
                exc.fields.use_pool = 1;
                exc.fields.set_flags = 1;
                if (((instr >> 28) & 0xF) != 0xE) exc.link_value = pc + 4;
            }
            is_terminal = true;
            targets_.push_back({pc + 4, false});  // Always push speculative hint
        }
        return;
    }

    // ARM SWI: 1111_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx
    // ARM DDI 0100E §A5.6: SWI encoding has bits 27:24 = 1111.
    // cluster bits 27:25 = 111 also matches coprocessor instructions (CDP, MCR, MRC)
    // which have bits 27:24 = 1110 (bit 24 = 0).  Gate on bit 24 to distinguish.
    // GBA has no coprocessor, but incorrectly decoding CDP/MCR/MRC as SWI would corrupt
    // the SWI immediate and set is_terminal on a non-branch instruction.
    if (cluster == 0b111 && (instr & (1u << 24))) {
        n.fields.opcode = IrOp::kSwi;
        n.immediate = instr & 0x00FFFFFF;
        n.link_value = pc + 4;  // return after BIOS call
        is_terminal = true;
        targets_.push_back({pc + 4, false});
        return;
    }

    if (cluster == 0b010 || cluster == 0b011) {
        bool is_load = (instr >> 20) & 1;
        bool is_byte = (instr >> 22) & 1;
        bool p_bit = (instr >> 24) & 1;
        bool u_bit = (instr >> 23) & 1;
        bool w_bit = (instr >> 21) & 1;
        bool is_reg_offset = (instr >> 25) & 1;

        n.fields.rn = (instr >> 16) & 0xF;
        n.fields.rd = (instr >> 12) & 0xF;

        // Decode the offset (immediate or register-shifted).
        // [ARM-ARM §A4.1.23/99]: P=1 → offset/pre-indexed, P=0 → post-indexed.
        // For post-indexed (P=0) the memory address is just Rn; the offset is
        // applied only to the writeback node below.
        int32_t  wb_immediate = 0;
        uint32_t wb_rm = 0, wb_shift_type = 0, wb_shift_imm = 0;
        bool     wb_shift_by_reg = false;
        int32_t  wb_reg_imm = 0;  // Rs index when shift_by_reg

        if (!is_reg_offset) {
            uint32_t imm = instr & 0xFFF;
            int32_t signed_imm = static_cast<int32_t>(u_bit ? imm : -static_cast<int32_t>(imm));
            wb_immediate = u_bit ? static_cast<int32_t>(imm) : static_cast<int32_t>(imm);
            if (p_bit) {
                // Pre-indexed / offset: apply offset to the load/store address
                n.immediate = signed_imm;
                n.fields.use_pool = 1;
            } else {
                // Post-indexed: address = Rn (no offset on the load/store node)
                n.immediate = 0;
                n.fields.use_pool = 1;
            }
        } else {
            wb_rm = instr & 0xF;
            wb_shift_type = (instr >> 5) & 0x3;
            wb_shift_by_reg = (instr >> 4) & 1;
            if (wb_shift_by_reg) {
                wb_reg_imm = (instr >> 8) & 0xF;
            }
            wb_shift_imm = (instr >> 7) & 0x1F;
            if (p_bit) {
                // Pre-indexed / offset: apply register offset to the load/store
                n.fields.rm = wb_rm;
                n.fields.shift_type = wb_shift_type;
                n.fields.shift_by_reg = wb_shift_by_reg;
                if (wb_shift_by_reg) {
                    n.immediate = wb_reg_imm;
                } else {
                    n.fields.shift_imm = wb_shift_imm;
                }
                n.fields.use_pool = 0;
                n.fields.u_bit = u_bit ? 1 : 0;
            } else {
                // Post-indexed: address = Rn (no register offset on the node)
                n.immediate = 0;
                n.fields.use_pool = 1;  // immediate 0
            }
        }

        if (n.fields.rn == 15 && is_load && !is_reg_offset) {
            n.fields.opcode = IrOp::kLoadLiteral;
            uint32_t addr = (pc + 8) + static_cast<int32_t>(n.immediate);
            if (is_byte) n.immediate = bus_->Read32(addr, pc) & 0xFF;
            else         n.immediate = bus_->Read32(addr, pc);
            
            n.fields.use_pool = 1;
            known_reg_val_[n.fields.rd] = n.immediate;
            known_reg_valid_[n.fields.rd] = true;

            if (n.fields.rd == 15) {
                is_terminal = true;
                // When conditional, set link_value so the skip path updates PC to pc+4 (avoids lockup).
                if (n.fields.cond != 0xE) {
                    n.link_value = pc + 4;
                    targets_.push_back({pc + 4, false});
                }
            }
            return;
        }

        if (known_reg_valid_[n.fields.rn]) {
            uint32_t base_addr = known_reg_val_[n.fields.rn];
            uint8_t region = base_addr >> 24;
            if (region == 0x02 || region == 0x03) {
                if (is_byte) {
                    n.fields.opcode = is_load ? IrOp::kLoadFast8 : IrOp::kStore8;  // no StoreFast8: 8-bit stores go through MemoryBus for VRAM/Palette/OAM
                } else {
                    // Fast path for loads only; stores go through MemoryBus for JIT invalidation
                    n.fields.opcode = is_load ? IrOp::kLoadFast32 : IrOp::kStore32;
                }
            } else if (region == 0x04) {
                if (is_byte) n.fields.opcode = is_load ? IrOp::kLoad8 : IrOp::kStore8;
                else         n.fields.opcode = is_load ? IrOp::kLoad32 : IrOp::kStore32;
            } else if (region == 0x0E) {
                // SRAM: 8-bit bus only, never use fast path
                if (is_byte) n.fields.opcode = is_load ? IrOp::kLoad8 : IrOp::kStore8;
                else         n.fields.opcode = is_load ? IrOp::kLoad32 : IrOp::kStore32;
            } else {
                if (is_byte) n.fields.opcode = is_load ? IrOp::kLoad8 : IrOp::kStore8;
                else         n.fields.opcode = is_load ? IrOp::kLoad32 : IrOp::kStore32;
            }
        } else {
            if (is_byte) n.fields.opcode = is_load ? IrOp::kLoad8 : IrOp::kStore8;
            else         n.fields.opcode = is_load ? IrOp::kLoad32 : IrOp::kStore32;
        }

        if (is_load) known_reg_valid_[n.fields.rd] = false;

        if (n.fields.rd == 15 && is_load) {
            is_terminal = true;
            if (n.fields.cond != 0xE) {
                n.link_value = pc + 4;
                targets_.push_back({pc + 4, false});
            }
        }

        const bool suppress_wb = is_load && (n.fields.rd == n.fields.rn);
        if ((w_bit || !p_bit) && n.fields.rn != 15 && !suppress_wb) {
            IrNode& n2 = *arena_->Alloc<IrNode>();
            n2.immediate = 0;
            n2.raw_bits = 0;
            n2.fields.cond = n.fields.cond;
            n2.fields.rd = n.fields.rn;
            n2.fields.rn = n.fields.rn;
            n2.fields.opcode = u_bit ? IrOp::kAdd : IrOp::kSub;
            n2.fields.set_flags = 0;
            if (is_reg_offset) {
                // Use saved register/shift info (not from load/store node,
                // which may have been zeroed for post-indexed mode).
                n2.fields.rm = wb_rm;
                n2.fields.shift_type = wb_shift_type;
                n2.fields.shift_by_reg = wb_shift_by_reg;
                n2.fields.shift_imm = wb_shift_imm;
                if (wb_shift_by_reg) n2.immediate = wb_reg_imm;
            } else {
                // Immediate form: use the unsigned magnitude saved earlier.
                n2.immediate = wb_immediate;
                n2.fields.use_pool = 1;
            }
        }
        return;
    }

    n.fields.opcode = IrOp::kOther;
}

void IrBuilder::DecodeThumb(uint32_t& pc, uint16_t instr, IrNode& n, bool& is_terminal) {
    n.fields.cond = 0xE;

    // Format 2 (Add/Sub) must be checked before Format 1 (Shift by immediate):
    // both have top 3 bits 000; 0x1800 is the more specific mask.
    if ((instr & 0xF800) == 0x1800) {
        bool sub = (instr >> 9) & 1;
        n.fields.opcode = sub ? IrOp::kSub : IrOp::kAdd;
        n.fields.rd = instr & 0x7;
        n.fields.rn = (instr >> 3) & 0x7;
        if ((instr >> 10) & 1) {
            n.immediate = (instr >> 6) & 0x7;
            n.fields.use_pool = 1;
        } else {
            n.fields.rm = (instr >> 6) & 0x7;
        }
        n.fields.set_flags = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xE000) == 0x0000) {
        uint8_t op = (instr >> 11) & 0x3;
        switch (op) {
            case 0: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 0; break;
            case 1: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 1; break;
            case 2: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 2; break;
            default: n.fields.opcode = IrOp::kOther; break;
        }
        n.fields.rd = instr & 0x7;
        n.fields.rm = (instr >> 3) & 0x7;
        n.fields.shift_imm = (instr >> 6) & 0x1F;
        n.fields.set_flags = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xE000) == 0x2000) {
        uint8_t op = (instr >> 11) & 0x3;
        switch (op) {
            case 0: n.fields.opcode = IrOp::kMov; break;
            case 1: n.fields.opcode = IrOp::kCmp; break;
            case 2: n.fields.opcode = IrOp::kAdd; break;
            case 3: n.fields.opcode = IrOp::kSub; break;
        }
        n.fields.rd = (instr >> 8) & 0x7;
        n.fields.rn = n.fields.rd;
        n.immediate = instr & 0xFF;
        n.fields.use_pool = 1;
        n.fields.set_flags = 1;
        
        if (op == 0) {
            known_reg_val_[n.fields.rd] = n.immediate;
            known_reg_valid_[n.fields.rd] = true;
        } else {
            known_reg_valid_[n.fields.rd] = false;
        }
        return;
    }

    if ((instr & 0xFC00) == 0x4000) {
        uint8_t op = (instr >> 6) & 0xF;
        switch (op) {
            case 0x0: n.fields.opcode = IrOp::kAnd; break;
            case 0x1: n.fields.opcode = IrOp::kEor; break;
            case 0x2: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 0; n.fields.shift_by_reg = 1; break;
            case 0x3: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 1; n.fields.shift_by_reg = 1; break;
            case 0x4: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 2; n.fields.shift_by_reg = 1; break;
            case 0x5: n.fields.opcode = IrOp::kAdc; break;
            case 0x6: n.fields.opcode = IrOp::kSbc; break;
            case 0x7: n.fields.opcode = IrOp::kMov; n.fields.shift_type = 3; n.fields.shift_by_reg = 1; break;
            case 0x8: n.fields.opcode = IrOp::kTst; break;
            case 0x9: n.fields.opcode = IrOp::kSub; n.immediate = 0; break;
            case 0xA: n.fields.opcode = IrOp::kCmp; break;
            case 0xB: n.fields.opcode = IrOp::kCmn; break;
            case 0xC: n.fields.opcode = IrOp::kOrr; break;
            case 0xD: n.fields.opcode = IrOp::kMul; break;
            case 0xE: n.fields.opcode = IrOp::kBic; break;
            case 0xF: n.fields.opcode = IrOp::kMvn; break;
        }
        n.fields.rd = instr & 0x7;
        uint8_t rs = (instr >> 3) & 0x7;

        if (op == 0x9) {
            // NEG: Rd = 0 - Rs  →  RSB Rd, Rs, #0
            n.fields.opcode = IrOp::kRsb;
            n.fields.rn = rs;
            n.fields.rm = rs;
            n.immediate = 0;
            n.fields.use_pool = 1;
        } else {
            n.fields.rn = n.fields.rd;
            if (n.fields.shift_by_reg) {
                // Shift-by-reg: rm = value to shift (Rd), immediate = shift-amount reg (Rs)
                n.fields.rm = n.fields.rd;
                n.immediate = rs;
            } else {
                // Standard ALU (AND, EOR, ORR, ADC, SBC, CMP, …): rm = Rs (second operand)
                n.fields.rm = rs;
            }
        }
        n.fields.set_flags = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xFC00) == 0x4400) {
        uint8_t op = (instr >> 8) & 0x3;
        bool h1 = (instr >> 7) & 1;
        bool h2 = (instr >> 6) & 1;
        uint8_t rd = (instr & 0x7) | (h1 << 3);
        uint8_t rs = ((instr >> 3) & 0x7) | (h2 << 3);

        if (op == 3) {
            n.fields.opcode = IrOp::kBranchExchange;
            n.fields.rm = rs;
            is_terminal = true;
        } else if (op == 1) {
            n.fields.opcode = IrOp::kCmp;
            n.fields.rd = rd;
            n.fields.rn = rd;
            n.fields.rm = rs;
            n.fields.set_flags = 1;

            if (rs == 15) {  // Evaluate CMP Rn, PC
                n.fields.use_pool = 1;
                n.immediate = (pc + 4) & ~2u;
            }
        } else {
            if (op == 0) n.fields.opcode = IrOp::kAdd;
            else n.fields.opcode = IrOp::kMov;

            n.fields.rd = rd;
            n.fields.rn = rd;
            n.fields.rm = rs;

            if (op == 0 && rd == 15) {
                // ADD PC, Rs -> ADD PC, Rs, PC (commutative, swap operands for emitter)
                n.fields.rn = rs;
                n.fields.use_pool = 1;
                n.immediate = (pc + 4) & ~2u;
            } else if (rs == 15) {
                // MOV/ADD Rd, PC
                n.fields.use_pool = 1;
                n.immediate = (pc + 4) & ~2u;
            }

            if (rd == 15) is_terminal = true;
            known_reg_valid_[rd] = false;
        }
        return;
    }

    if ((instr & 0xF800) == 0x4800) {
        n.fields.opcode = IrOp::kLoadLiteral;
        n.fields.rd = (instr >> 8) & 0x7;
        uint32_t imm = (instr & 0xFF) << 2;
        // Thumb LDR (literal): base is Align(PC+4, 4).
        uint32_t addr = ((pc + 4) & ~3u) + imm;
        n.immediate = bus_->Read32(addr, pc);
        n.fields.use_pool = 1;
        known_reg_val_[n.fields.rd] = n.immediate;
        known_reg_valid_[n.fields.rd] = true;
        return;
    }

    if ((instr & 0xF000) == 0x5000) {
        bool bit9 = (instr >> 9) & 1;
        n.fields.rd = instr & 0x7;
        n.fields.rn = (instr >> 3) & 0x7;
        n.fields.rm = (instr >> 6) & 0x7;
        n.fields.use_pool = 0;
        n.fields.u_bit = 1;  // Thumb register offsets are always add (U=1)
        known_reg_valid_[n.fields.rd] = false;

        if (!bit9) {
            bool L = (instr >> 11) & 1;
            bool B = (instr >> 10) & 1;
            if (L) n.fields.opcode = B ? IrOp::kLoad8 : IrOp::kLoad32;
            else   n.fields.opcode = B ? IrOp::kStore8 : IrOp::kStore32;
        } else {
            bool H = (instr >> 11) & 1;
            bool S = (instr >> 10) & 1;
            if (S) n.fields.opcode = H ? IrOp::kLoadSigned16 : IrOp::kLoadSigned8;
            else   n.fields.opcode = H ? IrOp::kLoad16 : IrOp::kStore16;
        }
        return;
    }

    if ((instr & 0xE000) == 0x6000) {
        bool load = (instr >> 11) & 1;
        bool byte = (instr >> 12) & 1;
        n.fields.opcode = load ? (byte ? IrOp::kLoad8 : IrOp::kLoad32) : (byte ? IrOp::kStore8 : IrOp::kStore32);
        n.fields.rd = instr & 0x7;
        n.fields.rn = (instr >> 3) & 0x7;
        n.immediate = (instr >> 6) & 0x1F;
        if (!byte) n.immediate <<= 2;
        n.fields.use_pool = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xF000) == 0x8000) {
        bool load = (instr >> 11) & 1;
        n.fields.opcode = load ? IrOp::kLoad16 : IrOp::kStore16;
        n.fields.rd = instr & 0x7;
        n.fields.rn = (instr >> 3) & 0x7;
        n.immediate = ((instr >> 6) & 0x1F) << 1;
        n.fields.use_pool = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xF000) == 0x9000) {
        bool load = (instr >> 11) & 1;
        n.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
        n.fields.rd = (instr >> 8) & 0x7;
        n.fields.rn = 13;
        n.immediate = (instr & 0xFF) << 2;
        n.fields.use_pool = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xF000) == 0xA000) {
        n.fields.opcode = IrOp::kAdd;
        n.fields.rd = (instr >> 8) & 0x7;
        uint8_t rn = ((instr >> 11) & 1) ? 13 : 15;
        n.fields.rn = rn;
        uint32_t offset = (instr & 0xFF) << 2;
        if (rn == 15) {
            // ADD rd, PC: PC is (current+4) with bit 1 forced to 0 (word-aligned). Pre-calculate for JIT.
            n.immediate = ((pc + 4) & ~2u) + offset;
        } else {
            n.immediate = offset;
        }
        n.fields.use_pool = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

    if ((instr & 0xFF00) == 0xB000) {
        n.fields.opcode = IrOp::kAdd;
        n.fields.rd = 13;
        n.fields.rn = 13;
        uint32_t imm = (instr & 0x7F) << 2;
        if (instr & 0x80) n.fields.opcode = IrOp::kSub;
        n.immediate = imm;
        n.fields.use_pool = 1;
        return;
    }

    // ARM7TDMI Thumb Format 14: PUSH/POP. R bit (bit 8) = 1 means PUSH includes LR (R14), POP includes PC (R15).
    if ((instr & 0xF600) == 0xB400) {
        bool load = (instr >> 11) & 1;
        uint16_t list = instr & 0xFF;  // R0–R7 from low 8 bits
        constexpr uint16_t kThumbPushPopRBit = 1u << 8;
        if (instr & kThumbPushPopRBit)
            list |= load ? (1u << 15) : (1u << 14);  // POP→PC, PUSH→LR

        int real_reg_count = std::popcount(static_cast<uint32_t>(list));
        int reg_count = real_reg_count == 0 ? 16 : real_reg_count;
        int writeback_bytes = reg_count * 4;

        if (real_reg_count == 0) {
            list = 1u << 15; // ARMv4 Empty Register List Quirk
        }

        // POP = LDMIA (Up/Post), PUSH = STMDB (Down/Pre)
        int32_t lowest_offset = load ? 0 : -(reg_count * 4);
        int idx = 0;

        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            int32_t offset = lowest_offset + (idx * 4);

            IrNode* node;
            if (idx == 0) node = &n;
            else          node = arena_->Alloc<IrNode>();

            node->immediate = static_cast<uint32_t>(offset);
            node->raw_bits = 0;
            node->fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
            node->fields.cond = 0xE;
            node->fields.rd = i;
            node->fields.rn = 13;
            node->fields.rm = 0;
            node->fields.use_pool = 1;

            // Suppress early exit for PUSH stores (same mid-transfer
            // abort issue as ARM STM — see above).  link_value=1 sentinel.
            if (!load) node->link_value = 1;
            if (load) known_reg_valid_[i] = false;
            ++idx;
        }

        IrNode& nw = *arena_->Alloc<IrNode>();
        nw.immediate = writeback_bytes;
        nw.raw_bits = 0;
        nw.fields.cond = 0xE;
        nw.fields.opcode = load ? IrOp::kAdd : IrOp::kSub;
        nw.fields.rd = 13;
        nw.fields.rn = 13;
        nw.fields.rm = 0;
        nw.fields.use_pool = 1;
        known_reg_valid_[13] = false;

        if (load && (list & (1 << 15))) {
            is_terminal = true;
            targets_.push_back({(pc + 2) | 1u, true}); // speculative return hint
        }
        return;
    }

    if ((instr & 0xF000) == 0xC000) {
        bool load = (instr >> 11) & 1;
        uint8_t rn = (instr >> 8) & 0x7;
        uint16_t list = instr & 0xFF;

        int real_reg_count = std::popcount(static_cast<uint32_t>(list));
        int reg_count = real_reg_count == 0 ? 16 : real_reg_count;
        int writeback_bytes = reg_count * 4;

        if (real_reg_count == 0) {
            list = 1u << 15;  // ARMv4 Empty Register List Quirk
        }

        bool w = true;
        if (load && (list & (1 << rn))) {
            w = false;  // LDM with base in list: no writeback applied
        }

        int idx = 0;
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            int32_t offset = idx * 4;

            IrNode* node;
            if (idx == 0) node = &n;
            else          node = arena_->Alloc<IrNode>();

            node->immediate = static_cast<uint32_t>(offset);
            node->raw_bits = 0;
            node->fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
            node->fields.cond = 0xE;
            node->fields.rd = i;
            node->fields.rn = rn;
            node->fields.rm = 0;
            node->fields.use_pool = 1;

            // Suppress early exit for STMIA stores (same mid-transfer
            // abort issue as ARM STM — see above).  link_value=1 sentinel.
            if (!load) node->link_value = 1;
            if (load) known_reg_valid_[i] = false;
            ++idx;
        }

        if (w) {
            IrNode& nw = *arena_->Alloc<IrNode>();
            nw.immediate = writeback_bytes;
            nw.raw_bits = 0;
            nw.fields.cond = 0xE;
            nw.fields.opcode = IrOp::kAdd;
            nw.fields.rd = rn;
            nw.fields.rn = rn;
            nw.fields.rm = 0;
            nw.fields.use_pool = 1;
            known_reg_valid_[rn] = false;
        }
        return;
    }

    if ((instr & 0xF000) == 0xD000) {
        if (((instr >> 8) & 0xF) == 0xF) {
            n.fields.opcode = IrOp::kSwi;
            n.immediate = instr & 0xFF;
            n.link_value = pc + 2;  // Thumb: return address after SWI
            is_terminal = true;
            return;
        }
        n.fields.opcode = IrOp::kBranch;
        n.fields.cond = (instr >> 8) & 0xF;
        n.link_value = (pc + 2) | 1u;
        int32_t offset = (int8_t)(instr & 0xFF);
        n.immediate = (pc + 4 + (offset << 1)) | 1u;

        // Force Thumb bit so decoder stays in Thumb for this block.
        targets_.push_back({n.immediate, true});
        targets_.push_back({(pc + 2) | 1u, true});
        is_terminal = true;
        return;
    }

    if ((instr & 0xF800) == 0xE000) {
        n.fields.opcode = IrOp::kBranch;
        int32_t offset = (instr & 0x7FF);
        if (offset & 0x400) offset |= 0xFFFFF800;
        n.immediate = (pc + 4 + (offset << 1)) | 1u;

        // Force Thumb bit so decoder stays in Thumb for this block.
        targets_.push_back({n.immediate, true});
        is_terminal = true;
        return;
    }

    if ((instr & 0xF000) == 0xF000) {
        uint16_t next_instr = bus_->Read16(pc + 2, pc);
        pc += 2;

        uint32_t off_high = (instr & 0x7FF);
        uint32_t off_low  = (next_instr & 0x7FF);
        
        int32_t offset = (off_high << 12) | (off_low << 1);
        if (offset & (1<<22)) offset |= 0xFF800000;

        n.fields.opcode = IrOp::kCall;
        n.immediate = ((pc - 2) + 4 + offset) | 1u;
        n.link_value = (pc + 2) | 1u;  // Thumb: LR must have bit 0 set for BX LR return to Thumb

        // Force Thumb bit so decoder stays in Thumb for this block.
        targets_.push_back({n.immediate, true});
        targets_.push_back({(pc + 2) | 1u, true});
        is_terminal = true;
        return;
    }

    n.fields.opcode = IrOp::kOther;
}

template void IrBuilder::BuildBlock<true>(uint32_t);
template void IrBuilder::BuildBlock<false>(uint32_t);
