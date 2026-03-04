#pragma once
#include <cstdint>

// All GBA display timing numbers from GBATEK.
// One cycle = 1/16,777,216 s ≈ 59.59 ns.
namespace GbaTiming {

// Horizontal timing
constexpr int32_t kCyclesPerDot        = 4;
constexpr int32_t kDotsVisible         = 240;
constexpr int32_t kDotsHBlank          = 68;
constexpr int32_t kDotsPerScanline     = 308;   // 240 + 68
constexpr int32_t kCyclesVisible       = 960;   // 240 * 4
// H-Blank flag asserts at cycle 1006, not 960 (GBATEK note).
// PPU renders at 960; flag / IRQ at 1006. We use 1006 for the flag.
constexpr int32_t kCyclesHBlankFlag    = 1006;
constexpr int32_t kCyclesPerScanline   = 1232;  // 308 * 4

// Vertical timing
constexpr int32_t kScanlinesVisible    = 160;
constexpr int32_t kScanlinesVBlank     = 68;
constexpr int32_t kScanlinesTotal      = 228;
constexpr int32_t kCyclesPerFrame      = 280896; // 228 * 1232
// First cycle of VBlank (DISPSTAT bit 0 set, VBlank IRQ): start of scanline 160 (per GBATEK).
constexpr int32_t kCyclesVBlankStart   = kScanlinesVisible * kCyclesPerScanline;

// Instruction fetch S-cycle cost by memory region.
// ROM bus is 16-bit; ARM (32-bit) needs two sequential fetches.
// Default WAITCNT: WS0 = 4N/2S -> 5N/3S cycles.
// EWRAM: 16-bit bus, 2 waitstates -> 3S/6N cycles.
// BIOS/IWRAM/IO/Palette/VRAM/OAM: 32-bit bus, 0 WS -> 1S/1N.
inline constexpr uint32_t FetchS(uint32_t region, bool is_thumb) {
    switch (region) {
        case 0x02:                                    // EWRAM
            return is_thumb ? 3u : 6u;
        case 0x08: case 0x09:                         // ROM WS0
        case 0x0A: case 0x0B:                         // ROM WS1
        case 0x0C: case 0x0D:                         // ROM WS2
            return is_thumb ? 3u : 6u;
        default:                                      // BIOS, IWRAM, IO, VRAM…
            return 1u;
    }
}

// Non-sequential (first fetch in block / after branch).
inline constexpr uint32_t FetchN(uint32_t region, bool is_thumb) {
    switch (region) {
        case 0x02:
            return is_thumb ? 6u : 12u;
        case 0x08: case 0x09:
        case 0x0A: case 0x0B:
        case 0x0C: case 0x0D:
            return is_thumb ? 5u : 10u;
        default:
            return 1u;
    }
}

}  // namespace GbaTiming
