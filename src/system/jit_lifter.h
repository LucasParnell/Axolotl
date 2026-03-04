#pragma once

#include <cstdint>
#include <vector>
#include <utility>

#include "data/ir_node.h"
#include "data/arena_alloc.h"
#include "data/literal_pool.h"

class MemoryBus;

// GBA machine code -> stream of IrNodes.
class IrBuilder {
 public:
    IrBuilder(MemoryBus* bus, ArenaAllocator* arena);

    template <bool IsThumb>
    void BuildBlock(uint32_t start_pc);

    const std::vector<std::pair<uint32_t, bool>>& GetDiscoveredTargets() const {
        return targets_;
    }

    ArenaAllocator* GetArena() { return arena_; }
    uint32_t GetBlockLength() const { return block_len_; }
    uint32_t GetBlockCycles() const { return block_cycles_; }
    MemoryBus* GetBus() const { return bus_; }

 private:
    inline void DecodeArm(uint32_t pc, uint32_t instr, IrNode& n, bool& is_terminal);
    inline void DecodeThumb(uint32_t& pc, uint16_t instr, IrNode& n, bool& is_terminal);

    MemoryBus* bus_;
    ArenaAllocator* arena_;
    LiteralPool literal_pool_;

    uint32_t block_len_;
    uint32_t block_cycles_{0};

    uint32_t known_reg_val_[16];
    bool     known_reg_valid_[16];
    std::vector<std::pair<uint32_t, bool>> targets_;
};
