#pragma once

#include <atomic>
#include <cstdint>

struct PrewarmPacingState {
    std::atomic<uint64_t> dispatcher_blocks_executed{0};
    std::atomic<uint64_t> prewarmer_blocks_warmed{0};
};
