#include "util/ir_printer.h"
#include <sstream>
#include <iomanip>

std::string IrPrinter::PrintArena(ArenaAllocator* arena, uint32_t start_pc) {
    std::stringstream ss;
    ss << "=== IR Block Start: 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << start_pc << " ===\n";

    size_t node_count = arena->BytesUsed() / sizeof(IrNode);
    IrNode* nodes = static_cast<IrNode*>(arena->GetBasePointer());

    for (size_t i = 0; i < node_count; ++i) {
        IrNode& n = nodes[i];
        
        ss << std::dec << std::setfill(' ') << std::setw(3) << i << " | ";
        ss << std::left << std::setw(12) << OpToString(static_cast<IrOp>(n.fields.opcode));

        ss << " Rd: R" << std::setw(2) << n.fields.rd 
           << " Rn: R" << std::setw(2) << n.fields.rn 
           << " Rm: R" << std::setw(2) << n.fields.rm;


        ss << " | Imm: 0x" << std::right << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << n.immediate;
        
        if (n.fields.use_pool) ss << " [POOL]";

        if (n.fields.set_flags) ss << " 'S'";

        // Note: in ARM/Thumb encodings, some shifts use "imm=0" to mean a special case:
        // - LSR #0 => LSR #32
        // - ASR #0 => ASR #32
        // - ROR #0 => RRX
        const bool has_shift = n.fields.shift_by_reg || n.fields.shift_imm != 0 || ((n.fields.shift_type & 3) != 0);
        if (has_shift) {
            const uint32_t st = n.fields.shift_type & 3;
            const char* shift_names[] = {"LSL", "LSR", "ASR", "ROR"};
            if (n.fields.shift_by_reg) {
                ss << " shift:" << shift_names[st] << ",Rs:R" << (int)n.immediate;
            } else {
                if (st == 3 && n.fields.shift_imm == 0) {
                    ss << " shift:RRX";
                } else if ((st == 1 || st == 2) && n.fields.shift_imm == 0) {
                    ss << " shift:" << shift_names[st] << ",#32";
                } else {
                    ss << " shift:" << shift_names[st] << ",#" << (int)n.fields.shift_imm;
                }
            }
        }

        if (n.fields.cond != 0xE) {
            ss << " (Cond: " << CondToString(n.fields.cond) << ")";
        }

        ss << "\n";
    }
    
    ss << "=== IR Block End (" << std::dec << node_count << " nodes) ===\n";
    return ss.str();
}

std::string IrPrinter::OpToString(IrOp op) {
    switch (op) {
        case IrOp::kAdd: return "ADD";
        case IrOp::kSub: return "SUB";
        case IrOp::kAdc: return "ADC";
        case IrOp::kSbc: return "SBC";
        case IrOp::kRsb: return "RSB";
        case IrOp::kRsc: return "RSC";
        case IrOp::kAnd: return "AND";
        case IrOp::kOrr: return "ORR";
        case IrOp::kEor: return "EOR";
        case IrOp::kBic: return "BIC";
        case IrOp::kMov: return "MOV";
        case IrOp::kMvn: return "MVN";
        case IrOp::kCmp: return "CMP";
        case IrOp::kCmn: return "CMN";
        case IrOp::kTst: return "TST";
        case IrOp::kTeq: return "TEQ";
        case IrOp::kMul: return "MUL";
        case IrOp::kUmull: return "UMULL";
        case IrOp::kUmlal: return "UMLAL";
        case IrOp::kSmull: return "SMULL";
        case IrOp::kSmlal: return "SMLAL";
        case IrOp::kLoad32: return "LDR.32";
        case IrOp::kLoad16: return "LDR.16";
        case IrOp::kLoad8: return "LDR.8";
        case IrOp::kLoadSigned16: return "LDR.S16";
        case IrOp::kLoadSigned8: return "LDR.S8";
        case IrOp::kStore32: return "STR.32";
        case IrOp::kStore16: return "STR.16";
        case IrOp::kStore8: return "STR.8";
        case IrOp::kLoadFast32: return "LDR_FAST.32";
        case IrOp::kLoadFast16: return "LDR_FAST.16";
        case IrOp::kLoadFast8: return "LDR_FAST.8";
        case IrOp::kStoreFast32: return "STR_FAST.32";
        case IrOp::kStoreFast16: return "STR_FAST.16";
        case IrOp::kStoreFast8: return "STR_FAST.8";
        case IrOp::kLoadIO: return "LDR_IO";
        case IrOp::kStoreIO: return "STR_IO";
        case IrOp::kLoadLiteral: return "LDR_LITERAL";
        case IrOp::kBranch: return "BRANCH";
        case IrOp::kCall: return "CALL (BL)";
        case IrOp::kBranchExchange: return "BX";
        case IrOp::kMrs: return "MRS";
        case IrOp::kMsr: return "MSR";
        case IrOp::kSwi: return "SWI";
        case IrOp::kSync: return "SYNC";
        case IrOp::kExit: return "EXIT";
        case IrOp::kOther: return "OTHER";
        
        default: return "UNKNOWN";
    }
}

std::string IrPrinter::CondToString(uint8_t cond) {
    const char* conds[] = {"EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", 
                           "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"};
    if (cond < 16) return conds[cond];
    return "??";
}