#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "data/block_cache_data.h"
#include "data/block_heuristics_data.h"
#include "system/seed_queue.h"

// Lock-free ingest + single-writer segmented cache for compiled block seeds.
// Persists (pc,thumb) block identities; payload is delta-varint compressed.
class BlockCompileCache {
 public:
    using OptimizationDecision = BlockHeuristicsData::OptimizationDecision;

    struct LoadedX86Block {
        uint32_t pc = 0;
        bool is_thumb = false;
        uint32_t block_cycles = 0;
        uint32_t block_len = 0;
        uint32_t x86_crc32 = 0;
        std::vector<uint8_t> x86_bytes;
        std::vector<BlockCacheData::X86RelocEntry> relocs;
    };
    struct X86LoadLimits {
        size_t max_blocks = static_cast<size_t>(-1);
        size_t max_bytes = static_cast<size_t>(-1);
    };

    struct Config {
        std::string root_dir;
        std::string cache_key;
        uint32_t segment_target_records = BlockCacheData::kDefaultSegmentRecords;
        bool x86_cache_enable = true;
        bool x86_load_enable = true;
        bool x86_save_enable = true;
        int x86_log_level = 1;
        std::string host_feature_mask;
    };

    explicit BlockCompileCache(Config cfg);
    ~BlockCompileCache();

    BlockCompileCache(const BlockCompileCache&) = delete;
    BlockCompileCache& operator=(const BlockCompileCache&) = delete;

    bool Initialize();
    void StartWriterThread();
    void StopWriterThread();

    // Lock-free producer API; drops on ring saturation.
    void RecordCompiled(uint32_t pc, bool is_thumb);
    void RecordCompiledX86(uint32_t pc,
                           bool is_thumb,
                           const uint8_t* x86_bytes,
                           size_t x86_size,
                           const BlockCacheData::X86RelocEntry* relocs,
                           size_t reloc_count,
                           uint32_t cycles,
                           uint32_t block_len,
                           uint32_t x86_crc32);

    // Load all cached seed records and append to out.
    bool LoadSeedRecords(std::vector<CompileTarget>* out) const;
    bool LoadX86Blocks(std::vector<LoadedX86Block>* out, const X86LoadLimits& limits) const;
    void RecordExecutionSample(uint32_t pc, bool is_thumb, uint32_t consumed_guest_cycles);
    OptimizationDecision DecideOptimization(uint32_t pc,
                                            bool is_thumb,
                                            uint32_t block_cycles,
                                            uint32_t block_len) const;

    const std::string& CacheDir() const { return cache_dir_; }
    const std::string& CacheKey() const { return cfg_.cache_key; }
    static std::string BuildDefaultCacheKey(const std::string& rom_path, const std::string& bios_path);

 private:
    struct QueueEntry {
        uint32_t pc = 0;
        uint8_t is_thumb = 0;
        uint8_t reserved0 = 0;
        uint8_t reserved1 = 0;
        uint8_t reserved2 = 0;
    };
    struct X86QueueEntry {
        uint32_t pc = 0;
        uint8_t is_thumb = 0;
        uint8_t reserved0 = 0;
        uint16_t reserved1 = 0;
        uint32_t block_cycles = 0;
        uint32_t block_len = 0;
        uint32_t x86_crc32 = 0;
        uint32_t heuristic_score = 0;
        std::vector<uint8_t> x86_bytes;
        std::vector<BlockCacheData::X86RelocEntry> relocs;
    };

    static constexpr size_t kQueueCapacity = 1u << 15;  // 32768
    static constexpr size_t kX86QueueCapacity = 1u << 14;  // 16384
    static constexpr uint64_t kEmptySlot = 0ull;
    static constexpr uintptr_t kEmptyPtrSlot = 0u;

    static uint64_t PackEntry(const QueueEntry& e);
    static QueueEntry UnpackEntry(uint64_t packed);
    static uint32_t Crc32(const uint8_t* data, size_t size);
    static uint64_t Fnv1a64File(const std::string& path);
    static std::string Hex64(uint64_t v);
    bool OpenFilesForAppend();
    bool OpenX86FilesForAppend();
    void WriterLoop();
    bool PopQueueEntry(QueueEntry* out);
    bool PopX86QueueEntry(X86QueueEntry* out);
    void FlushSegment(std::vector<BlockCacheData::BlockSeedRecord>* segment_records);
    void FlushX86Segment(std::vector<X86QueueEntry>* segment_records);
    void WriteManifest();
    void WriteX86Manifest();
    bool ReadManifest(BlockCacheData::CacheManifest* out) const;
    bool ReadX86Manifest(BlockCacheData::X86CacheManifest* out) const;

    Config cfg_;
    std::string cache_dir_;
    std::string manifest_path_;
    std::string index_path_;
    std::string segments_path_;
    std::string x86_manifest_path_;
    std::string x86_index_path_;
    std::string x86_segments_path_;

    mutable std::atomic<bool> initialized_{false};
    std::atomic<bool> writer_running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread* writer_thread_{nullptr};

    std::array<std::atomic<uint64_t>, kQueueCapacity> queue_slots_{};
    std::atomic<uint64_t> producer_seq_{0};
    uint64_t consumer_seq_{0};
    std::atomic<uint64_t> dropped_events_{0};
    std::array<std::atomic<uintptr_t>, kX86QueueCapacity> x86_queue_slots_{};
    std::atomic<uint64_t> x86_producer_seq_{0};
    uint64_t x86_consumer_seq_{0};
    std::atomic<uint64_t> x86_dropped_events_{0};

    // Writer-owned state.
    std::FILE* manifest_fp_{nullptr};
    std::FILE* index_fp_{nullptr};
    std::FILE* segments_fp_{nullptr};
    uint32_t next_segment_id_{0};
    uint64_t total_records_{0};
    uint32_t total_segments_{0};
    std::atomic<uint64_t> recorded_events_{0};

    std::FILE* x86_manifest_fp_{nullptr};
    std::FILE* x86_index_fp_{nullptr};
    std::FILE* x86_segments_fp_{nullptr};
    uint32_t x86_next_segment_id_{0};
    uint64_t x86_total_blocks_{0};
    uint32_t x86_total_segments_{0};
    std::atomic<uint64_t> x86_recorded_events_{0};

    // Shared cross-thread heuristics used to tune prewarm compute and cache storage.
    mutable std::mutex heuristics_mu_;
    mutable BlockHeuristicsData::SoaStore heuristics_;
};

// Utility used by main() to build a stable cache key.
std::string BuildBlockCompileCacheKey(const std::string& rom_path, const std::string& bios_path);
