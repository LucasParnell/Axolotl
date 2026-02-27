#pragma once
#include <cstdint>
#include <array>
#include "util/logger.h"

// Cache for 32-bit constants during IR build; deduplicates by value.
struct LiteralPool {
    static constexpr size_t kMaxLiterals = 64;
    
    std::array<uint32_t, kMaxLiterals> values;
    size_t count = 0;

    void reset() {
        count = 0;
    }

    uint8_t add(uint32_t value) {
        if (count >= kMaxLiterals) {
            Logger::log("LiteralPool: Overflow! Block has too many constants.", LogLevel::ERR);
            return 0; 
        }
        for (size_t i = 0; i < count; ++i) {
            if (values[i] == value) return static_cast<uint8_t>(i);
        }

        values[count] = value;
        return static_cast<uint8_t>(count++);
    }
};