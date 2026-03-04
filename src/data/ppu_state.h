//
// ppu_state.h — plain data for the PPU subsystem
//
// Split into three distinct structures for cache-friendly, thread-ready rendering:
// - FrameSnapshot: immutable per frame (written at VBlank, read-only during render).
// - ScanlineCtx: per-scanline scratch (stack/pool per worker, no sharing).
// - Framebuffer: output rows (workers write non-overlapping rows).
//

#pragma once

#include <cstdint>

struct MemoryMap;

// Packed pixel format in bgLine[] / objLine[]:
//   bits  0–14 : BGR555 colour
//   bit   15   : opaque flag
//   bits 16–17 : priority (0 = highest)
//   bit   18   : OBJ window pixel (objLine only; for WINOUT OBJ window)
//   bit   31   : OBJ semi-transparent (mode 1; alpha blend with layer below)
constexpr uint32_t kPixelOpaqueBit = 1u << 15;
constexpr uint32_t kObjWinBit      = 1u << 18;
constexpr uint32_t kSemiTransBit   = 1u << 31;

constexpr int kPpuScreenWidth  = 240;
constexpr int kPpuScreenHeight = 160;
constexpr uint32_t kVramSize    = 96 * 1024;
constexpr uint32_t kPaletteSize = 1024;
constexpr uint32_t kOamSize     = 1024;
constexpr uint32_t kIoRegsSize  = 0x400;

// 1. Immutable for the entire frame — written once at VBlank, read-only thereafter.
//    Aligned to cache line; never written during rendering.
struct alignas(64) FrameSnapshot {
    uint8_t  vram   [96  * 1024];
    uint8_t  palette[1   * 1024];
    uint8_t  oam    [1   * 1024];
    uint8_t  io     [1   * 1024];

    // Affine reference point latches — seeded from io[] at VBlank, one entry per scanline.
    // Renderer uses snap->bg2x_latch[line] etc.; no IO reads inside scanline loop.
    int32_t  bg2x_latch[160];
    int32_t  bg2y_latch[160];
    int32_t  bg3x_latch[160];
    int32_t  bg3y_latch[160];
};

// 2. Per-scanline scratch — stack- or pool-allocated by each worker. Entirely private.
struct ScanlineCtx {
    uint32_t bgLine [4][kPpuScreenWidth];   // packed: [17:16] priority | [15] opaque | [14:0] BGR555
    uint32_t objLine[kPpuScreenWidth];
    uint8_t  windowMask[kPpuScreenWidth];   // bit 0..4: which layers visible at this pixel (BG0..BG3, OBJ)
    const FrameSnapshot* snap = nullptr;
};

// 3. Output — one RGB888 row per scanline. Workers write non-overlapping rows.
struct Framebuffer {
    static constexpr int kWidth  = kPpuScreenWidth;
    static constexpr int kHeight = kPpuScreenHeight;
    uint8_t pixels[kHeight][kWidth * 3];   // [line][x * 3] R, G, B
};

// Legacy PpuState: kept for constants and any non-rendering state. Render path uses
// FrameSnapshot + ScanlineCtx + Framebuffer only.
struct PpuState {
    static constexpr int kScreenWidth  = kPpuScreenWidth;
    static constexpr int kScreenHeight = kPpuScreenHeight;
    static constexpr uint32_t kPixelOpaqueBit = ::kPixelOpaqueBit;
    static constexpr uint32_t kVramSize       = ::kVramSize;
    static constexpr uint32_t kPaletteSize    = ::kPaletteSize;
    static constexpr uint32_t kOamSize        = ::kOamSize;
    static constexpr uint32_t kIoRegsSize     = ::kIoRegsSize;
};
