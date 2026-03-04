#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include "../util/debug_alloc_tag.h"

inline void* const kClaimedPrewarmer = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
inline void* const kClaimedDispatcher = reinterpret_cast<void*>(static_cast<uintptr_t>(2));

// A Page covers 16KB of GBA memory.
// Because we use 1-byte resolution (bit 0 is our ISA flag), we need 16384 entries.
struct BlockPage {
    std::array<std::atomic<void*>, 16384> entries{};
};

class BlockMap {
public:
    enum class ClaimOwner : uint8_t {
        kPrewarmer = 0,
        kDispatcher,
    };

    static void* ClaimedSentinel() { return kClaimedPrewarmer; }  // Compatibility shim.
    static void* ClaimedSentinelFor(ClaimOwner owner) {
        return owner == ClaimOwner::kDispatcher ? kClaimedDispatcher : kClaimedPrewarmer;
    }
    static bool IsClaimed(void* ptr) {
        return ptr == kClaimedPrewarmer || ptr == kClaimedDispatcher;
    }
    static bool IsClaimedBy(void* ptr, ClaimOwner owner) {
        return ptr == ClaimedSentinelFor(owner);
    }
    static bool IsReady(void* ptr) {
        return ptr != nullptr && !IsClaimed(ptr);
    }

    BlockMap() {
        for (auto& page : l1_directory_) {
            page.store(nullptr, std::memory_order_relaxed);
        }
    }

    ~BlockMap() {
        for (auto& page : l1_directory_) {
            BlockPage* p = page.load(std::memory_order_relaxed);
            if (p) AXOLOTL_ALLOC_TAG_DELETE("BlockMap::BlockPage", p);
        }
    }

    void* Lookup(uint32_t pc, bool is_thumb) {
        uint32_t key = MakeKey(pc, is_thumb);

        uint32_t l1_idx = key >> 14;
        uint32_t l2_idx = key & 0x3FFF;

        BlockPage* page = l1_directory_[l1_idx].load(std::memory_order_acquire);
        if (!page) return nullptr;
        return page->entries[l2_idx].load(std::memory_order_acquire);
    }

    bool TryClaimIfEmpty(uint32_t pc, bool is_thumb, ClaimOwner owner) {
        void* expected = nullptr;
        return GetAtomicRef(pc, is_thumb).compare_exchange_strong(
            expected, ClaimedSentinelFor(owner), std::memory_order_acq_rel);
    }

    bool TryStealClaim(uint32_t pc, bool is_thumb, ClaimOwner from, ClaimOwner to) {
        void* expected = ClaimedSentinelFor(from);
        return GetAtomicRef(pc, is_thumb).compare_exchange_strong(
            expected, ClaimedSentinelFor(to), std::memory_order_acq_rel);
    }

    bool PublishIfClaimedBy(uint32_t pc, bool is_thumb, ClaimOwner owner, void* host_code) {
        void* expected = ClaimedSentinelFor(owner);
        return GetAtomicRef(pc, is_thumb).compare_exchange_strong(
            expected, host_code, std::memory_order_release);
    }

    bool AbandonClaimIfOwned(uint32_t pc, bool is_thumb, ClaimOwner owner) {
        void* expected = ClaimedSentinelFor(owner);
        return GetAtomicRef(pc, is_thumb).compare_exchange_strong(
            expected, nullptr, std::memory_order_release);
    }

    void ForcePublish(uint32_t pc, bool is_thumb, void* host_code) {
        GetAtomicRef(pc, is_thumb).store(host_code, std::memory_order_release);
    }

private:
    static uint32_t MakeKey(uint32_t pc, bool is_thumb) {
        // Canonicalize to architectural block start:
        // Thumb blocks are halfword aligned; ARM blocks are word aligned.
        const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
        return aligned_pc | (is_thumb ? 1u : 0u);
    }

    std::atomic<void*>& GetAtomicRef(uint32_t pc, bool is_thumb) {
        uint32_t key = MakeKey(pc, is_thumb);
        uint32_t l1_idx = key >> 14;
        uint32_t l2_idx = key & 0x3FFF;

        BlockPage* page = l1_directory_[l1_idx].load(std::memory_order_acquire);

        if (!page) {
            BlockPage* new_page = AXOLOTL_ALLOC_TAG_NEW("BlockMap::BlockPage", new BlockPage());
            BlockPage* expected = nullptr;
            if (l1_directory_[l1_idx].compare_exchange_strong(expected, new_page, std::memory_order_acq_rel)) {
                page = new_page;
            } else {
                AXOLOTL_ALLOC_TAG_DELETE("BlockMap::BlockPage(lost race)", new_page);
                page = expected;
            }
        }
        return page->entries[l2_idx];
    }

    // 262144 pages * 16KB = 4GB address space coverage (Exactly matches MemoryBus)
    std::array<std::atomic<BlockPage*>, 262144> l1_directory_{};
};
