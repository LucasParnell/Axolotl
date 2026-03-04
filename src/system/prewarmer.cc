#include "system/prewarmer.h"
#include "data/block_map.h"
#include "system/block_compile_cache.h"
#include "system/memory_bus.h"
#include "util/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <queue>
#include <sstream>
#include <thread>
#include <cstdint>
#include <vector>

#ifndef PREWARMER_VERBOSE_LOGS
#define PREWARMER_VERBOSE_LOGS 0
#endif

#ifndef PREWARMER_SUMMARY_LOGS
#define PREWARMER_SUMMARY_LOGS 0
#endif

namespace {

size_t ComputePrewarmCodeCacheBytes(const MemoryBus* bus) {
    constexpr size_t kBiosBytes = 16 * 1024;
    constexpr size_t kMiB = 1024 * 1024;
    // Keep default prewarm footprint modest; this buffer is reserved up-front.
    constexpr size_t kMinCacheBytes = 24 * kMiB;
    constexpr size_t kMaxCacheBytes = 192 * kMiB;
    constexpr size_t kCodeToInputMultiplier = 3;
    constexpr size_t kFixedHeadroomBytes = 8 * kMiB;

    if (const char* env_mb = std::getenv("AXOLOTL_PREWARM_CACHE_MB")) {
        const unsigned long long mb = std::strtoull(env_mb, nullptr, 10);
        if (mb > 0) {
            constexpr size_t kMaxEnvCacheBytes = 1024 * kMiB;
            constexpr size_t kMinEnvCacheBytes = 8 * kMiB;
            const size_t requested =
                std::clamp<size_t>(static_cast<size_t>(mb) * kMiB, kMinEnvCacheBytes, kMaxEnvCacheBytes);
            return requested;
        }
    }

    const size_t rom_bytes = static_cast<size_t>(bus->GetRomLoadedSize());
    const size_t input_bytes = rom_bytes + kBiosBytes;
    const size_t target_bytes =
        (input_bytes * kCodeToInputMultiplier) + kFixedHeadroomBytes;
    return std::clamp(target_bytes, kMinCacheBytes, kMaxCacheBytes);
}

size_t ComputePrewarmFillStopPercent() {
    constexpr size_t kDefaultPct = 85;
    constexpr size_t kMinPct = 50;
    constexpr size_t kMaxPct = 99;
    if (const char* env_pct = std::getenv("AXOLOTL_PREWARM_CACHE_FILL_PCT")) {
        const unsigned long long pct = std::strtoull(env_pct, nullptr, 10);
        if (pct > 0) {
            return std::clamp<size_t>(static_cast<size_t>(pct), kMinPct, kMaxPct);
        }
    }
    return kDefaultPct;
}

size_t ComputePrewarmLeadHighBlocks() {
    constexpr size_t kDefault = 4096;
    constexpr size_t kMin = 256;
    constexpr size_t kMax = 1u << 20;
    if (const char* env = std::getenv("AXOLOTL_PREWARM_LEAD_HIGH")) {
        const unsigned long long v = std::strtoull(env, nullptr, 10);
        if (v > 0) return std::clamp<size_t>(static_cast<size_t>(v), kMin, kMax);
    }
    return kDefault;
}

size_t ComputePrewarmLeadLowBlocks(size_t high) {
    constexpr size_t kMin = 128;
    size_t fallback = std::max<size_t>(kMin, high / 2);
    if (fallback >= high) fallback = high - 1;
    if (const char* env = std::getenv("AXOLOTL_PREWARM_LEAD_LOW")) {
        const unsigned long long v = std::strtoull(env, nullptr, 10);
        if (v > 0) {
            size_t low = static_cast<size_t>(v);
            if (low >= high) low = high - 1;
            return std::max<size_t>(kMin, low);
        }
    }
    return fallback;
}

bool ComputePrewarmSpiderEnabled() {
    // Default on: prewarmer should proactively spider reachable targets.
    if (const char* env = std::getenv("AXOLOTL_PREWARM_SPIDER_ENABLE")) {
        return std::strtoull(env, nullptr, 10) != 0;
    }
    return true;
}

bool ComputePrewarmSpiderAllowRam() {
    // Default off: RAM contains frequent data regions and dynamic pointers that
    // cause speculative spidering to decode garbage.
    if (const char* env = std::getenv("AXOLOTL_PREWARM_SPIDER_ALLOW_RAM")) {
        return std::strtoull(env, nullptr, 10) != 0;
    }
    return false;
}

bool ComputePrewarmUseBlockCacheLocks() {
    // Default off: keep prewarmer lock-free and avoid mutex contention on shared heuristics.
    if (const char* env = std::getenv("AXOLOTL_PREWARM_USE_BLOCK_CACHE_LOCKS")) {
        return std::strtoull(env, nullptr, 10) != 0;
    }
    return false;
}

size_t ComputePrewarmSpiderDepthLimit() {
    constexpr size_t kDefault = 32;
    constexpr size_t kMin = 1;
    constexpr size_t kMax = 4096;
    if (const char* env = std::getenv("AXOLOTL_PREWARM_SPIDER_DEPTH_LIMIT")) {
        const unsigned long long v = std::strtoull(env, nullptr, 10);
        if (v > 0) return std::clamp<size_t>(static_cast<size_t>(v), kMin, kMax);
    }
    return kDefault;
}

size_t EstimatePrewarmEmitBytes(uint32_t block_cycles, uint32_t block_len, bool is_thumb) {
    const uint32_t instr_bytes = is_thumb ? 2u : 4u;
    const uint32_t instr_count = std::max<uint32_t>(1u, block_len / instr_bytes);
    // Conservative estimate: base frame/save overhead + per-op expansion + memory/branch pessimism from cycles.
    uint64_t estimated = 1024u;
    estimated += static_cast<uint64_t>(instr_count) * 320u;
    estimated += static_cast<uint64_t>(block_cycles) * 20u;
    if (estimated < 4096u) estimated = 4096u;
    if (estimated > (128u * 1024u)) estimated = 128u * 1024u;
    return static_cast<size_t>(estimated);
}

uint32_t EstimatePrewarmHeuristicScore(uint32_t block_cycles, uint32_t block_len) {
    // Cheap lock-free estimate used by prewarmer admission/replacement.
    uint64_t score = static_cast<uint64_t>(block_cycles) * 4u +
                     static_cast<uint64_t>(block_len) * 2u;
    if (score > 0xFFFFFFFFu) score = 0xFFFFFFFFu;
    return static_cast<uint32_t>(score);
}

}  // namespace

