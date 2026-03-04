#include "system/block_compile_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <queue>
#include <sstream>
#include <unordered_map>

#include "util/debug_profiler.h"
#include "util/logger.h"
#include "util/varint_codec.h"

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr uint64_t HashStringFnv1a64(const std::string& s) {
    uint64_t h = kFnvOffset;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t MixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace

BlockCompileCache::BlockCompileCache(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.segment_target_records == 0) {
        cfg_.segment_target_records = BlockCacheData::kDefaultSegmentRecords;
    }
    if (cfg_.cache_key.empty()) cfg_.cache_key = "default";
    cache_dir_ = cfg_.root_dir + "/v1/" + cfg_.cache_key;
    manifest_path_ = cache_dir_ + "/manifest.bin";
    index_path_ = cache_dir_ + "/index.bin";
    segments_path_ = cache_dir_ + "/segments.bin";
    x86_manifest_path_ = cache_dir_ + "/x86_manifest.bin";
    x86_index_path_ = cache_dir_ + "/x86_index.bin";
    x86_segments_path_ = cache_dir_ + "/x86_segments.bin";
    for (auto& s : queue_slots_) s.store(kEmptySlot, std::memory_order_relaxed);
    for (auto& s : x86_queue_slots_) s.store(kEmptyPtrSlot, std::memory_order_relaxed);
    heuristics_.Reserve(16384);
}

BlockCompileCache::~BlockCompileCache() {
    StopWriterThread();
}

uint64_t BlockCompileCache::PackEntry(const QueueEntry& e) {
    uint64_t packed = 0;
    packed |= static_cast<uint64_t>(e.pc);
    packed |= static_cast<uint64_t>(e.is_thumb & 1u) << 32;
    return packed + 1u;  // zero reserved as empty
}

BlockCompileCache::QueueEntry BlockCompileCache::UnpackEntry(uint64_t packed) {
    packed -= 1u;
    QueueEntry e{};
    e.pc = static_cast<uint32_t>(packed & 0xFFFFFFFFu);
    e.is_thumb = static_cast<uint8_t>((packed >> 32) & 1u);
    return e;
}

uint32_t BlockCompileCache::Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = static_cast<uint32_t>(-(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint64_t BlockCompileCache::Fnv1a64File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return 0;
    uint64_t h = kFnvOffset;
    std::array<char, 16384> buf{};
    while (in.good()) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(buf[static_cast<size_t>(i)]));
            h *= kFnvPrime;
        }
    }
    return h;
}

std::string BlockCompileCache::Hex64(uint64_t v) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << v;
    return oss.str();
}

std::string BlockCompileCache::BuildDefaultCacheKey(const std::string& rom_path, const std::string& bios_path) {
    uint64_t h = HashStringFnv1a64("axolotl-block-cache-v1");
    h = MixHash(h, Fnv1a64File(rom_path));
    h = MixHash(h, Fnv1a64File(bios_path));
    // Bump ABI key to invalidate previously generated x86 preload entries.
    h = MixHash(h, HashStringFnv1a64("jit-abi-v2"));
    return Hex64(h);
}

std::string BuildBlockCompileCacheKey(const std::string& rom_path, const std::string& bios_path) {
    return BlockCompileCache::BuildDefaultCacheKey(rom_path, bios_path);
}

bool BlockCompileCache::OpenFilesForAppend() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
    if (ec) return false;

    manifest_fp_ = std::fopen(manifest_path_.c_str(), "r+b");
    if (!manifest_fp_) manifest_fp_ = std::fopen(manifest_path_.c_str(), "w+b");
    index_fp_ = std::fopen(index_path_.c_str(), "a+b");
    segments_fp_ = std::fopen(segments_path_.c_str(), "a+b");
    if (!manifest_fp_ || !index_fp_ || !segments_fp_) return false;

    std::fseek(index_fp_, 0, SEEK_END);
    const long idx_size = std::ftell(index_fp_);
    if (idx_size > 0) {
        total_segments_ = static_cast<uint32_t>(idx_size / static_cast<long>(sizeof(BlockCacheData::SegmentIndexEntry)));
        next_segment_id_ = total_segments_;
    }
    return true;
}

bool BlockCompileCache::OpenX86FilesForAppend() {
    if (!cfg_.x86_cache_enable) return true;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
    if (ec) return false;

    x86_manifest_fp_ = std::fopen(x86_manifest_path_.c_str(), "r+b");
    if (!x86_manifest_fp_) x86_manifest_fp_ = std::fopen(x86_manifest_path_.c_str(), "w+b");
    x86_index_fp_ = std::fopen(x86_index_path_.c_str(), "a+b");
    x86_segments_fp_ = std::fopen(x86_segments_path_.c_str(), "a+b");
    if (!x86_manifest_fp_ || !x86_index_fp_ || !x86_segments_fp_) return false;

    std::fseek(x86_index_fp_, 0, SEEK_END);
    const long idx_size = std::ftell(x86_index_fp_);
    if (idx_size > 0) {
        x86_total_segments_ =
            static_cast<uint32_t>(idx_size / static_cast<long>(sizeof(BlockCacheData::X86SegmentIndexEntry)));
        x86_next_segment_id_ = x86_total_segments_;
    }
    return true;
}

