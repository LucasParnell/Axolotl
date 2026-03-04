#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Per-block profiling and dump payload used by dispatcher analysis output.
struct BlockDump {
    uint32_t pc{0};
    bool is_thumb{false};
    uint32_t block_len{0};
    uint32_t block_cycles{0};
    uint32_t x86_size{0};
    bool captured_from_prewarm{false};
    uint64_t execution_count{0};
    uint64_t total_guest_cycles{0};
    uint64_t first_seen_guest_cycle{0};
    uint64_t last_seen_guest_cycle{0};
    std::vector<uint8_t> arm_bytes;
    std::string ir_text;
    std::vector<uint8_t> x86_bytes;
};
