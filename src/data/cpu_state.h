#pragma once
#include <cstdint>

struct alignas(64) CpuState {
    uint32_t registers[16];

    uint32_t cpsr;
    uint32_t spsr;

    uint32_t block_cycles;

    void** page_table_ptr;
};