bool BlockCompileCache::ReadManifest(BlockCacheData::CacheManifest* out) const {
    std::FILE* fp = std::fopen(manifest_path_.c_str(), "rb");
    if (!fp) return false;
    BlockCacheData::CacheManifest mf{};
    const size_t n = std::fread(&mf, 1, sizeof(mf), fp);
    std::fclose(fp);
    if (n != sizeof(mf)) return false;
    if (mf.magic != BlockCacheData::kMagic) return false;
    if (mf.format_version != BlockCacheData::kFormatVersion) return false;
    *out = mf;
    return true;
}

bool BlockCompileCache::ReadX86Manifest(BlockCacheData::X86CacheManifest* out) const {
    if (!cfg_.x86_cache_enable) return false;
    std::FILE* fp = std::fopen(x86_manifest_path_.c_str(), "rb");
    if (!fp) return false;
    BlockCacheData::X86CacheManifest mf{};
    const size_t n = std::fread(&mf, 1, sizeof(mf), fp);
    std::fclose(fp);
    if (n != sizeof(mf)) return false;
    if (mf.magic != BlockCacheData::kX86Magic) return false;
    if (mf.format_version != BlockCacheData::kX86FormatVersion) return false;
    *out = mf;
    return true;
}

void BlockCompileCache::WriteManifest() {
    if (!manifest_fp_) return;
    BlockCacheData::CacheManifest mf{};
    mf.magic = BlockCacheData::kMagic;
    mf.format_version = BlockCacheData::kFormatVersion;
    mf.codec = BlockCacheData::kCodecDeltaVarint;
    mf.segment_target_records = cfg_.segment_target_records;
    mf.total_segments = total_segments_;
    mf.total_records = total_records_;
    mf.created_unix_sec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    std::memset(mf.cache_key.data(), 0, mf.cache_key.size());
    const size_t copy_len = std::min(mf.cache_key.size() - 1u, cfg_.cache_key.size());
    std::memcpy(mf.cache_key.data(), cfg_.cache_key.data(), copy_len);

    std::rewind(manifest_fp_);
    std::fwrite(&mf, 1, sizeof(mf), manifest_fp_);
    std::fflush(manifest_fp_);
}

void BlockCompileCache::WriteX86Manifest() {
    if (!cfg_.x86_cache_enable || !x86_manifest_fp_) return;
    BlockCacheData::X86CacheManifest mf{};
    mf.magic = BlockCacheData::kX86Magic;
    mf.format_version = BlockCacheData::kX86FormatVersion;
    mf.codec = BlockCacheData::kCodecDeltaVarint;
    mf.total_segments = x86_total_segments_;
    mf.total_blocks = x86_total_blocks_;
    mf.created_unix_sec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::memset(mf.compat_key.data(), 0, mf.compat_key.size());
    const size_t key_copy = std::min(mf.compat_key.size() - 1u, cfg_.cache_key.size());
    std::memcpy(mf.compat_key.data(), cfg_.cache_key.data(), key_copy);
    std::memset(mf.cpu_feature_mask.data(), 0, mf.cpu_feature_mask.size());
    const size_t feat_copy = std::min(mf.cpu_feature_mask.size() - 1u, cfg_.host_feature_mask.size());
    std::memcpy(mf.cpu_feature_mask.data(), cfg_.host_feature_mask.data(), feat_copy);

    std::rewind(x86_manifest_fp_);
    std::fwrite(&mf, 1, sizeof(mf), x86_manifest_fp_);
    std::fflush(x86_manifest_fp_);
}

bool BlockCompileCache::Initialize() {
    AXOLOTL_PROFILE_SCOPE("block_cache.initialize");
    if (!OpenFilesForAppend()) {
        Logger::log("[BlockCache] failed to open cache files in " + cache_dir_, LogLevel::WARNING);
        return false;
    }
    if (!OpenX86FilesForAppend()) {
        Logger::log("[BlockCache] failed to open x86 cache files in " + cache_dir_, LogLevel::WARNING);
        return false;
    }

    BlockCacheData::CacheManifest old{};
    if (ReadManifest(&old)) {
        total_records_ = old.total_records;
        total_segments_ = old.total_segments;
        next_segment_id_ = old.total_segments;
    } else {
        WriteManifest();
    }
    if (cfg_.x86_cache_enable) {
        BlockCacheData::X86CacheManifest x86_old{};
        if (ReadX86Manifest(&x86_old)) {
            x86_total_blocks_ = x86_old.total_blocks;
            x86_total_segments_ = x86_old.total_segments;
            x86_next_segment_id_ = x86_old.total_segments;
        } else {
            WriteX86Manifest();
        }
    }

    initialized_.store(true, std::memory_order_release);
    Logger::log("[BlockCache] initialized key=" + cfg_.cache_key + " dir=" + cache_dir_, LogLevel::INFO);
    return true;
}

