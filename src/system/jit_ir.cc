#include "system/jit_ir.h"
#include "system/memory_bus.h"
#include <bit>

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

    for (int i = 0; i < 16; ++i) known_reg_valid_[i] = false;

    uint32_t pc = start_pc;
    bool is_terminal = false;

    while (!is_terminal) {
        IrNode& n = *arena_->Alloc<IrNode>();
        n.immediate = 0;
        n.raw_bits = 0;
        
        if constexpr (IsThumb) {
            uint16_t instr = bus_->Read16(pc, pc);
            DecodeThumb(pc, instr, n, is_terminal);
            pc += 2;
        } else {
            uint32_t instr = bus_->Read32(pc, pc);
            DecodeArm(pc, instr, n, is_terminal);
            pc += 4;
        }

        if (__builtin_expect(arena_->BytesUsed() >= 2000, 0)) is_terminal = true;
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
            n.fields.rd = (instr >> 16) & 0xF; // RdHi
            n.fields.rn = (instr >> 12) & 0xF; // RdLo
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

            n.fields.opcode = is_byte ? IrOp::kLoad8 : IrOp::kLoad32;
            n.fields.rn = rn;
            n.fields.rd = rd;
            n.fields.rm = 0;
            n.immediate = 0;

            IrNode& n2 = *arena_->Alloc<IrNode>();
            n2.immediate = 0;
            n2.raw_bits = 0;
            n2.fields.opcode = is_byte ? IrOp::kStore8 : IrOp::kStore32;
            n2.fields.cond = n.fields.cond;
            n2.fields.rn = rn;
            n2.fields.rd = rm;
            n2.fields.rm = 0;
            known_reg_valid_[rd] = false;
            return;
        }

        // LDRH/STRH/LDRSB/LDRSH: 000P UIMW Lnnn dddd 0000 1SH1 mmmm
        if ((instr & 0x0E000090) == 0x00000090) {
            bool load = (instr >> 20) & 1;
            bool sign = (instr >> 6) & 1;
            bool half = (instr >> 5) & 1;

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
            if ((instr >> 22) & 1) {
                n.immediate = ((instr >> 8) & 0xF) << 4 | (instr & 0xF);
                n.fields.use_pool = 1;
            } else {
                n.fields.rm = instr & 0xF;
            }
            
            if (load) known_reg_valid_[n.fields.rd] = false;
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

        // MSR: 0001 0R10 ffff 1111 ... (reg) / 0011 0R10 ffff 1111 ssss (imm)
        if ((instr & 0x0DB0F000) == 0x0120F000) {
            n.fields.opcode = IrOp::kMsr;
            n.immediate = (instr >> 22) & 1;
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
            if (n.fields.cond != 0xE) targets_.push_back({pc + 4, false});
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

        targets_.push_back({n.immediate, false});
        
        if (n.fields.cond == 0xE) is_terminal = true; 
        else targets_.push_back({pc + 4, false});
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

        int reg_count = std::popcount(static_cast<uint32_t>(list));
        if (reg_count == 0) return;

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
            if (idx == 0) {
                n.immediate = static_cast<uint32_t>(current_offset);
                n.raw_bits = 0;
                n.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                n.fields.cond = (instr >> 28) & 0xF;
                n.fields.rd = r;
                n.fields.rn = rn;
                n.fields.use_pool = 1;
            } else {
                IrNode& nx = *arena_->Alloc<IrNode>();
                nx.immediate = static_cast<uint32_t>(current_offset);
                nx.raw_bits = 0;
                nx.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                nx.fields.cond = (instr >> 28) & 0xF;
                nx.fields.rd = r;
                nx.fields.rn = rn;
                nx.fields.use_pool = 1;
            }
            if (load) known_reg_valid_[r] = false;
            ++idx;
        }
        if (w) {
            IrNode& nw = *arena_->Alloc<IrNode>();
            nw.immediate = reg_count * 4;
            nw.raw_bits = 0;
            nw.fields.opcode = u ? IrOp::kAdd : IrOp::kSub;
            nw.fields.cond = (instr >> 28) & 0xF;
            nw.fields.rd = rn;
            nw.fields.rn = rn;
            nw.fields.use_pool = 1;
        }
        if (load && (list & (1 << 15))) {
            is_terminal = true;
            if (((instr >> 28) & 0xF) != 0xE) targets_.push_back({pc + 4, false});
        }
        return;
    }

    if (cluster == 0b010 || cluster == 0b011) {
        bool is_load = (instr >> 20) & 1;
        bool is_byte = (instr >> 22) & 1;
        bool p_bit = (instr >> 24) & 1;
        bool u_bit = (instr >> 23) & 1;
        bool w_bit = (instr >> 21) & 1;

        n.fields.rn = (instr >> 16) & 0xF;
        n.fields.rd = (instr >> 12) & 0xF;

        if (!( (instr >> 25) & 1 )) {
            n.immediate = instr & 0xFFF;
        } else {
            n.fields.rm = instr & 0xF;
            n.fields.shift_type = (instr >> 5) & 0x3;
            n.fields.shift_by_reg = (instr >> 4) & 1;
            if (n.fields.shift_by_reg) {
                n.immediate = (instr >> 8) & 0xF;
            } else {
                n.fields.shift_imm = (instr >> 7) & 0x1F;
            }
        }

        if (n.fields.rn == 15 && is_load && !( (instr >> 25) & 1 )) {
            n.fields.opcode = IrOp::kLoadLiteral;
            uint32_t addr = (pc + 8) + n.immediate;
            if (is_byte) n.immediate = bus_->Read32(addr, pc) & 0xFF;
            else         n.immediate = bus_->Read32(addr, pc);
            
            n.fields.use_pool = 1;
            known_reg_val_[n.fields.rd] = n.immediate;
            known_reg_valid_[n.fields.rd] = true;
            
            if (n.fields.rd == 15) is_terminal = true;
            return;
        }

        if (known_reg_valid_[n.fields.rn]) {
            uint32_t base_addr = known_reg_val_[n.fields.rn];
            uint8_t region = base_addr >> 24;
            if (region == 0x02 || region == 0x03) {
                if (is_byte) n.fields.opcode = is_load ? IrOp::kLoadFast8 : IrOp::kStoreFast8;
                else         n.fields.opcode = is_load ? IrOp::kLoadFast32 : IrOp::kStoreFast32;
            } else if (region == 0x04) {
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
            if (n.fields.cond != 0xE) targets_.push_back({pc + 4, false});
        }

        if ((w_bit || !p_bit) && n.fields.rn != 15) {
            IrNode& n2 = *arena_->Alloc<IrNode>();
            n2.immediate = 0;
            n2.raw_bits = 0;
            n2.fields.cond = n.fields.cond;
            n2.fields.rd = n.fields.rn;
            n2.fields.rn = n.fields.rn;
            n2.fields.opcode = u_bit ? IrOp::kAdd : IrOp::kSub;
            n2.fields.set_flags = 0;
            if ((instr >> 25) & 1) {
                n2.fields.rm = n.fields.rm;
                n2.fields.shift_type = n.fields.shift_type;
                n2.fields.shift_by_reg = n.fields.shift_by_reg;
                n2.fields.shift_imm = n.fields.shift_imm;
                if (n.fields.shift_by_reg) n2.immediate = n.immediate;
            } else {
                n2.immediate = n.immediate;
                n2.fields.use_pool = 1;
            }
        }
        return;
    }

    n.fields.opcode = IrOp::kOther;
}

