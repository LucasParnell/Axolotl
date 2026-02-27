#include "memory_bus.h"
#include <fstream>
#include <bit>


constexpr uint32_t kBiosSize    = 16 * 1024;   // 16 KB
constexpr uint32_t kEwramSize   = 256 * 1024;  // 256 KB
constexpr uint32_t kIwramSize   = 32 * 1024;   // 32 KB
constexpr uint32_t kPaletteSize = 1 * 1024;    // 1 KB
constexpr uint32_t kVramSize    = 96 * 1024;   // 96 KB
constexpr uint32_t kOamSize     = 1 * 1024;    // 1 KB
constexpr uint32_t kRomSize     = 32 * 1024 * 1024; // 32MB

MemoryBus::MemoryBus() {
    for (int i=0; i<262144; ++i){
        page_table_[i] = nullptr;
    }

    bios_.resize(kBiosSize, 0);
    ewram_.resize(kEwramSize, 0);
    iwram_.resize(kIwramSize, 0);
    palette_.resize(kPaletteSize, 0);
    vram_.resize(kVramSize, 0);
    oam_.resize(kOamSize, 0);
    rom_.resize(kRomSize, 0);

    MapRegion(0x02000000, 0x02FFFFFF, ewram_.size(), ewram_.data());
    MapRegion(0x03000000, 0x03FFFFFF, kIwramSize, iwram_.data());
    MapRegion(0x05000000, 0x05FFFFFF, kPaletteSize, palette_.data());
    MapRegion(0x06000000, 0x06FFFFFF, kVramSize, vram_.data());
    MapRegion(0x07000000, 0x07FFFFFF, kOamSize, oam_.data());
    MapRegion(0x08000000, 0x0DFFFFFF, kRomSize, rom_.data());
}


// >> 14 = 16KB page index. host_addr = page_table_[idx] + gba_addr.
void MemoryBus::MapRegion(uint32_t virtual_start, uint32_t virtual_end, 
                          uint32_t physical_size, uint8_t* host_ptr) {
    uint32_t start_page = virtual_start >> 14;
    uint32_t end_page = virtual_end >> 14;

    for (uint32_t i = start_page; i <= end_page; ++i) {
        uint32_t page_virtual_addr = i << 14;
        uint32_t mirror_offset = (page_virtual_addr - virtual_start) % physical_size;
        uint8_t* offset_ptr = (host_ptr + mirror_offset) - page_virtual_addr;
        page_table_[i] = static_cast<void*>(offset_ptr);
    }
}


uint32_t MemoryBus::GetOpenBus(uint32_t current_pc) const {
    return 0x00000000;
}

void MemoryBus::Write32(uint32_t addr, uint32_t value) {
    uint32_t aligned_addr = addr & ~3;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
#ifdef B_DEBUG
        if (region == 0x00) return;
#else
        if (region == 0x00 || region >= 0x08) return; 
#endif

        *reinterpret_cast<uint32_t*>(host_ptr + aligned_addr) = value;
    } else {
        // TODO: Handle I/O 32-bit writes
    }
}

void MemoryBus::Write16(uint32_t addr, uint16_t value) {
    uint32_t aligned_addr = addr & ~1;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
#ifdef B_DEBUG
        if (region == 0x00) return;
#else
        if (region == 0x00 || region >= 0x08) return; 
#endif

        *reinterpret_cast<uint16_t*>(host_ptr + aligned_addr) = value;
    } else {
        // TODO: Handle I/O 16-bit writes
    }
}

void MemoryBus::Write8(uint32_t addr, uint8_t value) {
    uint32_t page = addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = addr >> 24;
        
#ifdef B_DEBUG
        if (region == 0x00) return;
#else
        if (region == 0x00 || region >= 0x08) return; 
#endif

        // VRAM 8-bit: hardware forces 16-bit write (val | val << 8). Palette/OAM: ignore.
        if (region == 0x05 || region == 0x06 || region == 0x07) {
            if (region == 0x06) {
                uint32_t aligned_addr = addr & ~1;
                uint16_t forced_16bit = value | (value << 8);
                *reinterpret_cast<uint16_t*>(host_ptr + aligned_addr) = forced_16bit;
            }
            return;
        }

        *(host_ptr + addr) = value;
    } else {
        // TODO: Handle I/O 8-bit writes
    }
}

uint32_t MemoryBus::Read32(uint32_t addr, uint32_t current_pc) const {
    uint32_t aligned_addr = addr & ~3;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    uint32_t data = 0;

    if (host_ptr != nullptr) {
        if (aligned_addr < 0x4000 && current_pc >= 0x4000) {
            data = GetOpenBus(current_pc);
        } else {
            data = *reinterpret_cast<uint32_t*>(host_ptr + aligned_addr);
        }
    } else {
        if ((aligned_addr >> 24) == 0x04) {
            data = 0; 
        } else {
            data = GetOpenBus(current_pc);
        }
    }

    uint32_t shift_bits = (addr & 3) * 8;
    return std::rotr(data, shift_bits); 
}

uint16_t MemoryBus::Read16(uint32_t addr, uint32_t current_pc) const {
    uint32_t aligned_addr = addr & ~1;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    uint16_t data = 0;

    if (host_ptr != nullptr) {
        if (aligned_addr < 0x4000 && current_pc >= 0x4000) {
            data = static_cast<uint16_t>(GetOpenBus(current_pc));
        } else {
            data = *reinterpret_cast<uint16_t*>(host_ptr + aligned_addr);
        }
    } else {
        // TODO: Handle I/O 16-bit
        data = static_cast<uint16_t>(GetOpenBus(current_pc));
    }

    if (addr & 1) {
        data = std::rotr(data, 8);
    }
    return data;
}

uint8_t MemoryBus::Read8(uint32_t addr, uint32_t current_pc) const {
    uint32_t page = addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        if (addr < 0x4000 && current_pc >= 0x4000) {
            uint32_t open_bus = GetOpenBus(current_pc);
            return static_cast<uint8_t>(open_bus >> ((addr & 3) * 8));
        } else {
            return *(host_ptr + addr);
        }
    } else {
        if ((addr >> 24) == 0x04) {
            return 0; 
        } else {
            uint32_t open_bus = GetOpenBus(current_pc);
            return static_cast<uint8_t>(open_bus >> ((addr & 3) * 8));
        }
    }
}


bool MemoryBus::LoadBios(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    file.read(reinterpret_cast<char*>(bios_.data()), kBiosSize);
    file.close();

    page_table_[0] = static_cast<void*>(bios_.data());

    return true;
}