#pragma once

#include <atomic>

#include "data/arena_alloc.h"
#include "system/jit_emitter.h"
#include "system/jit_ir.h"
#include "system/seed_queue.h"

class BlockMap;
class MemoryBus;

// Background thread: compiles blocks from seed queue, spiders branches.
class PreWarmer {
 public:
    PreWarmer(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue);

    void ThreadLoop();

    void Stop() { running_.store(false); }

 private:
    ArenaAllocator pw_arena_;
    IrBuilder pw_builder_;
    CodeEmitter pw_emitter_;

    BlockMap* block_map_;
    SeedQueue* seed_queue_;

    std::atomic<bool> running_{true};
};
