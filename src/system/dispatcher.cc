#include "system/dispatcher.h"
#include "data/block_map.h"
#include "data/ir_node.h"
#include "system/memory_bus.h"
#include "util/ir_printer.h"
#include "util/logger.h"

namespace {
// Normalize PC for BlockMap lookup.
uint32_t ToBlockMapPc(uint32_t pc) {
    return (pc < BlockMap::kRomStart) ? (BlockMap::kRomStart + pc) : pc;
}
}  // namespace

JitDispatcher::JitDispatcher(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue)
    : jit_arena_(1024 * 1024),
      jit_builder_(bus, &jit_arena_),
      jit_emitter_(),
      block_map_(block_map),
      seed_queue_(seed_queue) {}

void JitDispatcher::Run(uint32_t start_pc, bool is_thumb) {
    uint32_t pc = start_pc;

    while (system_running_.load(std::memory_order_relaxed)) {
        uint32_t map_pc = ToBlockMapPc(pc);
        void* host_code = block_map_->Lookup(map_pc, is_thumb);

        bool did_compile = false;
        if (host_code == nullptr || host_code == BlockMap::ClaimedSentinel()) {
            if (is_thumb) {
                jit_builder_.BuildBlock<true>(pc);
            } else {
                jit_builder_.BuildBlock<false>(pc);
            }

            host_code = jit_emitter_.EmitBlock(jit_builder_.GetArena(), pc);
            block_map_->ForcePublish(map_pc, is_thumb, host_code);
            did_compile = true;
        }

        pc = reinterpret_cast<uint32_t (*)(void)>(host_code)();
        is_thumb = (pc & 1);  // ARM interworking: LSB indicates Thumb

        if (did_compile) {
            for (const auto& target : jit_builder_.GetDiscoveredTargets()) {
                if (target.first == pc && target.second == is_thumb) continue;
                if (block_map_->Lookup(ToBlockMapPc(target.first), target.second) == nullptr) {
                    seed_queue_->Push(target.first, target.second);
                }
            }
        }
    }
}

void JitDispatcher::RunSimulated(uint32_t start_pc, bool is_thumb, size_t max_blocks) {
    uint32_t pc = start_pc;
    size_t blocks_done = 0;

    while (system_running_.load(std::memory_order_relaxed) && (max_blocks == 0 || blocks_done < max_blocks)) {
        uint32_t map_pc = ToBlockMapPc(pc);
        void* host_code = block_map_->Lookup(map_pc, is_thumb);

        if (is_thumb) {
            jit_builder_.BuildBlock<true>(pc);
        } else {
            jit_builder_.BuildBlock<false>(pc);
        }

        if (host_code == nullptr || host_code == BlockMap::ClaimedSentinel()) {
#ifdef B_DEBUG
            {
                size_t ir_count = jit_builder_.GetArena()->BytesUsed() / sizeof(IrNode);
                if (ir_count < 20) {
                    std::string dump = IrPrinter::PrintArena(jit_builder_.GetArena(), pc);
                    Logger::log("[JitDispatcher] " + dump, LogLevel::DEBUG);
                }
            }
#endif
            host_code = jit_emitter_.EmitBlock(jit_builder_.GetArena(), pc);
            block_map_->ForcePublish(map_pc, is_thumb, host_code);
            const auto& targets = jit_builder_.GetDiscoveredTargets();
            for (size_t i = 0; i < targets.size(); ++i) {
                if (i == 0) continue;  // we'll compile this one next iteration
                if (block_map_->Lookup(ToBlockMapPc(targets[i].first), targets[i].second) == nullptr) {
                    seed_queue_->Push(targets[i].first, targets[i].second);
                }
            }
        }

        const auto& targets = jit_builder_.GetDiscoveredTargets();
        if (targets.empty()) {
            pc += jit_builder_.GetBlockLength();
        } else {
            pc = targets[0].first;
            is_thumb = targets[0].second;
        }
        blocks_done++;
    }
}
