#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "data/arena_alloc.h"
#include "system/jit_emitter.h"
#include "system/jit_ir.h"
#include "system/seed_queue.h"

class BlockMap;
class MemoryBus;

// Main execution thread: compile on miss, push targets to PreWarmer.
class JitDispatcher {
 public:
    JitDispatcher(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue);

    void Run(uint32_t start_pc, bool is_thumb);

    void RunSimulated(uint32_t start_pc, bool is_thumb, size_t max_blocks);

    std::atomic<bool> system_running_{true};

 private:
    ArenaAllocator jit_arena_;
    IrBuilder jit_builder_;
    CodeEmitter jit_emitter_;

    BlockMap* block_map_;
    SeedQueue* seed_queue_;
};
