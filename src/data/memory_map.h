//
// memory_map.h — flat view of GBA memory for the PPU
//
// The PPU reads DISPCNT, BG registers, palette RAM, VRAM, and OAM through
// this struct. MemoryBus::CopyToMemoryMap() copies into buffers pointed to
// by this struct (PPU-owned snapshots), so the render thread never touches
// bus memory — lock-free, no mutexes.
//

#pragma once

#include <cstdint>

struct MemoryMap {
    // I/O register block 0x04000000–0x040003FF (offsets relative to 0x04000000).
    uint8_t* ioRegs = nullptr;

    // 96 KB VRAM (0x06000000), 1 KB palette (0x05000000), 1 KB OAM (0x07000000).
    uint8_t* vRam = nullptr;
    uint8_t* paletteRam = nullptr;
    uint8_t* oam = nullptr;
};
