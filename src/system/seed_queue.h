#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

struct CompileTarget {
    uint32_t pc;
    bool is_thumb;
};

// SPSC queue: JIT pushes seed PCs, PreWarmer pops.
class SeedQueue {
 public:
    bool Push(uint32_t pc, bool is_thumb) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % buffer_.size();
        if (next_tail == head_.load(std::memory_order_acquire)) return false;

        buffer_[current_tail] = {pc, is_thumb};
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool Pop(CompileTarget& req) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        if (current_head == tail_.load(std::memory_order_acquire)) return false;

        req = buffer_[current_head];
        head_.store((current_head + 1) % buffer_.size(), std::memory_order_release);
        return true;
    }

 private:
    std::array<CompileTarget, 2048> buffer_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};
