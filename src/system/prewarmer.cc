#include "system/prewarmer.h"
#include "data/block_map.h"
#include "data/ir_node.h"
#include "system/memory_bus.h"
#include "util/ir_printer.h"
#include "util/logger.h"

#include <queue>
#include <thread>

namespace {
uint32_t ToBlockMapPc(uint32_t pc) {
    return (pc < BlockMap::kRomStart) ? (BlockMap::kRomStart + pc) : pc;
}
}  // namespace

PreWarmer::PreWarmer(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue)
    : pw_arena_(1024 * 1024),
      pw_builder_(bus, &pw_arena_),
      pw_emitter_(),
      block_map_(block_map),
      seed_queue_(seed_queue) {}

void PreWarmer::ThreadLoop() {
    std::queue<CompileTarget> local_spider_queue;
    CompileTarget seed;

    while (running_.load(std::memory_order_relaxed)) {
        while (seed_queue_->Pop(seed)) {
            local_spider_queue.push(seed);
        }

        if (!local_spider_queue.empty()) {
            CompileTarget req = local_spider_queue.front();
            local_spider_queue.pop();

            if (!block_map_->TryClaim(ToBlockMapPc(req.pc), req.is_thumb)) {
                continue;  // Someone else is compiling it, or it's done. Skip.
            }

            if (req.is_thumb) {
                pw_builder_.BuildBlock<true>(req.pc);
            } else {
                pw_builder_.BuildBlock<false>(req.pc);
            }

#ifdef B_DEBUG
            {
                size_t ir_count = pw_builder_.GetArena()->BytesUsed() / sizeof(IrNode);
                if (ir_count < 20) {
                    std::string dump = IrPrinter::PrintArena(pw_builder_.GetArena(), req.pc);
                    Logger::log("[PreWarmer] " + dump, LogLevel::DEBUG);
                }
            }
#endif

            void* host_code = pw_emitter_.EmitBlock(pw_builder_.GetArena(), req.pc);
            block_map_->PublishIfClaimed(ToBlockMapPc(req.pc), req.is_thumb, host_code);

            for (const auto& target : pw_builder_.GetDiscoveredTargets()) {
                void* ptr = block_map_->Lookup(ToBlockMapPc(target.first), target.second);
                if (ptr == nullptr) {
                    local_spider_queue.push({target.first, target.second});
                }
            }
        } else {
            std::this_thread::yield();
        }
    }
}