void BlockCompileCache::StartWriterThread() {
    if (!initialized_.load(std::memory_order_acquire)) return;
    if (writer_running_.exchange(true, std::memory_order_acq_rel)) return;
    stop_requested_.store(false, std::memory_order_release);
    writer_thread_ = new std::thread([this]() { WriterLoop(); });
}

void BlockCompileCache::StopWriterThread() {
    AXOLOTL_PROFILE_SCOPE("block_cache.stop_writer_thread");
    if (!writer_running_.exchange(false, std::memory_order_acq_rel)) return;
    stop_requested_.store(true, std::memory_order_release);
    if (writer_thread_) {
        writer_thread_->join();
        delete writer_thread_;
        writer_thread_ = nullptr;
    }
    if (manifest_fp_) { std::fclose(manifest_fp_); manifest_fp_ = nullptr; }
    if (index_fp_) { std::fclose(index_fp_); index_fp_ = nullptr; }
    if (segments_fp_) { std::fclose(segments_fp_); segments_fp_ = nullptr; }
    if (x86_manifest_fp_) { std::fclose(x86_manifest_fp_); x86_manifest_fp_ = nullptr; }
    if (x86_index_fp_) { std::fclose(x86_index_fp_); x86_index_fp_ = nullptr; }
    if (x86_segments_fp_) { std::fclose(x86_segments_fp_); x86_segments_fp_ = nullptr; }
    AXOLOTL_PROFILE_DUMP("block_cache.writer_stopped");
}

void BlockCompileCache::RecordCompiled(uint32_t pc, bool is_thumb) {
    if (!writer_running_.load(std::memory_order_relaxed)) return;
    QueueEntry e{};
    e.pc = pc & (is_thumb ? ~1u : ~3u);
    if (!BlockCacheData::IsPersistentCachePc(e.pc)) return;
    e.is_thumb = is_thumb ? 1u : 0u;
    const uint64_t packed = PackEntry(e);
    const uint64_t seq = producer_seq_.fetch_add(1, std::memory_order_relaxed);
    const size_t idx = static_cast<size_t>(seq & (kQueueCapacity - 1u));
    uint64_t expected = kEmptySlot;
    if (!queue_slots_[idx].compare_exchange_strong(expected, packed, std::memory_order_release, std::memory_order_relaxed)) {
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    recorded_events_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(heuristics_mu_);
        BlockHeuristicsData::RecordCompiled(&heuristics_, BlockHeuristicsData::MakeBlockKey(e.pc, is_thumb));
    }
}