void IrBuilder::DecodeThumb(uint32_t& pc, uint16_t instr, IrNode& n, bool& is_terminal) {
    n.fields.cond = 0xE;

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
        n.fields.use_pool = 1;
        n.fields.set_flags = 1;
        known_reg_valid_[n.fields.rd] = false;
        return;
    }

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
        n.fields.rn = n.fields.rd;
        n.fields.rm = (instr >> 3) & 0x7;
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
        } else {
            if (op == 0) n.fields.opcode = IrOp::kAdd;
            else if (op == 1) n.fields.opcode = IrOp::kCmp;
            else n.fields.opcode = IrOp::kMov;
            
            n.fields.rd = rd;
            n.fields.rn = rd;
            n.fields.rm = rs;
            if (rd == 15) is_terminal = true;
            known_reg_valid_[rd] = false;
        }
        return;
    }

    if ((instr & 0xF800) == 0x4800) {
        n.fields.opcode = IrOp::kLoadLiteral;
        n.fields.rd = (instr >> 8) & 0x7;
        uint32_t imm = (instr & 0xFF) << 2;
        uint32_t addr = (pc & ~2) + 4 + imm;
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
        n.fields.rn = ((instr >> 11) & 1) ? 13 : 15;
        n.immediate = (instr & 0xFF) << 2;
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

    if ((instr & 0xF600) == 0xB400) {
        bool load = (instr >> 11) & 1;
        uint16_t list = instr & 0xFF;
        bool r = (instr >> 8) & 1;
        if (r) list |= load ? (1 << 15) : (1 << 14);
        int step = load ? 4 : -4;
        int start_offset = load ? 0 : -4;
        int idx = 0;
        for (int i = 0; i < 16; ++i) {
            if (!(list & (1 << i))) continue;
            int32_t offset = start_offset + idx * step;
            if (idx == 0) {
                n.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                n.fields.rd = i;
                n.fields.rn = 13;
                n.fields.rm = 0;
                n.immediate = static_cast<uint32_t>(static_cast<int32_t>(offset));
                n.fields.use_pool = 1;
            } else {
                IrNode& nx = *arena_->Alloc<IrNode>();
                nx.immediate = 0;
                nx.raw_bits = 0;
                nx.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                nx.fields.cond = 0xE;
                nx.fields.rd = i;
                nx.fields.rn = 13;
                nx.fields.rm = 0;
                nx.immediate = static_cast<uint32_t>(static_cast<int32_t>(offset));
                nx.fields.use_pool = 1;
            }
            if (load) known_reg_valid_[i] = false;
            ++idx;
        }
        IrNode& nw = *arena_->Alloc<IrNode>();
        nw.immediate = 0;
        nw.raw_bits = 0;
        nw.fields.cond = 0xE;
        nw.fields.opcode = load ? IrOp::kAdd : IrOp::kSub;
        nw.fields.rd = 13;
        nw.fields.rn = 13;
        nw.fields.rm = 0;
        nw.immediate = 4 * idx;
        nw.fields.use_pool = 1;
        if (load && r) is_terminal = true;
        return;
    }

    if ((instr & 0xF000) == 0xC000) {
        bool load = (instr >> 11) & 1;
        uint8_t rn = (instr >> 8) & 0x7;
        uint16_t list = instr & 0xFF;
        int idx = 0;
        for (int i = 0; i < 8; ++i) {
            if (!(list & (1 << i))) continue;
            int32_t offset = idx * 4;
            if (idx == 0) {
                n.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                n.fields.rd = i;
                n.fields.rn = rn;
                n.fields.rm = 0;
                n.immediate = static_cast<uint32_t>(offset);
                n.fields.use_pool = 1;
            } else {
                IrNode& nx = *arena_->Alloc<IrNode>();
                nx.immediate = 0;
                nx.raw_bits = 0;
                nx.fields.opcode = load ? IrOp::kLoad32 : IrOp::kStore32;
                nx.fields.cond = 0xE;
                nx.fields.rd = i;
                nx.fields.rn = rn;
                nx.fields.rm = 0;
                nx.immediate = static_cast<uint32_t>(offset);
                nx.fields.use_pool = 1;
            }
            if (load) known_reg_valid_[i] = false;
            ++idx;
        }
        IrNode& nw = *arena_->Alloc<IrNode>();
        nw.immediate = 0;
        nw.raw_bits = 0;
        nw.fields.cond = 0xE;
        nw.fields.opcode = IrOp::kAdd;
        nw.fields.rd = rn;
        nw.fields.rn = rn;
        nw.fields.rm = 0;
        nw.immediate = 4 * idx;
        nw.fields.use_pool = 1;
        return;
    }

    if ((instr & 0xF000) == 0xD000) {
        if (((instr >> 8) & 0xF) == 0xF) {
            n.fields.opcode = IrOp::kSwi;
            n.immediate = instr & 0xFF;
            is_terminal = true;
            return;
        }
        n.fields.opcode = IrOp::kBranch;
        n.fields.cond = (instr >> 8) & 0xF;
        int32_t offset = (int8_t)(instr & 0xFF);
        n.immediate = pc + 4 + (offset << 1);

        targets_.push_back({n.immediate, true});
        targets_.push_back({pc + 2, true});
        is_terminal = true;
        return;
    }

    if ((instr & 0xF800) == 0xE000) {
        n.fields.opcode = IrOp::kBranch;
        int32_t offset = (instr & 0x7FF);
        if (offset & 0x400) offset |= 0xFFFFF800;
        n.immediate = pc + 4 + (offset << 1);

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
        n.immediate = (pc - 2) + 4 + offset;
        
        targets_.push_back({n.immediate, true});
        targets_.push_back({pc + 2, true});
        is_terminal = true;
        return;
    }

    n.fields.opcode = IrOp::kOther;
}

template void IrBuilder::BuildBlock<true>(uint32_t);
template void IrBuilder::BuildBlock<false>(uint32_t);