#pragma once

#include <atomic>
#include <cstddef>

#include "data/arena_alloc.h"
#include "system/jit_emitter.h"
#include "system/jit_lifter.h"
#include "system/prewarm_pacing.h"
#include "system/seed_queue.h"

class BlockMap;
class MemoryBus;
class BlockCompileCache;

// Background thread: compiles blocks from seed queue, spiders branches.
class PreWarmer {
 public:
    PreWarmer(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue,
              PrewarmPacingState* prewarm_pacing,
              BlockCompileCache* block_cache = nullptr);

    void ThreadLoop();

    void Stop() { running_.store(false); }

 private:
    ArenaAllocator pw_arena_;
    IrBuilder pw_builder_;
    size_t pw_code_cache_bytes_{0};
    CodeEmitter pw_emitter_;

    BlockMap* block_map_;
    SeedQueue* seed_queue_;
    PrewarmPacingState* prewarm_pacing_;
    BlockCompileCache* block_cache_;

    std::atomic<bool> running_{true};
};
