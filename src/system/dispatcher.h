#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "data/arena_alloc.h"
#include "data/block_profile_data.h"
#include "data/cpu_state.h"
#include "system/gba_timing.h"
#include "system/jit_emitter.h"
#include "system/jit_lifter.h"
#include "system/prewarm_pacing.h"
#include "system/seed_queue.h"

#ifdef B_DEBUG
#include "util/debug_tools.h"
#endif

class BlockMap;
class MemoryBus;
class BlockCompileCache;

// Callbacks set by the owner of the dispatcher (typically main.cc / system).
struct TimingCallbacks {
    // Called when HBLANK starts (cycle 1006 of a visible scanline).
    // Fired from the JIT thread — must be lock-free or post to an atomic flag.
    std::function<void(int scanline)> on_hblank;

    // Called when a new scanline begins (cycle 0 of every scanline, including vblank).
    std::function<void(int scanline)> on_scanline_start;

    // Called when VBLANK starts (after scanline 159 completes).
    std::function<void()> on_vblank;

    // Called whenever CPU cycles have been committed to timing.
    // Fired from the JIT thread.
    std::function<void(uint32_t cpu_cycles)> on_cycles_advanced;
};

// Main execution thread: compile on miss, push targets to PreWarmer.
class JitDispatcher {
 public:
    JitDispatcher(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue,
                  PrewarmPacingState* prewarm_pacing,
                  BlockCompileCache* block_cache = nullptr);

    void Run(uint32_t start_pc, bool is_thumb);

    void RunSimulated(uint32_t start_pc, bool is_thumb, size_t max_blocks);

    /** Write collected block dumps (ARM, IR, x86 + meta) to path. Call after run. */
    void DumpAnalysisFiles(const std::string& path) const;

    /** Print last executed block IR (PC + thumb + IR text). No-op in release; only in debug build. */
    void PrintLastBlockIfDebug();

    /** If DISASM_LAST_BLOCK: run Python+Capstone to disassemble last block ARM and x86; print before exit. */
    void DisasmLastBlockIfEnabled();

    void Stop() { system_running_.store(false, std::memory_order_relaxed); }
    bool Running() const { return system_running_.load(std::memory_order_relaxed); }
    void SetHostPaused(bool paused) { host_pause_requested_.store(paused, std::memory_order_release); }
    bool HostPaused() const { return host_pause_active_.load(std::memory_order_acquire); }

    void SetTimingCallbacks(TimingCallbacks cb) { timing_callbacks_ = std::move(cb); }

    // ── Debug API (compiled away entirely in release) ──────────────────
#ifdef B_DEBUG
    void SetDebugState(WatchpointSet* wp, TraceState* ts) {
        watchpoints_ = wp;
        trace_       = ts;
    }
    std::atomic<bool>& PauseFlag() { return pause_requested_; }

    void RequestPause()         { pause_requested_.store(true, std::memory_order_release); }
    void Resume()               { step_count_.store(0, std::memory_order_release);
                                  paused_.store(false, std::memory_order_release); }
    void StepBlocks(uint32_t n) { step_count_.store(static_cast<int32_t>(n), std::memory_order_release);
                                  paused_.store(false, std::memory_order_release); }

    void PrintIrForBlock(uint32_t pc);
    void DisasmBlockAtAddress(uint32_t pc);
    const std::vector<BlockDump>& GetBlockDumps() const { return block_dumps_; }
    CpuState& GetCpuState() { return cpu_state_; }
    bool IsPaused() const {
        return paused_.load(std::memory_order_acquire) ||
               host_pause_active_.load(std::memory_order_acquire);
    }
#else
    bool IsPaused() const { return host_pause_active_.load(std::memory_order_acquire); }
#endif

    std::atomic<bool> system_running_{true};

 private:
    ArenaAllocator jit_arena_;
    IrBuilder jit_builder_;
    CodeEmitter jit_emitter_;

    CpuState cpu_state_{};
    BlockMap* block_map_;
    SeedQueue* seed_queue_;
    PrewarmPacingState* prewarm_pacing_;
    BlockCompileCache* block_cache_;
    std::vector<BlockDump> block_dumps_;
    std::unordered_map<uint64_t, size_t> block_dump_index_;

