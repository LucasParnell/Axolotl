#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace BlockHeuristicsData {

struct OptimizationDecision {
    bool prewarm_compile = true;
    bool persist_seed = true;
    bool persist_x86 = false;
    bool has_heuristics = false;
    uint32_t heuristic_score = 0;
};

// Data-oriented block heuristics store:
// - sparse key->index lookup
// - dense SoA vectors for each tracked signal
struct SoaStore {
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    std::unordered_map<uint64_t, uint32_t> key_to_index;
    std::vector<uint64_t> keys;
    std::vector<uint32_t> known_block_cycles;
    std::vector<uint32_t> known_block_len;
    std::vector<uint32_t> known_x86_size;
    std::vector<uint64_t> execution_count;
    std::vector<uint64_t> total_guest_cycles;
    std::vector<uint64_t> compiled_count;

    void Reserve(size_t n) {
        key_to_index.reserve(n);
        keys.reserve(n);
        known_block_cycles.reserve(n);
        known_block_len.reserve(n);
        known_x86_size.reserve(n);
        execution_count.reserve(n);
        total_guest_cycles.reserve(n);
        compiled_count.reserve(n);
    }
};

inline uint64_t MakeBlockKey(uint32_t pc, bool is_thumb) {
    const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
    return (static_cast<uint64_t>(aligned_pc) << 1) | static_cast<uint64_t>(is_thumb ? 1u : 0u);
}

inline uint32_t FindIndex(const SoaStore& s, uint64_t key) {
    const auto it = s.key_to_index.find(key);
    return it == s.key_to_index.end() ? SoaStore::kInvalidIndex : it->second;
}

inline uint32_t UpsertIndex(SoaStore* s, uint64_t key) {
    const auto it = s->key_to_index.find(key);
    if (it != s->key_to_index.end()) return it->second;

    const uint32_t idx = static_cast<uint32_t>(s->keys.size());
    s->key_to_index.emplace(key, idx);
    s->keys.push_back(key);
    s->known_block_cycles.push_back(0);
    s->known_block_len.push_back(0);
    s->known_x86_size.push_back(0);
    s->execution_count.push_back(0);
    s->total_guest_cycles.push_back(0);
    s->compiled_count.push_back(0);
    return idx;
}

inline uint32_t ComputeHeuristicScore(const SoaStore& s, uint32_t idx, uint32_t block_cycles, uint32_t block_len) {
    const uint32_t known_cycles = s.known_block_cycles[idx];
    const uint32_t known_len = s.known_block_len[idx];
    const uint32_t x86_size = s.known_x86_size[idx];
    const uint64_t execs = s.execution_count[idx];
    const uint64_t guest_cycles = s.total_guest_cycles[idx];
    const uint64_t compiles = s.compiled_count[idx];

    const uint32_t effective_cycles = block_cycles != 0 ? block_cycles : known_cycles;
    const uint32_t effective_len = block_len != 0 ? block_len : known_len;
    const uint64_t avg_guest_cycles = execs != 0 ? (guest_cycles / execs) : 0u;

    uint64_t score = 0;
    score += static_cast<uint64_t>(effective_cycles) * 2u;
    score += static_cast<uint64_t>(effective_len);
    score += std::min<uint64_t>(4096u, execs * 24u);
    score += std::min<uint64_t>(1024u, avg_guest_cycles);
    score += std::min<uint64_t>(1024u, static_cast<uint64_t>(x86_size) / 8u);
    score += std::min<uint64_t>(1024u, compiles * 8u);

    return score > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                         : static_cast<uint32_t>(score);
}

inline void RecordCompiled(SoaStore* s, uint64_t key) {
    const uint32_t idx = UpsertIndex(s, key);
    s->compiled_count[idx] += 1;
}

inline void RecordCompiledX86(SoaStore* s,
                              uint64_t key,
                              uint32_t block_cycles,
                              uint32_t block_len,
                              uint32_t x86_size) {
    const uint32_t idx = UpsertIndex(s, key);
    s->known_block_cycles[idx] = block_cycles;
    s->known_block_len[idx] = block_len;
    s->known_x86_size[idx] = x86_size;
    s->compiled_count[idx] += 1;
}

