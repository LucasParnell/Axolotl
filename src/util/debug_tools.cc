//
// util/debug_tools.cc — Debug watchpoints and execution tracing
//

#include "util/debug_tools.h"

#ifdef B_DEBUG

#include <algorithm>
#include <iostream>
#include <sstream>

// ── Internal helper ────────────────────────────────────────────────────────

static uint64_t MakeKey(uint32_t address, bool is_thumb) {
    return (static_cast<uint64_t>(address) << 1) | (is_thumb ? 1u : 0u);
}

// ── Watchpoint queries ─────────────────────────────────────────────────────

bool ShouldPauseOnIOOffset(const WatchpointSet& wp, uint32_t offset) {
    return wp.pause_on_io_write && wp.io_offsets.count(offset);
}

bool ShouldPauseOnBGWrite(const WatchpointSet& wp, uint32_t base_offset) {
    if (wp.pause_on_bg_write) return true;
    if (wp.pause_on_io_write &&
        (wp.io_offsets.count(base_offset) || wp.io_offsets.count(base_offset + 2)))
        return true;
    return false;
}

bool ShouldPauseOnSWI(const WatchpointSet& wp, uint32_t swi_number) {
    return wp.pause_on_swi && wp.swi_numbers.count(swi_number);
}

bool ShouldPauseOnBlock(const WatchpointSet& wp, uint32_t block_start) {
    return wp.block_addresses.count(block_start) != 0;
}

// ── Backtrace ring buffer ──────────────────────────────────────────────────
//
// RecordBlockExecution just writes one slot.  All complexity lives in the
// read path (GetBacktrace) which runs at human speed from the console thread.

static void PushRing(TraceState& ts, const TraceBlock& blk) {
    // Drop very long self-loops from the ring after a cap so older history
    // remains visible in backtrace output.
    if (ts.ring_fill > 0 &&
        ts.ring_last.address == blk.address &&
        ts.ring_last.is_thumb == blk.is_thumb) {
        if (ts.ring_last_run_len >= TraceState::kBacktraceLoopCap) {
            return;
        }
        ts.ring_last_run_len++;
    } else {
        ts.ring_last = blk;
        ts.ring_last_run_len = 1;
    }

    ts.ring[ts.ring_head] = blk;
    ts.ring_head = (ts.ring_head + 1) & (TraceState::kRingSize - 1);
    if (ts.ring_fill < TraceState::kRingSize) ts.ring_fill++;
}

// ── Loop detection ─────────────────────────────────────────────────────────
//
// Scans the recent-key ring for back-edges.  When the same loop repeats
// kLoopIterCap times it is recorded and printed.

static void UpdateLoopDetection(TraceState& ts, uint32_t address, bool is_thumb) {
    const uint64_t key = MakeKey(address, is_thumb);

    if (!ts.loop_body_keys.empty()) {
        // Inside a candidate loop — verify the next key matches.
        if (key != ts.loop_body_keys[ts.loop_next_index]) {
            // Candidate broken; reset and restart detection from this key.
            ts.loop_body_keys.clear();
            ts.loop_next_index = 0;
            ts.loop_iter_count = 0;
            ts.recent_keys.clear();
            ts.recent_keys.push_back(key);
            return;
        }
        ts.loop_next_index++;
        if (ts.loop_next_index >= ts.loop_body_keys.size()) {
            ts.loop_next_index = 0;
            ts.loop_iter_count++;
            if (ts.loop_iter_count >= TraceState::kLoopIterCap) {
                // Commit this loop as a recorded loop.
                LoopBody body;
                body.reserve(ts.loop_body_keys.size());
                for (uint64_t k : ts.loop_body_keys)
                    body.push_back({ static_cast<uint32_t>(k >> 1), (k & 1) != 0 });

                {
                    std::lock_guard<std::mutex> lock(ts.mtx);
                    ts.recorded_loops.push_back(std::move(body));
                }

                std::ostringstream os;
                os << "[LoopTrace] Recorded loop after " << TraceState::kLoopIterCap
                   << " iterations (" << ts.loop_body_keys.size() << " blocks)";
                std::cout << os.str() << std::endl;

                ts.loop_body_keys.clear();
                ts.loop_next_index = 0;
                ts.loop_iter_count = 0;
                ts.recent_keys.clear();
                ts.recent_keys.push_back(key);
            }
        }
        return;
    }

    // Append to the recent key ring.
    ts.recent_keys.push_back(key);
    if (ts.recent_keys.size() > TraceState::kMaxRecentBlocks)
        ts.recent_keys.erase(ts.recent_keys.begin());

    // Check for a back-edge in the recent window.
    const size_t n = ts.recent_keys.size();
    if (n < 2) return;
    for (size_t i = 0; i < n - 1; ++i) {
        if (ts.recent_keys[i] == key) {
            // Everything from i to n-2 forms the candidate loop body.
            ts.loop_body_keys.assign(ts.recent_keys.begin() + i, ts.recent_keys.end() - 1);
            if (ts.loop_body_keys.empty()) continue;
            ts.loop_next_index = 1;
            ts.loop_iter_count = 1;
            return;
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

void RecordBlockExecution(TraceState& ts, uint32_t address, bool is_thumb) {
    const TraceBlock blk = { address, is_thumb };
    {
        std::lock_guard<std::mutex> lock(ts.mtx);
        PushRing(ts, blk);
    }
    if (ts.loop_tracing_enabled)
        UpdateLoopDetection(ts, address, is_thumb);
}

std::vector<BacktraceEntry> GetBacktrace(const TraceState& ts) {
    std::lock_guard<std::mutex> lock(ts.mtx);

    if (ts.ring_fill == 0) return {};

    // Read the ring oldest-first into a flat array.
    // oldest slot = (ring_head - ring_fill) mod kRingSize
    const size_t n    = ts.ring_fill;
    const size_t mask = TraceState::kRingSize - 1;
    const size_t base = (ts.ring_head - n) & mask;

    // Collapse consecutive identical blocks into (block, count) entries.
    std::vector<BacktraceEntry> collapsed;
    collapsed.reserve(std::min(n, TraceState::kBacktraceDepth * 4));
    for (size_t i = 0; i < n; ++i) {
        const TraceBlock& blk = ts.ring[(base + i) & mask];
        if (!collapsed.empty() &&
            collapsed.back().block.address == blk.address &&
            collapsed.back().block.is_thumb == blk.is_thumb) {
            collapsed.back().count++;
        } else {
            collapsed.push_back({ blk, 1 });
        }
    }

    // Keep only the most recent kBacktraceDepth distinct entries.
    if (collapsed.size() > TraceState::kBacktraceDepth) {
        collapsed.erase(collapsed.begin(),
                        collapsed.begin() + (collapsed.size() - TraceState::kBacktraceDepth));
    }
    return collapsed;
}

BacktraceState GetBacktraceState(const TraceState& ts) {
    // Legacy: return a minimal BacktraceState derived from the ring so
    // existing callers don't need to change.
    auto bt = GetBacktrace(ts);  // already locks inside
    BacktraceState st;
    for (const auto& e : bt)
        st.path.push_back(e.block);
    return st;
}

std::vector<LoopBody> GetRecordedLoopsSnapshot(const TraceState& ts) {
    std::lock_guard<std::mutex> lock(ts.mtx);
    return ts.recorded_loops;
}

#endif  // B_DEBUG
