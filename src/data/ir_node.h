#pragma once
#include <cstddef>
#include <cstdint>

enum class IrOp : uint64_t {
    kAdd, kSub, kAdc, kSbc, kRsb, kRsc,
    kAnd, kOrr, kEor, kBic, kMov, kMvn,
    kCmp, kCmn, kTst, kTeq,
    kMul, kUmull, kUmlal, kSmull, kSmlal,
    kLoad32, kLoad16, kLoad8, kLoadSigned16, kLoadSigned8,
    kStore32, kStore16, kStore8,
    kLoadFast32, kLoadFast16, kLoadFast8,
    kStoreFast32, kStoreFast16, kStoreFast8,
    kLoadIO, kStoreIO, kLoadLiteral,
    kLoadUser32, kStoreUser32,
    kBranch, kCall, kBranchExchange,
    kMrs, kMsr, kSwi, kSync, kExit, kOther,
    kCount, kJumpLabel
};

// Bitfield layout: uint64_t to avoid padding. Cast opcode with static_cast<IrOp>(node.fields.opcode).
struct IrNodeFields {
    IrOp opcode      : 6;
    uint64_t rd          : 4;
    uint64_t rn          : 4;
    uint64_t rm          : 4;
    uint64_t cond        : 4;
    uint64_t set_flags   : 1;
    uint64_t use_pool    : 1;
    uint64_t shift_type  : 2;  // 0=LSL, 1=LSR, 2=ASR, 3=ROR
    uint64_t shift_imm   : 5;  // 0-31
    uint64_t shift_by_reg: 1;  // 1 = use Rs for shift amount
    uint64_t u_bit       : 1;  // ARM mem: 1 = add offset (Up), 0 = subtract (Down)
    uint64_t reserved    : 31;
};

// alignas(16) plus padding yields sizeof(IrNode) == 32. Arena and block limits
// must use sizeof(IrNode) (or kMaxBlockNodes below) for correct node counts.
struct alignas(16) IrNode {
    int32_t immediate;
    union {
        uint64_t raw_bits;
        IrNodeFields fields;
    };
    /** For kCall: return address (ARM: pc+4, Thumb: pc+2). Unused otherwise. */
    uint32_t link_value = 0;
    /** Architectural pipeline PC for this instruction (ARM: addr+8, Thumb: addr+4). */
    uint32_t instr_pc = 0;
};

/// Max IR nodes per block; must fit in arena capacity (e.g. 2048 bytes / 32 = 64).
constexpr size_t kMaxBlockNodes = 62;
