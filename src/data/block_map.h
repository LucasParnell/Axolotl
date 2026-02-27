#pragma once
#include <atomic>
#include <array>
#include <cstdint>

// Block claimed by a thread during compilation.
inline void* const kClaimed = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

// One page: 8192 block pointers (16KB instruction range).
struct BlockPage {
    std::array<std::atomic<void*>, 8192> entries{};
};

class BlockMap {
public:
    static constexpr uint32_t kExecStart = 0x02000000;
    static constexpr uint32_t kExecEnd   = 0x0A000000;
    static constexpr uint32_t kRomStart = 0x08000000;

    static void* ClaimedSentinel() { return kClaimed; }

    BlockMap() {
        for (auto& page : l1_directory_) {
            page.store(nullptr, std::memory_order_relaxed);
        }
    }

    ~BlockMap() {
        for (auto& page : l1_directory_) {
            BlockPage* p = page.load(std::memory_order_relaxed);
            if (p) delete p;
        }
    }

    void* Lookup(uint32_t pc, bool /* is_thumb */) {
        if (pc < kExecStart || pc >= kExecEnd) return nullptr;

        uint32_t offset = pc - kExecStart;
        uint32_t index = offset >> 1;  // 2-byte resolution

        uint32_t l1_idx = index >> 13;
        BlockPage* page = l1_directory_[l1_idx].load(std::memory_order_acquire);

        if (!page) return nullptr;

        uint32_t l2_idx = index & 0x1FFF;
        return page->entries[l2_idx].load(std::memory_order_acquire);
    }

    bool TryClaim(uint32_t pc, bool /* is_thumb */) {
        if (pc < kExecStart || pc >= kExecEnd) return false;
        void* expected = nullptr;
        return GetAtomicRef(pc).compare_exchange_strong(expected, kClaimed,
                                                        std::memory_order_acq_rel);
    }

    void PublishIfClaimed(uint32_t pc, bool /* is_thumb */, void* host_code) {
        if (pc < kExecStart || pc >= kExecEnd) return;
        void* expected = kClaimed;
        GetAtomicRef(pc).compare_exchange_strong(expected, host_code,
                                                 std::memory_order_release);
    }

    void ForcePublish(uint32_t pc, bool /* is_thumb */, void* host_code) {
        if (pc < kExecStart || pc >= kExecEnd) return;
        GetAtomicRef(pc).store(host_code, std::memory_order_release);
    }

private:
    std::atomic<void*>& GetAtomicRef(uint32_t pc) {
        uint32_t offset = pc - kExecStart;
        uint32_t index = offset >> 1;
        uint32_t l1_idx = index >> 13;
        uint32_t l2_idx = index & 0x1FFF;

        BlockPage* page = l1_directory_[l1_idx].load(std::memory_order_acquire);

        if (!page) {
            BlockPage* new_page = new BlockPage();
            BlockPage* expected = nullptr;
            if (l1_directory_[l1_idx].compare_exchange_strong(expected, new_page,
                                                              std::memory_order_acq_rel)) {
                page = new_page;
            } else {
                delete new_page;
                page = expected;
            }
        }
        return page->entries[l2_idx];
    }

    // L1: 128MB range / 16KB per page = 8192 entries
    static constexpr size_t kL1Size = (kExecEnd - kExecStart) >> 1 >> 13;
    std::array<std::atomic<BlockPage*>, kL1Size> l1_directory_;
};
