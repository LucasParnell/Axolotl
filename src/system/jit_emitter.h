#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "data/arena_alloc.h"
#include "data/block_cache_data.h"

struct CpuState;
class MemoryBus;

// IR -> x86-64. Implementation uses Xbyak (in .cc only, so -fno-exceptions is OK elsewhere).
class CodeEmitter {
 public:
    struct RelocEntry {
        uint32_t offset = 0;
        BlockCacheData::X86RelocType type = BlockCacheData::X86RelocType::kAbs64;
        BlockCacheData::X86SymbolId symbol = BlockCacheData::X86SymbolId::kMaterializeCpsr;
    };

    struct EmittedBlockArtifact {
        void* host_code = nullptr;
        size_t x86_size = 0;
        std::vector<uint8_t> x86_bytes;
        std::vector<RelocEntry> relocs;
        uint32_t pc = 0;
        bool is_thumb = false;
        uint32_t block_cycles = 0;
        uint32_t block_len = 0;
        uint32_t x86_crc32 = 0;
    };

    explicit CodeEmitter(size_t buffer_size = 32 * 1024 * 1024);
    ~CodeEmitter();

    void* EmitBlock(ArenaAllocator* arena, uint32_t pc, uint32_t block_cycles);
    EmittedBlockArtifact EmitBlockWithRelocs(ArenaAllocator* arena, uint32_t pc, uint32_t block_cycles, bool is_thumb, uint32_t block_len);

    /** Size in bytes of the block last emitted by EmitBlock (for debug dumps). */
    size_t GetLastEmittedBlockSize() const;
    /** Number of bytes currently used in the executable code buffer. */
    size_t GetBytesUsed() const;
    /** Total executable code buffer capacity in bytes. */
    size_t GetCapacityBytes() const;

    /** Set CpuState memory helpers and page table for JIT slow path. Call before running JIT with this state. */
    static void SetupCpuStateForJit(CpuState* state, MemoryBus* bus);

    /** Set callback for IO jump crash: given (block_pc, target_pc), dump that block's ARM/x86/IR (e.g. run disasm script). Call from JIT thread before Run(). */
    static void SetJitCrashDumpCallback(std::function<void(uint32_t block_pc, uint32_t target_pc)> fn);
    static uintptr_t ResolveCacheSymbol(BlockCacheData::X86SymbolId id);
    static std::string HostFeatureMaskString();

    CodeEmitter(const CodeEmitter&) = delete;
    CodeEmitter& operator=(const CodeEmitter&) = delete;

 private:
    struct Impl;
    Impl* impl_;
};