    struct BlockUsageStats {
        uint64_t execution_count = 0;
        uint64_t total_guest_cycles = 0;
        uint64_t first_seen_guest_cycle = 0;
        uint64_t last_seen_guest_cycle = 0;
    };
    std::unordered_map<uint64_t, BlockUsageStats> block_usage_stats_;
    bool collect_block_usage_stats_{false};
    bool capture_block_dumps_{false};
    bool capture_crash_arm_bytes_{false};
    // Runtime perf controls (env-driven, conservative defaults in dispatcher.cc).
    uint32_t halt_step_cycles_{64};
    uint32_t halt_step_max_cycles_{1024};
    uint32_t halt_idle_streak_{0};
    bool timing_batch_enable_{true};
    uint32_t timing_batch_max_cycles_{32};
    static constexpr uint64_t kInvalidBlockKey = ~0ull;
    std::array<uint64_t, 2> recent_ready_block_keys_{{kInvalidBlockKey, kInvalidBlockKey}};
    std::array<void*, 2> recent_ready_block_hosts_{{nullptr, nullptr}};
    uint8_t recent_ready_block_insert_idx_{0};

    static constexpr uint32_t kNoPreviousBlock = 0xFFFFFFFFu;
    uint32_t last_executed_pc_{kNoPreviousBlock};
    bool last_executed_is_thumb_{false};
    /** Block that branched to last_executed_pc_ (the real "calling" block for overflow dump / debug). */
    uint32_t last_caller_pc_{kNoPreviousBlock};
    bool last_caller_is_thumb_{false};

    /** On x86 block size overflow: dump calling (and overflowed) block IR to analysis folder, log, abort. */
    void DumpCallingBlockAndAbort(uint32_t overflowed_pc, bool overflowed_thumb,
                                  size_t x86_size_or_zero);

    /** Write ARM/x86/IR to temp files and run disasm script (for DISASM_LAST_BLOCK and IO jump crash). */
    void RunDisasmScriptForBlock(const std::vector<uint8_t>& arm_bytes,
                                 const std::vector<uint8_t>& x86_bytes,
                                 const std::string& ir_text,
                                 uint32_t pc,
                                 bool is_thumb) const;

    /** Write a single block to path (same layout as DumpAnalysisFiles). */
    void DumpSingleBlockToPath(const BlockDump& d, const std::string& path) const;
    static uint64_t MakeBlockKey(uint32_t pc, bool is_thumb);
    void RememberReadyBlock(uint64_t block_key, void* host_code);
    void* LookupReadyBlockCache(uint64_t block_key) const;
    void UpsertBlockDump(BlockDump&& dump);
    void ApplyUsageStatsToDump(BlockDump* dump) const;
    void HandleHostPause();

#ifdef B_DEBUG
    void HandlePauseStep();

    WatchpointSet*        watchpoints_    = nullptr;
    TraceState*           trace_          = nullptr;
    std::atomic<bool>     pause_requested_{false};
    std::atomic<bool>     paused_         {false};
    std::atomic<int32_t>  step_count_     {0};
#endif
    std::atomic<bool>     host_pause_requested_{false};
    std::atomic<bool>     host_pause_active_{false};

    // ── Timing ────────────────────────────────────────────────────────────
    int32_t timing_scanline_{0};         // current scanline 0..227
    int32_t timing_scanline_cycle_{0};   // cycles elapsed within current scanline
    int32_t timing_last_counter_{0};     // previous cycle counter for timer stepping
    TimingCallbacks timing_callbacks_;

    void AdvanceTiming(int32_t new_counter);
    uint32_t ComputeTimingFlushThresholdCycles() const;

    /** Check IE/IF/IME and dispatch IRQ; materialize CPSR before saving to SPSR. */
    void CheckAndDispatchIRQ();
    uint32_t ComputeAdaptiveHaltStep(MemoryBus* bus, uint16_t ie);
    // ──────────────────────────────────────────────────────────────────────
};
