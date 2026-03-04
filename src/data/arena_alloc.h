#pragma once

#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include "../util/logger.h"

// Bump allocator for IR nodes / compilation; no heap in hot path.
class ArenaAllocator {
 public:
    // 256 instructions * 8 bytes = 2KB; fits in L1.
    explicit ArenaAllocator(size_t size_in_bytes = 2048)
        : capacity_(size_in_bytes), used_(0) {
        constexpr size_t alignment = 64;
        capacity_ = (capacity_ + alignment - 1) & ~(alignment - 1);
        data_ = static_cast<uint8_t*>(std::aligned_alloc(alignment, capacity_));
        
        if (!data_) {
            Logger::log("ArenaAllocator: Failed to allocate memory block.", LogLevel::ERR);
        }
    }

    ~ArenaAllocator() { std::free(data_); }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    template <typename T>
    T* Alloc(size_t count = 1) {
        if (!data_) return nullptr;  // OOM at construction (Bug 6.2)
        size_t align = alignof(T);
        size_t aligned_used = (used_ + align - 1) & ~(align - 1);
        size_t total_size = sizeof(T) * count;

        if (aligned_used + total_size > capacity_) {
            Logger::log("ArenaAllocator: Out of memory! Capacity reached.", LogLevel::ERR);
            return nullptr;
        }

        T* ptr = reinterpret_cast<T*>(data_ + aligned_used);
        used_ = aligned_used + total_size;
        return ptr;
    }

    void Reset() {
        used_ = 0;
    }

    uint8_t* RawData() const { return data_; }
    void* GetBasePointer() const { return data_; }
    size_t BytesUsed() const { return used_; }
    size_t Capacity() const { return capacity_; }

 private:
    uint8_t* data_;
    size_t capacity_;
    size_t used_;
};