void BlockCompileCache::RecordCompiledX86(uint32_t pc,
                                          bool is_thumb,
                                          const uint8_t* x86_bytes,
                                          size_t x86_size,
                                          const BlockCacheData::X86RelocEntry* relocs,
                                          size_t reloc_count,
                                          uint32_t cycles,
                                          uint32_t block_len,
                                          uint32_t x86_crc32) {
    if (!cfg_.x86_cache_enable || !cfg_.x86_save_enable) return;
    if (!writer_running_.load(std::memory_order_relaxed)) return;
    if (!x86_bytes || x86_size == 0) return;
    if (x86_size > (1u << 20) || reloc_count > (1u << 14)) return;  // hard caps
    const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
    if (!BlockCacheData::IsPersistentCachePc(aligned_pc)) return;

    uint32_t heuristic_score = 0;
    {
        std::lock_guard<std::mutex> lock(heuristics_mu_);
        BlockHeuristicsData::RecordCompiledX86(&heuristics_,
                                               BlockHeuristicsData::MakeBlockKey(aligned_pc, is_thumb),
                                               cycles,
                                               block_len,
                                               static_cast<uint32_t>(x86_size));
        heuristic_score =
            BlockHeuristicsData::Decide(heuristics_,
                                        BlockHeuristicsData::MakeBlockKey(aligned_pc, is_thumb),
                                        cycles,
                                        block_len)
                .heuristic_score;
    }

    X86QueueEntry* e = new (std::nothrow) X86QueueEntry();
    if (!e) {
        x86_dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    e->pc = aligned_pc;
    e->is_thumb = is_thumb ? 1u : 0u;
    e->block_cycles = cycles;
    e->block_len = block_len;
    e->x86_crc32 = x86_crc32;
    e->heuristic_score = heuristic_score;
    e->x86_bytes.assign(x86_bytes, x86_bytes + x86_size);
    if (relocs && reloc_count != 0) e->relocs.assign(relocs, relocs + reloc_count);

    const uint64_t seq = x86_producer_seq_.fetch_add(1, std::memory_order_relaxed);
    const size_t idx = static_cast<size_t>(seq & (kX86QueueCapacity - 1u));
    uintptr_t expected = kEmptyPtrSlot;
    if (!x86_queue_slots_[idx].compare_exchange_strong(expected,
                                                        reinterpret_cast<uintptr_t>(e),
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed)) {
        delete e;
        x86_dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    x86_recorded_events_.fetch_add(1, std::memory_order_relaxed);
}

void BlockCompileCache::RecordExecutionSample(uint32_t pc, bool is_thumb, uint32_t consumed_guest_cycles) {
    const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
    std::lock_guard<std::mutex> lock(heuristics_mu_);
    BlockHeuristicsData::RecordExecutionSample(&heuristics_,
                                               BlockHeuristicsData::MakeBlockKey(aligned_pc, is_thumb),
                                               consumed_guest_cycles);
}

BlockCompileCache::OptimizationDecision BlockCompileCache::DecideOptimization(uint32_t pc,
                                                                              bool is_thumb,
                                                                              uint32_t block_cycles,
                                                                              uint32_t block_len) const {
    if (!initialized_.load(std::memory_order_acquire)) return OptimizationDecision{};

    const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
    std::lock_guard<std::mutex> lock(heuristics_mu_);
    return BlockHeuristicsData::Decide(heuristics_,
                                       BlockHeuristicsData::MakeBlockKey(aligned_pc, is_thumb),
                                       block_cycles,
                                       block_len);
}

bool BlockCompileCache::PopQueueEntry(QueueEntry* out) {
    const uint64_t produced = producer_seq_.load(std::memory_order_acquire);
    if (consumer_seq_ >= produced) return false;

    const size_t idx = static_cast<size_t>(consumer_seq_ & (kQueueCapacity - 1u));
    const uint64_t packed = queue_slots_[idx].load(std::memory_order_acquire);
    if (packed == kEmptySlot) {
        // Producer either dropped this slot due contention or is late publishing.
        // This cache is best-effort; advance to guarantee lock-free progress.
        ++consumer_seq_;
        return false;
    }
    *out = UnpackEntry(packed);
    queue_slots_[idx].store(kEmptySlot, std::memory_order_release);
    ++consumer_seq_;
    return true;
}

bool BlockCompileCache::PopX86QueueEntry(X86QueueEntry* out) {
    const uint64_t produced = x86_producer_seq_.load(std::memory_order_acquire);
    if (x86_consumer_seq_ >= produced) return false;

    const size_t idx = static_cast<size_t>(x86_consumer_seq_ & (kX86QueueCapacity - 1u));
    const uintptr_t ptr = x86_queue_slots_[idx].load(std::memory_order_acquire);
    if (ptr == kEmptyPtrSlot) {
        ++x86_consumer_seq_;
        return false;
    }
    X86QueueEntry* e = reinterpret_cast<X86QueueEntry*>(ptr);
    *out = std::move(*e);
    delete e;
    x86_queue_slots_[idx].store(kEmptyPtrSlot, std::memory_order_release);
    ++x86_consumer_seq_;
    return true;
}

void BlockCompileCache::FlushSegment(std::vector<BlockCacheData::BlockSeedRecord>* segment_records) {
    AXOLOTL_PROFILE_SCOPE("block_cache.flush_seed_segment");
    if (segment_records->empty() || !index_fp_ || !segments_fp_) return;

    std::sort(segment_records->begin(), segment_records->end(),
              [](const BlockCacheData::BlockSeedRecord& a, const BlockCacheData::BlockSeedRecord& b) {
                  const uint64_t ka = (static_cast<uint64_t>(a.pc) << 1) | (a.is_thumb ? 1ull : 0ull);
                  const uint64_t kb = (static_cast<uint64_t>(b.pc) << 1) | (b.is_thumb ? 1ull : 0ull);
                  return ka < kb;
              });
    segment_records->erase(std::unique(segment_records->begin(), segment_records->end(),
                                       [](const BlockCacheData::BlockSeedRecord& a, const BlockCacheData::BlockSeedRecord& b) {
                                           return a.pc == b.pc && a.is_thumb == b.is_thumb;
                                       }),
                           segment_records->end());

    std::vector<uint8_t> payload;
    payload.reserve(segment_records->size() * 3u);
    uint64_t prev_key = 0;
    for (const auto& r : *segment_records) {
        const uint64_t key = (static_cast<uint64_t>(r.pc) << 1) | (r.is_thumb ? 1ull : 0ull);
        const uint64_t delta = key - prev_key;
        VarintCodec::EncodeU64(delta, &payload);
        prev_key = key;
    }

    std::fseek(segments_fp_, 0, SEEK_END);
    const uint64_t file_off = static_cast<uint64_t>(std::ftell(segments_fp_));

    BlockCacheData::SegmentPayloadHeader sh{};
    sh.segment_id = next_segment_id_;
    sh.record_count = static_cast<uint32_t>(segment_records->size());
    sh.codec = BlockCacheData::kCodecDeltaVarint;
    sh.payload_size = static_cast<uint32_t>(payload.size());
    sh.uncompressed_size = static_cast<uint32_t>(segment_records->size() * sizeof(BlockCacheData::BlockSeedRecord));
    sh.crc32 = Crc32(payload.data(), payload.size());
    std::fwrite(&sh, 1, sizeof(sh), segments_fp_);
    if (!payload.empty()) std::fwrite(payload.data(), 1, payload.size(), segments_fp_);
    std::fflush(segments_fp_);

    BlockCacheData::SegmentIndexEntry ie{};
    ie.segment_id = next_segment_id_;
    ie.record_count = sh.record_count;
    ie.payload_file_offset = file_off;
    ie.payload_size = sh.payload_size;
    ie.uncompressed_size = sh.uncompressed_size;
    ie.first_global_record = total_records_;
    ie.crc32 = sh.crc32;
    std::fwrite(&ie, 1, sizeof(ie), index_fp_);
    std::fflush(index_fp_);

    total_records_ += ie.record_count;
    ++total_segments_;
    ++next_segment_id_;
    WriteManifest();
    segment_records->clear();
}

void BlockCompileCache::FlushX86Segment(std::vector<X86QueueEntry>* segment_records) {
    AXOLOTL_PROFILE_SCOPE("block_cache.flush_x86_segment");
    if (!cfg_.x86_cache_enable || !cfg_.x86_save_enable) return;
    if (segment_records->empty() || !x86_index_fp_ || !x86_segments_fp_) return;

    std::stable_sort(segment_records->begin(), segment_records->end(),
                     [](const X86QueueEntry& a, const X86QueueEntry& b) {
                         const uint64_t ka =
                             (static_cast<uint64_t>(a.pc) << 1) | static_cast<uint64_t>(a.is_thumb & 1u);
                         const uint64_t kb =
                             (static_cast<uint64_t>(b.pc) << 1) | static_cast<uint64_t>(b.is_thumb & 1u);
                         return ka < kb;
                     });
    segment_records->erase(
        std::unique(segment_records->begin(), segment_records->end(),
                    [](const X86QueueEntry& a, const X86QueueEntry& b) {
                        return a.pc == b.pc && a.is_thumb == b.is_thumb;
                    }),
        segment_records->end());

    std::vector<uint8_t> payload;
    uint64_t prev_key = 0;
    for (const X86QueueEntry& rec : *segment_records) {
        const uint64_t key = (static_cast<uint64_t>(rec.pc) << 1) | static_cast<uint64_t>(rec.is_thumb & 1u);
        const uint64_t delta = key - prev_key;
        VarintCodec::EncodeU64(delta, &payload);
        prev_key = key;

        BlockCacheData::X86BlockHeader hdr{};
        hdr.pc = rec.pc;
        hdr.is_thumb = rec.is_thumb;
        hdr.x86_size = static_cast<uint32_t>(rec.x86_bytes.size());
        hdr.reloc_count = static_cast<uint32_t>(rec.relocs.size());
        hdr.block_cycles = rec.block_cycles;
        hdr.block_len = rec.block_len;
        hdr.x86_crc32 = rec.x86_crc32;
        hdr.reserved2 = rec.heuristic_score;

        const uint8_t* hdr_ptr = reinterpret_cast<const uint8_t*>(&hdr);
        payload.insert(payload.end(), hdr_ptr, hdr_ptr + sizeof(hdr));

        if (!rec.relocs.empty()) {
            const uint8_t* rel_ptr = reinterpret_cast<const uint8_t*>(rec.relocs.data());
            payload.insert(payload.end(), rel_ptr, rel_ptr + rec.relocs.size() * sizeof(BlockCacheData::X86RelocEntry));
        }
        payload.insert(payload.end(), rec.x86_bytes.begin(), rec.x86_bytes.end());
    }

    std::fseek(x86_segments_fp_, 0, SEEK_END);
    const uint64_t file_off = static_cast<uint64_t>(std::ftell(x86_segments_fp_));

    BlockCacheData::X86SegmentHeader sh{};
    sh.segment_id = x86_next_segment_id_;
    sh.block_count = static_cast<uint32_t>(segment_records->size());
    sh.codec = BlockCacheData::kCodecDeltaVarint;
    sh.payload_size = static_cast<uint32_t>(payload.size());
    sh.payload_crc32 = Crc32(payload.data(), payload.size());
    std::fwrite(&sh, 1, sizeof(sh), x86_segments_fp_);
    if (!payload.empty()) std::fwrite(payload.data(), 1, payload.size(), x86_segments_fp_);
    std::fflush(x86_segments_fp_);

    BlockCacheData::X86SegmentIndexEntry ie{};
    ie.segment_id = x86_next_segment_id_;
    ie.block_count = sh.block_count;
    ie.payload_file_offset = file_off;
    ie.payload_size = sh.payload_size;
    ie.payload_crc32 = sh.payload_crc32;
    ie.first_global_block = x86_total_blocks_;
    std::fwrite(&ie, 1, sizeof(ie), x86_index_fp_);
    std::fflush(x86_index_fp_);

    x86_total_blocks_ += ie.block_count;
    ++x86_total_segments_;
    ++x86_next_segment_id_;
    WriteX86Manifest();
    segment_records->clear();
}

void BlockCompileCache::WriterLoop() {
    AXOLOTL_PROFILE_SCOPE("block_cache.writer_loop");
    std::vector<BlockCacheData::BlockSeedRecord> segment_records;
    segment_records.reserve(cfg_.segment_target_records);
    std::vector<X86QueueEntry> x86_segment_records;
    x86_segment_records.reserve(std::max<uint32_t>(256u, cfg_.segment_target_records / 4u));
    uint64_t idle_ticks = 0;
    uint32_t idle_spins = 0;
    while (!stop_requested_.load(std::memory_order_acquire) ||
           producer_seq_.load(std::memory_order_relaxed) != consumer_seq_ ||
           x86_producer_seq_.load(std::memory_order_relaxed) != x86_consumer_seq_) {
        QueueEntry e{};
        X86QueueEntry x86e{};
        bool had_seed = PopQueueEntry(&e);
        bool had_x86 = PopX86QueueEntry(&x86e);
        if (!had_seed && !had_x86) {
            ++idle_ticks;
            ++idle_spins;
            if (!segment_records.empty() && idle_ticks > 2048u) {
                FlushSegment(&segment_records);
                idle_ticks = 0;
            }
            if (!x86_segment_records.empty() && idle_ticks > 2048u) {
                FlushX86Segment(&x86_segment_records);
                idle_ticks = 0;
            }
            if (idle_spins < 64u) {
                std::this_thread::yield();
            } else if (idle_spins < 1024u) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            continue;
        }
        idle_ticks = 0;
        idle_spins = 0;
        if (had_seed) {
            BlockCacheData::BlockSeedRecord rec{};
            rec.pc = e.pc;
            rec.is_thumb = e.is_thumb;
            segment_records.push_back(rec);
            if (segment_records.size() >= cfg_.segment_target_records) {
                FlushSegment(&segment_records);
            }
        }
        if (had_x86) {
            x86_segment_records.push_back(std::move(x86e));
            if (x86_segment_records.size() >= std::max<uint32_t>(256u, cfg_.segment_target_records / 4u)) {
                FlushX86Segment(&x86_segment_records);
            }
        }
    }
    FlushSegment(&segment_records);
    FlushX86Segment(&x86_segment_records);

    const uint64_t dropped = dropped_events_.load(std::memory_order_relaxed);
    if (dropped != 0) {
        Logger::log("[BlockCache] dropped " + std::to_string(dropped) + " queue events", LogLevel::WARNING);
    }
    const uint64_t x86_dropped = x86_dropped_events_.load(std::memory_order_relaxed);
    if (x86_dropped != 0) {
        Logger::log("[BlockCache] dropped " + std::to_string(x86_dropped) + " x86 queue events", LogLevel::WARNING);
    }
}

bool BlockCompileCache::LoadSeedRecords(std::vector<CompileTarget>* out) const {
    AXOLOTL_PROFILE_SCOPE("block_cache.load_seed_records");
    if (!initialized_.load(std::memory_order_acquire)) return false;
    std::FILE* idx = std::fopen(index_path_.c_str(), "rb");
    std::FILE* seg = std::fopen(segments_path_.c_str(), "rb");
    if (!idx || !seg) {
        if (idx) std::fclose(idx);
        if (seg) std::fclose(seg);
        return false;
    }

    BlockCacheData::SegmentIndexEntry ie{};
    std::vector<uint8_t> payload;
    while (std::fread(&ie, 1, sizeof(ie), idx) == sizeof(ie)) {
        if (ie.payload_size == 0) continue;
        payload.resize(ie.payload_size);
        std::fseek(seg, static_cast<long>(ie.payload_file_offset), SEEK_SET);
        BlockCacheData::SegmentPayloadHeader sh{};
        if (std::fread(&sh, 1, sizeof(sh), seg) != sizeof(sh)) break;
        if (sh.payload_size != ie.payload_size || sh.codec != BlockCacheData::kCodecDeltaVarint) continue;
        if (std::fread(payload.data(), 1, payload.size(), seg) != payload.size()) break;
        if (Crc32(payload.data(), payload.size()) != sh.crc32) continue;

        size_t pos = 0;
        uint64_t key = 0;
        for (uint32_t i = 0; i < sh.record_count; ++i) {
            uint64_t delta = 0;
            if (!VarintCodec::DecodeU64(payload.data(), payload.size(), &pos, &delta)) break;
            key += delta;
            CompileTarget t{};
            t.pc = static_cast<uint32_t>(key >> 1);
            t.is_thumb = (key & 1u) != 0;
            if (!BlockCacheData::IsPersistentCachePc(t.pc)) continue;
            out->push_back(t);
        }
    }
    std::fclose(idx);
    std::fclose(seg);
    return true;
}

bool BlockCompileCache::LoadX86Blocks(std::vector<LoadedX86Block>* out, const X86LoadLimits& limits) const {
    AXOLOTL_PROFILE_SCOPE("block_cache.load_x86_blocks");
    if (!cfg_.x86_cache_enable || !cfg_.x86_load_enable) return false;
    if (!initialized_.load(std::memory_order_acquire)) return false;

    BlockCacheData::X86CacheManifest mf{};
    if (!ReadX86Manifest(&mf)) return false;

    const std::string manifest_key(mf.compat_key.data());
    const std::string manifest_feat(mf.cpu_feature_mask.data());
    if (manifest_key != cfg_.cache_key) {
        if (cfg_.x86_log_level > 0) {
            Logger::log("[BlockCache] x86 cache key mismatch; skipping x86 preload", LogLevel::INFO);
        }
        return false;
    }
    if (!cfg_.host_feature_mask.empty() && manifest_feat != cfg_.host_feature_mask) {
        if (cfg_.x86_log_level > 0) {
            Logger::log("[BlockCache] x86 host feature mismatch; skipping x86 preload", LogLevel::INFO);
        }
        return false;
    }

    std::FILE* idx = std::fopen(x86_index_path_.c_str(), "rb");
    std::FILE* seg = std::fopen(x86_segments_path_.c_str(), "rb");
    if (!idx || !seg) {
        if (idx) std::fclose(idx);
        if (seg) std::fclose(seg);
        return false;
    }

    struct PreloadCandidate {
        uint64_t key = 0;
        uint32_t score = 0;
        uint64_t serial = 0;
        size_t bytes_aligned = 0;
        LoadedX86Block block;
    };
    struct HeapEntry {
        uint32_t score = 0;
        uint64_t serial = 0;
        uint64_t key = 0;
    };
    struct HeapCompare {
        bool operator()(const HeapEntry& a, const HeapEntry& b) const {
            if (a.score != b.score) return a.score > b.score;  // min-heap
            return a.serial > b.serial;
        }
    };
    auto ComputePreloadScore = [](uint32_t cycles,
                                  uint32_t len,
                                  uint32_t x86_size,
                                  uint32_t reloc_count,
                                  uint32_t persisted_score) -> uint32_t {
        uint64_t score = 0;
        score += static_cast<uint64_t>(persisted_score) * 8u;
        score += static_cast<uint64_t>(cycles) * 6u;
        score += static_cast<uint64_t>(len) * 3u;
        score += static_cast<uint64_t>(x86_size) / 3u;
        score += static_cast<uint64_t>(reloc_count) * 4u;
        return score > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(score);
    };

    const size_t max_blocks = limits.max_blocks;
    std::unordered_map<uint64_t, PreloadCandidate> selected;
    selected.reserve(std::min<size_t>(max_blocks, 16384u));
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapCompare> min_heap;
    uint64_t serial_counter = 0;

    auto prune_heap = [&]() {
        while (!min_heap.empty()) {
            const HeapEntry top = min_heap.top();
            const auto it = selected.find(top.key);
            if (it == selected.end() || it->second.serial != top.serial || it->second.score != top.score) {
                min_heap.pop();
                continue;
            }
            break;
        }
    };

    auto upsert_candidate = [&](uint64_t key, LoadedX86Block&& loaded, uint32_t score) {
        if (max_blocks == 0) return;
        const size_t bytes_aligned = (loaded.x86_bytes.size() + 15u) & ~size_t(15u);

        auto it = selected.find(key);
        if (it != selected.end()) {
            it->second.score = score;
            it->second.serial = ++serial_counter;
            it->second.bytes_aligned = bytes_aligned;
            it->second.block = std::move(loaded);
            min_heap.push({it->second.score, it->second.serial, key});
            return;
        }

        if (selected.size() < max_blocks) {
            PreloadCandidate cand{};
            cand.key = key;
            cand.score = score;
            cand.serial = ++serial_counter;
            cand.bytes_aligned = bytes_aligned;
            cand.block = std::move(loaded);
            selected.emplace(key, std::move(cand));
            min_heap.push({score, serial_counter, key});
            return;
        }

        prune_heap();
        if (min_heap.empty()) return;
        const HeapEntry weakest = min_heap.top();
        if (score <= weakest.score) return;

        selected.erase(weakest.key);
        min_heap.pop();

        PreloadCandidate cand{};
        cand.key = key;
        cand.score = score;
        cand.serial = ++serial_counter;
        cand.bytes_aligned = bytes_aligned;
        cand.block = std::move(loaded);
        selected.emplace(key, std::move(cand));
        min_heap.push({score, serial_counter, key});
    };

    BlockCacheData::X86SegmentIndexEntry ie{};
    std::vector<uint8_t> payload;
    while (std::fread(&ie, 1, sizeof(ie), idx) == sizeof(ie)) {
        if (ie.payload_size == 0) continue;
        payload.resize(ie.payload_size);
        std::fseek(seg, static_cast<long>(ie.payload_file_offset), SEEK_SET);
        BlockCacheData::X86SegmentHeader sh{};
        if (std::fread(&sh, 1, sizeof(sh), seg) != sizeof(sh)) break;
        if (sh.payload_size != ie.payload_size || sh.codec != BlockCacheData::kCodecDeltaVarint) continue;
        if (std::fread(payload.data(), 1, payload.size(), seg) != payload.size()) break;
        if (Crc32(payload.data(), payload.size()) != sh.payload_crc32) continue;

        size_t pos = 0;
        uint64_t key = 0;
        for (uint32_t i = 0; i < sh.block_count; ++i) {
            uint64_t delta = 0;
            if (!VarintCodec::DecodeU64(payload.data(), payload.size(), &pos, &delta)) break;
            key += delta;
            if (pos + sizeof(BlockCacheData::X86BlockHeader) > payload.size()) break;

            BlockCacheData::X86BlockHeader bh{};
            std::memcpy(&bh, payload.data() + pos, sizeof(bh));
            pos += sizeof(bh);
            const size_t reloc_bytes = static_cast<size_t>(bh.reloc_count) * sizeof(BlockCacheData::X86RelocEntry);
            if (pos + reloc_bytes > payload.size()) break;
            std::vector<BlockCacheData::X86RelocEntry> relocs;
            if (reloc_bytes != 0) {
                relocs.resize(bh.reloc_count);
                std::memcpy(relocs.data(), payload.data() + pos, reloc_bytes);
            }
            pos += reloc_bytes;
            if (pos + bh.x86_size > payload.size()) break;
            std::vector<uint8_t> bytes;
            bytes.assign(payload.data() + pos, payload.data() + pos + bh.x86_size);
            pos += bh.x86_size;
            if (Crc32(bytes.data(), bytes.size()) != bh.x86_crc32) continue;
            const bool tiny_block = bh.block_cycles <= 3u && bh.block_len <= 8u;
            if (tiny_block && bh.x86_size <= 128u) continue;

            LoadedX86Block loaded{};
            loaded.pc = bh.pc;
            loaded.is_thumb = (bh.is_thumb & 1u) != 0;
            if (!BlockCacheData::IsPersistentCachePc(loaded.pc)) continue;
            loaded.block_cycles = bh.block_cycles;
            loaded.block_len = bh.block_len;
            loaded.x86_crc32 = bh.x86_crc32;
            loaded.x86_bytes = std::move(bytes);
            loaded.relocs = std::move(relocs);
            upsert_candidate(key, std::move(loaded),
                             ComputePreloadScore(
                                 bh.block_cycles, bh.block_len, bh.x86_size, bh.reloc_count, bh.reserved2));
        }
    }

    std::fclose(idx);
    std::fclose(seg);

    std::vector<PreloadCandidate*> ranked;
    ranked.reserve(selected.size());
    for (auto& kv : selected) ranked.push_back(&kv.second);
    std::sort(ranked.begin(), ranked.end(), [](const PreloadCandidate* a, const PreloadCandidate* b) {
        if (a->score != b->score) return a->score > b->score;
        return a->serial > b->serial;
    });

    size_t total_loaded_bytes = 0;
    for (PreloadCandidate* c : ranked) {
        if (out->size() >= limits.max_blocks) break;
        if (total_loaded_bytes + c->bytes_aligned > limits.max_bytes) continue;
        total_loaded_bytes += c->bytes_aligned;
        {
            std::lock_guard<std::mutex> lock(heuristics_mu_);
            BlockHeuristicsData::RecordCompiledX86(&heuristics_,
                                                   BlockHeuristicsData::MakeBlockKey(c->block.pc, c->block.is_thumb),
                                                   c->block.block_cycles,
                                                   c->block.block_len,
                                                   static_cast<uint32_t>(c->block.x86_bytes.size()));
        }
        out->push_back(std::move(c->block));
    }
    return !out->empty();
}
