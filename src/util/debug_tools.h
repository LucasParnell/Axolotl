//
// util/debug_tools.h — Debug watchpoints and execution tracing
//
// All declarations are compiled away in release builds (B_DEBUG not defined).
// Data is separated from operations: WatchpointSet and TraceState are plain
// structs; all logic lives in free functions.
//

#pragma once

#ifdef B_DEBUG

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

// ── Plain data structs ─────────────────────────────────────────────────────

// Configuration for all active watchpoints / pause triggers.
// Written from the console thread, read from the JIT thread.
struct WatchpointSet {
    // I/O offset watchpoints (0x04000000-relative, e.g. 0x028 for BG2X_L)
    std::unordered_set<uint32_t> io_offsets;

    // Block-address breakpoints: pause before executing a block at this PC
    std::unordered_set<uint32_t> block_addresses;

    // SWI numbers that trigger a pause when executed
    std::unordered_set<uint32_t> swi_numbers;

    // Pause flags — each independently enables a class of pause trigger
    bool pause_on_bg_write  = false;   // any BG2X/Y or BG3X/Y write
    bool pause_on_io_write  = false;   // any io_offsets write
    bool pause_on_warning   = false;   // any Logger::WARNING
    bool pause_on_dma       = false;   // any DMA transfer
    bool pause_on_irq       = false;   // any IRQ dispatch
    bool pause_on_swi       = false;   // any swi_numbers match

    // Logging flags — log events without necessarily pausing
    bool log_bg_writes      = false;
    bool log_dma            = true;
    bool log_swi            = true;
};

// A single block in the execution path or loop body.
struct TraceBlock {
    uint32_t address;
    bool     is_thumb;
};

// One entry in the collapsed backtrace (consecutive duplicates merged).
struct BacktraceEntry {
    TraceBlock block;
    size_t     count;   // ≥1; >1 means this block ran consecutively that many times
};

// A detected loop body: ordered list of blocks forming one iteration.
using LoopBody = std::vector<TraceBlock>;

// Snapshot returned by GetBacktraceState — just the flattened block list.
struct BacktraceState {
    std::vector<TraceBlock> path;  // collapsed entries, oldest first
};

// All mutable tracing state — owned by the JIT thread, snapshotted by console.
// Protected by mtx for cross-thread reads from the console.
struct TraceState {
    static constexpr size_t kBacktraceDepth  = 16;
    static constexpr size_t kRingSize        = 512;  // must be power-of-two
    // Preserve older history by capping how many consecutive identical blocks
    // we keep in the backtrace ring.
    static constexpr size_t kBacktraceLoopCap = 64;
    static constexpr size_t kLoopIterCap     = 15;
    static constexpr size_t kMaxRecentBlocks = 64;

    // ── Backtrace: simple fixed-size ring of the last kRingSize raw blocks ──
    // Written only from the JIT thread; read under mtx by the console.
    TraceBlock ring[kRingSize] = {};
    size_t     ring_head = 0;   // index of the *next* slot to write (mod kRingSize)
    size_t     ring_fill = 0;   // how many valid entries are in the ring (≤ kRingSize)
    TraceBlock ring_last = {};
    size_t     ring_last_run_len = 0;

    // ── Loop-detection (only active when loop_tracing_enabled) ─────────────
    bool                  loop_tracing_enabled = false;
    std::vector<uint64_t> recent_keys;       // (address << 1) | is_thumb
    std::vector<uint64_t> loop_body_keys;    // current candidate loop (key form)
    size_t                loop_next_index  = 0;
    size_t                loop_iter_count  = 0;
    std::vector<LoopBody> recorded_loops;    // loops captured after kLoopIterCap iters

    mutable std::mutex mtx;
};

// ── Watchpoint queries ─────────────────────────────────────────────────────

// Returns true if pause_on_io_write is set and offset is in io_offsets.
bool ShouldPauseOnIOOffset(const WatchpointSet& wp, uint32_t offset);

// Returns true if pause_on_bg_write is set, or if pause_on_io_write is set and
// base_offset or base_offset+2 appear in io_offsets (handles the 32-bit BG ref
// registers stored as two consecutive 16-bit halves).
bool ShouldPauseOnBGWrite(const WatchpointSet& wp, uint32_t base_offset);

// Returns true if pause_on_swi is set and swi_number is in swi_numbers.
bool ShouldPauseOnSWI(const WatchpointSet& wp, uint32_t swi_number);

// Returns true if block_start is in block_addresses.
bool ShouldPauseOnBlock(const WatchpointSet& wp, uint32_t block_start);

// ── Trace update functions ─────────────────────────────────────────────────

// Must be called by the JIT thread after every block execution.
// Updates the ring buffer and (if enabled) loop detection.
void RecordBlockExecution(TraceState& ts, uint32_t address, bool is_thumb);

// Returns the last kBacktraceDepth distinct block executions, oldest first.
// Consecutive runs of the same block are collapsed into one entry (count > 1).
// Thread-safe: acquires ts.mtx.
std::vector<BacktraceEntry> GetBacktrace(const TraceState& ts);

// Simplified state snapshot — path is the same collapsed list as GetBacktrace.
BacktraceState GetBacktraceState(const TraceState& ts);

// Returns a snapshot of all loops recorded by loop detection.
// Thread-safe: acquires ts.mtx.
std::vector<LoopBody> GetRecordedLoopsSnapshot(const TraceState& ts);

#endif  // B_DEBUG
