#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VarintCodec {

inline void EncodeU64(uint64_t value, std::vector<uint8_t>* out) {
    while (value >= 0x80u) {
        out->push_back(static_cast<uint8_t>(value) | 0x80u);
        value >>= 7;
    }
    out->push_back(static_cast<uint8_t>(value));
}

inline bool DecodeU64(const uint8_t* data, size_t size, size_t* pos, uint64_t* out) {
    uint64_t value = 0;
    uint32_t shift = 0;
    while (*pos < size && shift <= 63u) {
        const uint8_t byte = data[(*pos)++];
        value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
        shift += 7u;
    }
    return false;
}

}  // namespace VarintCodec

