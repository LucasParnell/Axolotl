#pragma once

#include <array>
#include <cstdint>

// On-disk block cache format (v1): manifest + index + segmented payload file.
namespace BlockCacheData {

static constexpr uint32_t kFormatVersion = 1u;
static constexpr uint32_t kCodecDeltaVarint = 1u;
static constexpr uint32_t kDefaultSegmentRecords = 8192u;
static constexpr uint32_t kX86FormatVersion = 1u;
static constexpr std::array<char, 8> kMagic = {'A', 'X', 'B', 'C', 'A', 'C', 'H', 'E'};
static constexpr std::array<char, 8> kX86Magic = {'A', 'X', 'X', '8', 'C', 'A', 'C', 'H'};

struct CacheManifest {
    std::array<char, 8> magic{};
    uint32_t format_version = 0;
    uint32_t codec = 0;
    uint32_t segment_target_records = 0;
    uint32_t total_segments = 0;
    uint64_t total_records = 0;
    uint64_t created_unix_sec = 0;
    std::array<char, 64> cache_key{};
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};
static_assert(sizeof(CacheManifest) == 112, "CacheManifest size must be stable");

struct SegmentIndexEntry {
    uint32_t segment_id = 0;
    uint32_t record_count = 0;
    uint64_t payload_file_offset = 0;
    uint32_t payload_size = 0;
    uint32_t uncompressed_size = 0;
    uint64_t first_global_record = 0;
    uint32_t crc32 = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(SegmentIndexEntry) == 40, "SegmentIndexEntry size must be stable");

struct SegmentPayloadHeader {
    uint32_t segment_id = 0;
    uint32_t record_count = 0;
    uint32_t codec = 0;
    uint32_t payload_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t crc32 = 0;
};
static_assert(sizeof(SegmentPayloadHeader) == 24, "SegmentPayloadHeader size must be stable");

struct BlockSeedRecord {
    uint32_t pc = 0;
    uint8_t is_thumb = 0;
    uint8_t reserved0 = 0;
    uint8_t reserved1 = 0;
    uint8_t reserved2 = 0;
};
static_assert(sizeof(BlockSeedRecord) == 8, "BlockSeedRecord size must be stable");

enum class X86SymbolId : uint32_t {
    kJitRead32 = 1,
    kJitWrite32,
    kJitRead16,
    kJitWrite16,
    kJitRead8,
    kJitWrite8,
    kMaterializeCpsr,
    kSwapBankedRegisters,
    kJitCrashJumpToIo,
    kJitGetUserReg,
    kJitSetUserReg,
    kDebugLogSwi,
};

enum class X86RelocType : uint32_t {
    kAbs64 = 1,
};

struct X86RelocEntry {
    uint32_t offset = 0;      // Byte offset in block x86 payload
    uint32_t type = 0;        // X86RelocType
    uint32_t symbol_id = 0;   // X86SymbolId
    uint32_t reserved = 0;
};
static_assert(sizeof(X86RelocEntry) == 16, "X86RelocEntry size must be stable");

struct X86BlockHeader {
    uint32_t pc = 0;
    uint8_t is_thumb = 0;
    uint8_t reserved0 = 0;
    uint16_t reserved1 = 0;
    uint32_t x86_size = 0;
    uint32_t reloc_count = 0;
    uint32_t block_cycles = 0;
    uint32_t block_len = 0;
    uint32_t x86_crc32 = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(X86BlockHeader) == 32, "X86BlockHeader size must be stable");

struct X86SegmentHeader {
    uint32_t segment_id = 0;
    uint32_t block_count = 0;
    uint32_t codec = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc32 = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(X86SegmentHeader) == 24, "X86SegmentHeader size must be stable");

struct X86SegmentIndexEntry {
    uint32_t segment_id = 0;
    uint32_t block_count = 0;
    uint64_t payload_file_offset = 0;
    uint32_t payload_size = 0;
    uint32_t payload_crc32 = 0;
    uint64_t first_global_block = 0;
};
static_assert(sizeof(X86SegmentIndexEntry) == 32, "X86SegmentIndexEntry size must be stable");

struct X86CacheManifest {
    std::array<char, 8> magic{};
    uint32_t format_version = 0;
    uint32_t codec = 0;
    uint32_t total_segments = 0;
    uint64_t total_blocks = 0;
    uint64_t created_unix_sec = 0;
    std::array<char, 64> compat_key{};
    std::array<char, 32> cpu_feature_mask{};
};
static_assert(sizeof(X86CacheManifest) == 136, "X86CacheManifest size must be stable");

inline uint32_t RoundUpToExecPage(uint32_t bytes) {
    if (bytes == 0) return 0;
    return (bytes + 0x3FFFu) & ~0x3FFFu;
}

inline bool IsMappedRomExecPc(uint32_t pc, uint32_t rom_loaded_size) {
    const uint8_t region = static_cast<uint8_t>(pc >> 24);
    if (region < 0x08 || region > 0x0D) return false;
    const uint32_t mapped_rom_bytes = RoundUpToExecPage(rom_loaded_size);
    if (mapped_rom_bytes == 0) return false;
    const uint32_t window_offset = pc & 0x01FFFFFFu;
    return window_offset < mapped_rom_bytes;
}

inline bool IsCacheableExecPcWithRomSize(uint32_t pc, uint32_t rom_loaded_size) {
    const uint8_t region = static_cast<uint8_t>(pc >> 24);
    // BIOS executable window is only 0x00000000..0x00003FFF.
    if (region == 0x00) return pc < 0x00004000u;
    // Work RAM is executable.
    if (region == 0x02 || region == 0x03) return true;
    return IsMappedRomExecPc(pc, rom_loaded_size);
}

inline bool IsCacheableExecPc(uint32_t pc) {
    return IsCacheableExecPcWithRomSize(pc, 0x02000000u);
}

inline bool IsPersistentCachePcWithRomSize(uint32_t pc, uint32_t rom_loaded_size) {
    const uint8_t region = static_cast<uint8_t>(pc >> 24);
    if (region == 0x00) return pc < 0x00004000u;  // BIOS
    return IsMappedRomExecPc(pc, rom_loaded_size);
}

// Cross-run persistent cache should only include stable executable regions.
// Work RAM code can be valid at runtime, but its contents are not stable
// across process startup and should not be persisted/preloaded from disk.
inline bool IsPersistentCachePc(uint32_t pc) {
    return IsPersistentCachePcWithRomSize(pc, 0x02000000u);
}

}  // namespace BlockCacheData