inline void RecordExecutionSample(SoaStore* s, uint64_t key, uint32_t consumed_guest_cycles) {
    const uint32_t idx = UpsertIndex(s, key);
    s->execution_count[idx] += 1;
    s->total_guest_cycles[idx] += consumed_guest_cycles;
}

inline OptimizationDecision Decide(const SoaStore& s, uint64_t key, uint32_t block_cycles, uint32_t block_len) {
    OptimizationDecision out{};
    constexpr uint32_t kCompileThreshold = 32u;
    constexpr uint32_t kPersistX86Threshold = 96u;
    constexpr uint32_t kTinyCycles = 3u;
    constexpr uint32_t kTinyLenBytes = 8u;
    constexpr uint32_t kHotExecThreshold = 8u;
    constexpr uint32_t kVeryHotExecThreshold = 24u;
    constexpr uint32_t kHeavyCyclesThreshold = 24u;
    constexpr uint32_t kHeavyLenThreshold = 48u;

    const uint32_t idx = FindIndex(s, key);
    if (idx == SoaStore::kInvalidIndex) {
        const bool have_shape = block_cycles != 0 && block_len != 0;
        const bool tiny_block = have_shape &&
                                block_cycles <= kTinyCycles &&
                                block_len <= kTinyLenBytes;
        const bool heavy_block = block_cycles >= kHeavyCyclesThreshold || block_len >= kHeavyLenThreshold;
        out.heuristic_score = static_cast<uint32_t>(
            std::min<uint64_t>(0xFFFFFFFFu, static_cast<uint64_t>(block_cycles) * 4u +
                                              static_cast<uint64_t>(block_len) * 2u));
        out.prewarm_compile = !tiny_block;
        out.persist_seed = !tiny_block;
        // Unknown blocks should only be x86-persisted when they are clearly heavy.
        out.persist_x86 = heavy_block && (block_cycles >= (kHeavyCyclesThreshold + 8u) ||
                                          block_len >= (kHeavyLenThreshold + 16u));
        return out;
    }

    const uint32_t known_cycles = s.known_block_cycles[idx];
    const uint32_t known_len = s.known_block_len[idx];
    const uint32_t x86_size = s.known_x86_size[idx];
    const uint64_t execs = s.execution_count[idx];
    out.has_heuristics = true;
    out.heuristic_score = ComputeHeuristicScore(s, idx, block_cycles, block_len);

    const uint32_t effective_cycles = block_cycles != 0 ? block_cycles : known_cycles;
    const uint32_t effective_len = block_len != 0 ? block_len : known_len;
    const bool tiny_block = (effective_cycles != 0 && effective_len != 0 &&
                             effective_cycles <= kTinyCycles && effective_len <= kTinyLenBytes);
    const bool had_runtime_hits = execs > 0;
    const bool hot_block = execs >= kHotExecThreshold;
    const bool very_hot_block = execs >= kVeryHotExecThreshold;
    const bool heavy_block = (effective_cycles >= kHeavyCyclesThreshold) || (effective_len >= kHeavyLenThreshold);

    // Tiny+cold blocks are low-value for persistence and prewarm budget.
    if (tiny_block && execs < 3 && x86_size == 0) {
        out.prewarm_compile = false;
        out.persist_seed = false;
        out.persist_x86 = false;
        return out;
    }

    // Heavy/hot blocks are explicitly prioritized.
    if (hot_block || heavy_block) {
        out.prewarm_compile = true;
        out.persist_seed = true;
        out.persist_x86 = very_hot_block || out.heuristic_score >= (kPersistX86Threshold / 2u) || x86_size != 0;
        return out;
    }

    if (tiny_block && !had_runtime_hits && x86_size == 0) {
        out.prewarm_compile = false;
    } else {
        out.prewarm_compile = out.heuristic_score >= kCompileThreshold || had_runtime_hits || x86_size != 0;
    }
    out.persist_seed = !tiny_block || hot_block || x86_size != 0;
    out.persist_x86 = out.heuristic_score >= kPersistX86Threshold || execs >= 2 || x86_size != 0;
    return out;
}

}  // namespace BlockHeuristicsData