PreWarmer::PreWarmer(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue,
                     PrewarmPacingState* prewarm_pacing,
                     BlockCompileCache* block_cache)
    : pw_arena_(1024 * 1024),
      pw_builder_(bus, &pw_arena_),
      pw_code_cache_bytes_(ComputePrewarmCodeCacheBytes(bus)),
      pw_emitter_(pw_code_cache_bytes_),
      block_map_(block_map),
      seed_queue_(seed_queue),
      prewarm_pacing_(prewarm_pacing),
      block_cache_(block_cache) {
    std::ostringstream msg;
    msg << "[PreWarmer] code cache budget: " << (pw_code_cache_bytes_ / (1024 * 1024))
        << " MB (ROM " << (bus->GetRomLoadedSize() / 1024) << " KB + BIOS 16 KB)";
    Logger::log(msg.str(), LogLevel::INFO);
}

void PreWarmer::ThreadLoop() {
    struct SpiderItem {
        CompileTarget target;
        size_t depth;
    };
    std::queue<SpiderItem> local_spider_queue;
    CompileTarget seed;
    size_t blocks_warmed_in_burst = 0;
    size_t seeds_in_burst = 0;
    size_t skips_claimed = 0;
    const size_t kBatchLogThreshold = 3;  // show per-block log for first N per burst
    const size_t kPrewarmCodeBufferBytes = pw_code_cache_bytes_;
    const size_t kFillStopPct = ComputePrewarmFillStopPercent();
    const size_t kPrewarmSoftLimitBytes = (kPrewarmCodeBufferBytes * kFillStopPct) / 100;
    const size_t kEmitSafetyReserveBytes =
        std::max<size_t>(4 * 1024 * 1024, kPrewarmCodeBufferBytes / 25);
    const size_t kPrewarmHardLimitBytes =
        kPrewarmCodeBufferBytes - std::max<size_t>(2 * 1024 * 1024, kPrewarmCodeBufferBytes / 50);
    constexpr uint32_t kHighValueScoreThreshold = 192u;
    const size_t kLeadHighBlocks = ComputePrewarmLeadHighBlocks();
    const size_t kLeadLowBlocks = ComputePrewarmLeadLowBlocks(kLeadHighBlocks);
    bool prewarm_soft_warned = false;
    bool prewarm_hard_fit_warned = false;
    bool prewarm_hard_priority_warned = false;
    bool prewarm_lead_paused = false;
    bool prewarm_lead_config_logged = false;
    const bool prewarm_spider_enabled = ComputePrewarmSpiderEnabled();
    const bool prewarm_spider_allow_ram = ComputePrewarmSpiderAllowRam();
    const size_t prewarm_spider_depth_limit = ComputePrewarmSpiderDepthLimit();
    const bool prewarm_use_block_cache_locks = ComputePrewarmUseBlockCacheLocks();
    bool prewarm_spider_config_logged = false;
    uint32_t idle_spins = 0;
    const uint32_t rom_loaded_size = pw_builder_.GetBus()->GetRomLoadedSize();
    auto IsSpiderTargetAllowed = [&](uint32_t pc_aligned) {
        if (!BlockCacheData::IsCacheableExecPcWithRomSize(pc_aligned, rom_loaded_size)) return false;
        if (prewarm_spider_allow_ram) return true;
        const uint8_t region = static_cast<uint8_t>(pc_aligned >> 24);
        // Keep recursive spidering in stable code regions by default.
        return region == 0x00 || (region >= 0x08 && region <= 0x0D);
    };
    struct PrewarmedBlockMeta {
        uint32_t pc = 0;
        bool is_thumb = false;
        void* host_code = nullptr;
        uint32_t score = 0;
        bool live = false;
    };
    struct PrewarmedMinEntry {
        uint32_t score = 0;
        size_t meta_index = 0;
    };
    struct PrewarmedMinCompare {
        bool operator()(const PrewarmedMinEntry& a, const PrewarmedMinEntry& b) const {
            return a.score > b.score;  // min-heap by score
        }
    };
    std::vector<PrewarmedBlockMeta> prewarmed_meta;
    prewarmed_meta.reserve(1u << 15);
    std::priority_queue<PrewarmedMinEntry,
                        std::vector<PrewarmedMinEntry>,
                        PrewarmedMinCompare>
        prewarmed_min_heap;
    auto RememberPrewarmedBlock = [&](uint32_t pc, bool is_thumb, void* host_code, uint32_t score) {
        PrewarmedBlockMeta meta{};
        meta.pc = pc;
        meta.is_thumb = is_thumb;
        meta.host_code = host_code;
        meta.score = score;
        meta.live = true;
        const size_t idx = prewarmed_meta.size();
        prewarmed_meta.push_back(meta);
        prewarmed_min_heap.push(PrewarmedMinEntry{score, idx});
    };
    auto EvictLowScorePrewarmed = [&](uint32_t min_score_to_keep, size_t max_to_evict) -> size_t {
        size_t evicted = 0;
        while (evicted < max_to_evict && !prewarmed_min_heap.empty()) {
            const PrewarmedMinEntry e = prewarmed_min_heap.top();
            prewarmed_min_heap.pop();
            if (e.meta_index >= prewarmed_meta.size()) continue;
            PrewarmedBlockMeta& m = prewarmed_meta[e.meta_index];
            if (!m.live || m.score != e.score) continue;  // stale heap entry
            if (m.score >= min_score_to_keep) break;
            void* current = block_map_->Lookup(m.pc, m.is_thumb);
            if (current == m.host_code) {
                block_map_->ForcePublish(m.pc, m.is_thumb, nullptr);
                ++evicted;
            }
            m.live = false;
        }
        return evicted;
    };

    while (running_.load(std::memory_order_relaxed)) {
        if (prewarm_pacing_) {
            const uint64_t warmed =
                prewarm_pacing_->prewarmer_blocks_warmed.load(std::memory_order_relaxed);
            const uint64_t executed =
                prewarm_pacing_->dispatcher_blocks_executed.load(std::memory_order_relaxed);
            const uint64_t lead = (warmed > executed) ? (warmed - executed) : 0;

            if (!prewarm_lead_config_logged) {
                std::ostringstream cfg;
                cfg << "[PreWarmer] lead pacing: high=" << kLeadHighBlocks
                    << " low=" << kLeadLowBlocks;
                Logger::log(cfg.str(), LogLevel::INFO);
                prewarm_lead_config_logged = true;
            }
            if (!prewarm_spider_config_logged) {
                Logger::log(std::string("[PreWarmer] spidering: ") +
                                (prewarm_spider_enabled ? "enabled" : "disabled") +
                                (prewarm_spider_enabled
                                     ? (prewarm_spider_allow_ram ? " (RAM enabled)" : " (RAM disabled)")
                                     : ""),
                            LogLevel::INFO);
                Logger::log(std::string("[PreWarmer] block-cache locks: ") +
                                (prewarm_use_block_cache_locks ? "enabled" : "disabled"),
                            LogLevel::INFO);
                if (prewarm_spider_enabled) {
                    Logger::log("[PreWarmer] spider depth limit: " +
                                    std::to_string(prewarm_spider_depth_limit),
                                LogLevel::INFO);
                }
                prewarm_spider_config_logged = true;
            }

            if (!prewarm_lead_paused && lead >= kLeadHighBlocks) {
                prewarm_lead_paused = true;
                std::ostringstream msg;
                msg << "[PreWarmer] pausing (lead " << lead
                    << " blocks ahead of dispatcher)";
                Logger::log(msg.str(), LogLevel::INFO);
            } else if (prewarm_lead_paused && lead <= kLeadLowBlocks) {
                prewarm_lead_paused = false;
                std::ostringstream msg;
                msg << "[PreWarmer] resuming (lead " << lead << " blocks)";
                Logger::log(msg.str(), LogLevel::INFO);
            }

            if (prewarm_lead_paused) {
                ++idle_spins;
                if (idle_spins < 1024u) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                continue;
            }
        }

        size_t popped = 0;
        while (seed_queue_->Pop(seed)) {
            local_spider_queue.push({seed, 0});
            ++popped;
        }
        seeds_in_burst += popped;

        if (!local_spider_queue.empty()) {
            idle_spins = 0;
            SpiderItem req_item = local_spider_queue.front();
            local_spider_queue.pop();
            CompileTarget req = req_item.target;
            const uint32_t req_pc_aligned = req.pc & (req.is_thumb ? ~1u : ~3u);
            if (!BlockCacheData::IsCacheableExecPcWithRomSize(req_pc_aligned, rom_loaded_size)) {
                continue;
            }

            BlockCompileCache::OptimizationDecision decision{};
            if (prewarm_use_block_cache_locks && block_cache_) {
                decision = block_cache_->DecideOptimization(req.pc, req.is_thumb, 0, 0);
                if (decision.has_heuristics && !decision.prewarm_compile) {
                    if (PREWARMER_VERBOSE_LOGS) {
                        std::ostringstream msg;
                        msg << "[PreWarmer] skipped by heuristics 0x" << std::hex << req.pc
                            << (req.is_thumb ? " T" : " A")
                            << " score=" << std::dec << decision.heuristic_score;
                        Logger::log(msg.str(), LogLevel::INFO);
                    }
                    continue;
                }
            }

            if (!block_map_->TryClaimIfEmpty(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer)) {
                ++skips_claimed;
                continue;  // Someone else is compiling it, or it's done. Skip.
            }

            // Normalize PC for fetch: Thumb may have bit 0 set from static branch targets.
            uint32_t build_pc = req.pc & (req.is_thumb ? ~1u : ~3u);
            if (req.is_thumb) {
                pw_builder_.BuildBlock<true>(build_pc);
            } else {
                pw_builder_.BuildBlock<false>(build_pc);
            }
            if (prewarm_use_block_cache_locks && block_cache_) {
                decision = block_cache_->DecideOptimization(req.pc,
                                                            req.is_thumb,
                                                            pw_builder_.GetBlockCycles(),
                                                            pw_builder_.GetBlockLength());
                if (!decision.prewarm_compile) {
                    block_map_->AbandonClaimIfOwned(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer);
                    continue;
                }
            } else {
                decision.prewarm_compile = true;
                decision.persist_seed = false;
                decision.persist_x86 = false;
                decision.heuristic_score = EstimatePrewarmHeuristicScore(
                    pw_builder_.GetBlockCycles(), pw_builder_.GetBlockLength());
            }

            const size_t code_used = pw_emitter_.GetBytesUsed();
            const size_t estimated_emit_bytes = EstimatePrewarmEmitBytes(
                pw_builder_.GetBlockCycles(), pw_builder_.GetBlockLength(), req.is_thumb);
            constexpr size_t kMaxEstimatedBlockEmitBytes = 60u * 1024u;
            if (estimated_emit_bytes >= kMaxEstimatedBlockEmitBytes) {
                block_map_->AbandonClaimIfOwned(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer);
                continue;
            }
            const bool near_soft_limit =
                (code_used + kEmitSafetyReserveBytes >= kPrewarmSoftLimitBytes) ||
                (code_used + estimated_emit_bytes + kEmitSafetyReserveBytes >= kPrewarmSoftLimitBytes);
            const size_t projected_used = code_used + estimated_emit_bytes;
            const bool over_hard_fit =
                (projected_used + kEmitSafetyReserveBytes >= kPrewarmHardLimitBytes);
            const bool hard_pressure =
                (code_used + kEmitSafetyReserveBytes >= kPrewarmHardLimitBytes);
            if (near_soft_limit || hard_pressure) {
                const uint32_t candidate_score = decision.heuristic_score;
                if (candidate_score > 0) {
                    const uint32_t margin = hard_pressure ? 64u : 32u;
                    const uint32_t min_score_to_keep =
                        candidate_score > margin ? (candidate_score - margin) : 0u;
                    const size_t evict_budget = hard_pressure ? 16u : 4u;
                    const size_t evicted = EvictLowScorePrewarmed(min_score_to_keep, evict_budget);
                    if (evicted != 0 && PREWARMER_VERBOSE_LOGS) {
                        std::ostringstream msg;
                        msg << "[PreWarmer] evicted " << evicted
                            << " low-score prewarmed blocks for candidate score="
                            << candidate_score;
                        Logger::log(msg.str(), LogLevel::INFO);
                    }
                }
            }

            if (over_hard_fit) {
                if (!prewarm_hard_fit_warned) {
                    std::ostringstream warn;
                    warn << "[PreWarmer] hard cache fit limit reached; skipping blocks that cannot fit";
                    Logger::log(warn.str(), LogLevel::WARNING);
                    prewarm_hard_fit_warned = true;
                }
                block_map_->AbandonClaimIfOwned(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer);
                continue;
            }
            if (near_soft_limit) {
                const bool high_value = decision.persist_x86 || decision.heuristic_score >= kHighValueScoreThreshold;
                if (!high_value) {
                    if (!prewarm_soft_warned) {
                        std::ostringstream warn;
                        warn << "[PreWarmer] reached soft watermark (" << (kFillStopPct)
                             << "%); prioritizing high-value blocks only";
                        Logger::log(warn.str(), LogLevel::WARNING);
                        prewarm_soft_warned = true;
                    }
                    block_map_->AbandonClaimIfOwned(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer);
                    continue;
                }
            }
            if (hard_pressure) {
                constexpr uint32_t kVeryHighValueScoreThreshold = 320u;
                const bool very_high_value =
                    decision.persist_x86 || decision.heuristic_score >= kVeryHighValueScoreThreshold;
                if (!very_high_value) {
                    if (!prewarm_hard_priority_warned) {
                        std::ostringstream warn;
                        warn << "[PreWarmer] hard cache pressure; admitting very-high-value blocks only";
                        Logger::log(warn.str(), LogLevel::WARNING);
                        prewarm_hard_priority_warned = true;
                    }
                    block_map_->AbandonClaimIfOwned(req.pc, req.is_thumb, BlockMap::ClaimOwner::kPrewarmer);
                    continue;
                }
            }

            CodeEmitter::EmittedBlockArtifact emitted = pw_emitter_.EmitBlockWithRelocs(
                pw_builder_.GetArena(),
                build_pc,
                pw_builder_.GetBlockCycles(),
                req.is_thumb,
                pw_builder_.GetBlockLength());
            void* host_code = emitted.host_code;

            const bool published = block_map_->PublishIfClaimedBy(req.pc, req.is_thumb,
                                                                  BlockMap::ClaimOwner::kPrewarmer,
                                                                  host_code);
            if (published) {
                RememberPrewarmedBlock(req.pc, req.is_thumb, host_code, decision.heuristic_score);
            } else {
                // Dispatcher stole claim and published first; keep its canonical pointer.
            }
            if (published && prewarm_use_block_cache_locks && block_cache_) {
                if (decision.persist_seed) {
                    block_cache_->RecordCompiled(req.pc, req.is_thumb);
                }
                if (decision.persist_x86) {
                    std::vector<BlockCacheData::X86RelocEntry> reloc_entries;
                    reloc_entries.reserve(emitted.relocs.size());
                    for (const auto& r : emitted.relocs) {
                        BlockCacheData::X86RelocEntry re{};
                        re.offset = r.offset;
                        re.type = static_cast<uint32_t>(r.type);
                        re.symbol_id = static_cast<uint32_t>(r.symbol);
                        reloc_entries.push_back(re);
                    }
                    block_cache_->RecordCompiledX86(req.pc,
                                                    req.is_thumb,
                                                    emitted.x86_bytes.data(),
                                                    emitted.x86_bytes.size(),
                                                    reloc_entries.data(),
                                                    reloc_entries.size(),
                                                    emitted.block_cycles,
                                                    emitted.block_len,
                                                    emitted.x86_crc32);
                } else if (PREWARMER_VERBOSE_LOGS) {
                    std::ostringstream msg;
                    msg << "[PreWarmer] x86-store skipped by heuristics 0x" << std::hex << req.pc
                        << (req.is_thumb ? " T" : " A")
                        << " score=" << std::dec << decision.heuristic_score;
                    Logger::log(msg.str(), LogLevel::INFO);
                }
            }
            ++blocks_warmed_in_burst;
            if (prewarm_pacing_) {
                prewarm_pacing_->prewarmer_blocks_warmed.fetch_add(1, std::memory_order_relaxed);
            }

            if (PREWARMER_VERBOSE_LOGS && blocks_warmed_in_burst <= kBatchLogThreshold) {
                std::ostringstream msg;
                msg << "[PreWarmer] warmed 0x" << std::hex << req.pc << (req.is_thumb ? " T" : " A");
                Logger::log(msg.str(), LogLevel::INFO);
            }

            if (prewarm_spider_enabled) {
                if (req_item.depth >= prewarm_spider_depth_limit) {
                    continue;
                }
                for (const auto& target : pw_builder_.GetDiscoveredTargets()) {
                    const uint32_t target_pc_aligned = target.first & (target.second ? ~1u : ~3u);
                    if (!IsSpiderTargetAllowed(target_pc_aligned)) {
                        continue;
                    }
                    void* ptr = block_map_->Lookup(target.first, target.second);
                    if (ptr == nullptr) {
                        local_spider_queue.push({{target.first, target.second}, req_item.depth + 1});
                    }
                }
            }
        } else {
            // Burst ended — emit one summary line
            if (PREWARMER_SUMMARY_LOGS && (blocks_warmed_in_burst > 0 || skips_claimed > 0)) {
                std::ostringstream msg;
                msg << "[PreWarmer] " << blocks_warmed_in_burst << " blocks warmed";
                if (seeds_in_burst > 0) msg << ", " << seeds_in_burst << " seeds";
                if (skips_claimed > 0)  msg << ", " << skips_claimed << " skipped";
                Logger::log(msg.str(), LogLevel::INFO);
            }
            blocks_warmed_in_burst = 0;
            seeds_in_burst = 0;
            skips_claimed = 0;
            ++idle_spins;
            if (idle_spins < 1024u) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    }
}
