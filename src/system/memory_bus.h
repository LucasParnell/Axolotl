#pragma once
#include <cstdint>
#include <vector>
#include <string>

class MemoryBus {
    public:
        MemoryBus();

        bool LoadBios(const std::string& filepath);

        void** GetPageTablePtr(){ return page_table_; }

        void Write32(uint32_t addr, uint32_t value);

        void Write16(uint32_t addr, uint16_t value);

        void Write8(uint32_t addr, uint8_t value);

        uint32_t Read32(uint32_t addr, uint32_t current_pc) const;

        uint16_t Read16(uint32_t addr, uint32_t current_pc) const;

        uint8_t Read8(uint32_t addr, uint32_t current_pc = 0) const;

    private:

        void MapRegion(uint32_t virtual_start, uint32_t virtual_end, uint32_t physical_size, uint8_t* host_ptr);

        uint32_t GetOpenBus(uint32_t current_pc) const;

        void* page_table_[262144];

        std::vector<uint8_t> bios_;
        std::vector<uint8_t> ewram_;
        std::vector<uint8_t> iwram_;
        std::vector<uint8_t> palette_;
        std::vector<uint8_t> vram_;
        std::vector<uint8_t> oam_;
        std::vector<uint8_t> rom_;

};
