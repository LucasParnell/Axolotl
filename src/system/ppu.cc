//
// ppu.cc — GBA Pixel Processing Unit (scanline renderer)
//

#include "system/ppu.h"
#include "system/memory_bus.h"
#include "util/logger.h"

// Diagnostics counters provided by memory_bus.cc.
extern uint32_t DebugGetVramWriteCount();
extern uint32_t DebugGetPalWriteCount();
extern uint32_t DebugGetOamWriteCount();
extern void     DebugResetGfxWriteCounts();

#ifndef PPU_DEBUG_LOGS
#define PPU_DEBUG_LOGS 0
#endif

#ifndef PPU_FRAME_INFO_LOGS
#define PPU_FRAME_INFO_LOGS 0
#endif

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

// ── I/O register offsets (relative to 0x04000000) ─────────────────────
namespace PPUReg {
    constexpr uint32_t kDispcnt = 0x000;
    constexpr uint32_t kBg0cnt  = 0x008;
    constexpr uint32_t kBg0hofs = 0x010;
    constexpr uint32_t kBg0vofs = 0x012;
    // Affine BG2
    constexpr uint32_t kBg2pa   = 0x020;
    constexpr uint32_t kBg2pb   = 0x022;
    // constexpr uint32_t kBg2pc   = 0x024;  // currently unused
    constexpr uint32_t kBg2pd   = 0x026;
    constexpr uint32_t kBg2x    = 0x028;  // 32-bit
    constexpr uint32_t kBg2y    = 0x02C;  // 32-bit
    // Affine BG3
    constexpr uint32_t kBg3pa   = 0x030;
    constexpr uint32_t kBg3pb   = 0x032;
    // constexpr uint32_t kBg3pc   = 0x034;  // currently unused
    constexpr uint32_t kBg3pd   = 0x036;
    constexpr uint32_t kBg3x    = 0x038;  // 32-bit
    constexpr uint32_t kBg3y    = 0x03C;  // 32-bit
    // Windows
    constexpr uint32_t kWin0h   = 0x040;
    constexpr uint32_t kWin1h   = 0x042;
    constexpr uint32_t kWin0v   = 0x044;
    constexpr uint32_t kWin1v   = 0x046;
    constexpr uint32_t kWinin   = 0x048;
    constexpr uint32_t kWinout  = 0x04A;
    // Blend
    constexpr uint32_t kBldcnt   = 0x050;
    constexpr uint32_t kBldalpha = 0x052;
    constexpr uint32_t kBldy     = 0x054;
}  // namespace PPUReg

// ── Helpers (snapshot uses io[] instead of MemoryMap) ───────────────────

static inline uint16_t ioReg16(const uint8_t* io, uint32_t offset) {
    return static_cast<uint16_t>(io[offset])
         | (static_cast<uint16_t>(io[offset + 1]) << 8);
}

static inline int32_t readAffineRef(const uint8_t* io, uint32_t off) {
    uint32_t raw = static_cast<uint32_t>(io[off])
                 | (static_cast<uint32_t>(io[off + 1]) << 8)
                 | (static_cast<uint32_t>(io[off + 2]) << 16)
                 | (static_cast<uint32_t>(io[off + 3]) << 24);
    raw &= 0x0FFFFFFFu;
    if (raw & 0x08000000u) raw |= 0xF0000000u;
    return static_cast<int32_t>(raw);
}

// Fix 5 — replicate top bits for full 5→8 bit precision
static inline void bgr555toRgb888(uint16_t bgr, uint8_t* dst) {
    uint8_t r = (bgr        & 0x1F);
    uint8_t g = ((bgr >>  5) & 0x1F);
    uint8_t b = ((bgr >> 10) & 0x1F);
    dst[0] = (r << 3) | (r >> 2);
    dst[1] = (g << 3) | (g >> 2);
    dst[2] = (b << 3) | (b >> 2);
}

static inline uint32_t bgCntOffset(int bg) { return PPUReg::kBg0cnt  + bg * 2; }
static inline uint32_t bgHofsOffset(int bg) { return PPUReg::kBg0hofs + bg * 4; }
static inline uint32_t bgVofsOffset(int bg) { return PPUReg::kBg0vofs + bg * 4; }

// ── Background renderers (take ScanlineCtx + snapshot) ──────────────────

// Fix 8 — tile-stepping: one map lookup per 8-pixel tile instead of per pixel
static void renderRegularBG(ScanlineCtx& ctx, int bg, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* io = snap->io;
    const uint8_t* vram = snap->vram;
    const uint8_t* palette = snap->palette;

    uint16_t bgcnt = ioReg16(io, bgCntOffset(bg));
    int priority      = bgcnt & 0x3;
    int charBaseBlock = (bgcnt >> 2) & 0x3;
    bool color256     = (bgcnt >> 7) & 1;
    int scrBaseBlock  = (bgcnt >> 8) & 0x1F;
    int bgSize        = (bgcnt >> 14) & 0x3;

    int mapW = 32, mapH = 32;
    switch (bgSize) {
        case 1: mapW = 64; mapH = 32; break;
        case 2: mapW = 32; mapH = 64; break;
        case 3: mapW = 64; mapH = 64; break;
    }
    int mapWMask = mapW * 8 - 1;
    int mapWMaskTiles = mapW - 1;

    uint16_t hofs = ioReg16(io, bgHofsOffset(bg)) & 0x1FF;
    uint16_t vofs = ioReg16(io, bgVofsOffset(bg)) & 0x1FF;
    int y = (line + vofs) % (mapH * 8);
    int tileY = y / 8;
    int pixelY = y & 7;

    uint32_t charBase = charBaseBlock * 0x4000u;
    uint32_t scrBase  = scrBaseBlock  * 0x800u;

    int x0 = (0 + hofs) & mapWMask;
    int screenX = 0;
    while (screenX < kPpuScreenWidth) {
        int mapX = (x0 + screenX) & mapWMask;
        int tileX = (mapX / 8) & mapWMaskTiles;
        int pixelXstart = mapX & 7;
        int colsLeft = 8 - pixelXstart;
        colsLeft = std::min(colsLeft, kPpuScreenWidth - screenX);

        uint32_t scrBlockOffset = 0;
        int tx = tileX, ty = tileY;
        if (mapW == 64 && tx >= 32) { scrBlockOffset += 0x800;  tx -= 32; }
        if (mapH == 64 && ty >= 32) { scrBlockOffset += (mapW == 64) ? 0x1000u : 0x800u; ty -= 32; }
        uint32_t mapAddr = (scrBase + scrBlockOffset + (ty * 32 + tx) * 2) & 0xFFFF;
        uint16_t mapEntry = static_cast<uint16_t>(vram[mapAddr])
                          | (static_cast<uint16_t>(vram[mapAddr + 1]) << 8);
        int tileNum = mapEntry & 0x3FF;
        bool hFlip  = (mapEntry >> 10) & 1;
        bool vFlip  = (mapEntry >> 11) & 1;
        int palNum  = (mapEntry >> 12) & 0xF;
        int py = vFlip ? (7 - pixelY) : pixelY;

        for (int col = 0; col < colsLeft; col++, screenX++) {
            int px = pixelXstart + col;
            int pixelX = hFlip ? (7 - px) : px;
            if (!color256) {
                uint32_t tileAddr = (charBase + tileNum * 32 + py * 4 + (pixelX / 2)) & 0xFFFF;
                uint8_t byte = vram[tileAddr];
                uint8_t colourIdx = (pixelX & 1) ? (byte >> 4) : (byte & 0xF);
                if (colourIdx == 0) continue;
                uint16_t colour = static_cast<uint16_t>(palette[(palNum * 16 + colourIdx) * 2])
                                | (static_cast<uint16_t>(palette[(palNum * 16 + colourIdx) * 2 + 1]) << 8);
                ctx.bgLine[bg][screenX] = (colour & 0x7FFF) | kPixelOpaqueBit
                                        | (static_cast<uint32_t>(priority) << 16);
            } else {
                uint32_t tileAddr = (charBase + tileNum * 64 + py * 8 + pixelX) & 0xFFFF;
                uint8_t colourIdx = vram[tileAddr];
                if (colourIdx == 0) continue;
                uint16_t colour = static_cast<uint16_t>(palette[colourIdx * 2])
                                | (static_cast<uint16_t>(palette[colourIdx * 2 + 1]) << 8);
                ctx.bgLine[bg][screenX] = (colour & 0x7FFF) | kPixelOpaqueBit
                                        | (static_cast<uint32_t>(priority) << 16);
            }
        }
    }
}

// Fix 1 — use precomputed latch for this scanline from snapshot; never read BG2X/BG2Y from IO in loop
static void renderAffineBG(ScanlineCtx& ctx, int bg, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* io = snap->io;
    const uint8_t* vram = snap->vram;
    const uint8_t* palette = snap->palette;

    uint16_t bgcnt = ioReg16(io, bgCntOffset(bg));
    int priority      = bgcnt & 0x3;
    int charBaseBlock = (bgcnt >> 2) & 0x3;
    int scrBaseBlock  = (bgcnt >> 8) & 0x1F;
    bool wrapping     = (bgcnt >> 13) & 1;
    int bgSize        = (bgcnt >> 14) & 0x3;

    int sizeInTiles  = 16 << bgSize;
    int sizeInPixels = sizeInTiles * 8;

    uint32_t charBase = charBaseBlock * 0x4000u;
    uint32_t scrBase  = scrBaseBlock  * 0x800u;

    uint32_t paramBase = (bg == 2) ? PPUReg::kBg2pa : PPUReg::kBg3pa;
    int16_t pa = static_cast<int16_t>(ioReg16(io, paramBase + 0));
    int16_t pc = static_cast<int16_t>(ioReg16(io, paramBase + 4));

    int32_t texX = (bg == 2) ? snap->bg2x_latch[line] : snap->bg3x_latch[line];
    int32_t texY = (bg == 2) ? snap->bg2y_latch[line] : snap->bg3y_latch[line];

    // Trace BG3 VRAM fetches for selected diagnostic points.
#if PPU_DEBUG_LOGS
    if (bg == 3 && line == 80) {
        static int dbg_affine_ctr = 0;
        dbg_affine_ctr++;
        if (dbg_affine_ctr <= 8 || dbg_affine_ctr == 60 || dbg_affine_ctr == 115
            || dbg_affine_ctr == 116 || dbg_affine_ctr == 120 || dbg_affine_ctr == 180) {
            for (int sampleX : {0, 60, 120, 200}) {
                int32_t sTexX = texX + static_cast<int32_t>(pa) * sampleX;
                int32_t sTexY = texY + static_cast<int32_t>(pc) * sampleX;
                int six = sTexX >> 8;
                int siy = sTexY >> 8;
                if (wrapping) { six &= (sizeInPixels-1); siy &= (sizeInPixels-1); }
                int stx = six/8, sty = siy/8;
                uint32_t mAddr = (scrBase + uint32_t(sty)*sizeInTiles + stx) & 0xFFFF;
                uint8_t tNum = vram[mAddr];
                uint32_t tAddr = (charBase + uint32_t(tNum)*64 + (siy&7)*8 + (six&7)) & 0xFFFF;
                uint8_t cIdx = vram[tAddr];
                std::ostringstream ds;
                ds << "[PPU-DBG] AffineBG3 ln80 x=" << sampleX
                   << " tex=(" << six << "," << siy << ")"
                   << " tile=(" << stx << "," << sty << ")"
                   << " mapAddr=0x" << std::hex << mAddr
                   << " tileNum=" << std::dec << (int)tNum
                   << " tileAddr=0x" << std::hex << tAddr
                   << " colIdx=" << std::dec << (int)cIdx;
                Logger::log(ds.str(), LogLevel::DEBUG);
            }
            // Also dump tile 0 bytes to verify source tile content.
            std::ostringstream t0;
            t0 << "[PPU-DBG] Tile0 @charBase=0x" << std::hex << charBase << " bytes[0..15]:";
            for (int i = 0; i < 16; i++)
                t0 << " " << std::hex << std::setw(2) << std::setfill('0') << (int)vram[charBase + i];
            Logger::log(t0.str(), LogLevel::DEBUG);
        }
    }
#endif
    // End diagnostic trace block.

    for (int screenX = 0; screenX < kPpuScreenWidth; screenX++) {
        int ix = texX >> 8;
        int iy = texY >> 8;
        texX += pa;
        texY += pc;

        if (wrapping) {
            ix &= (sizeInPixels - 1);
            iy &= (sizeInPixels - 1);
        } else if (ix < 0 || ix >= sizeInPixels || iy < 0 || iy >= sizeInPixels) {
            continue;
        }

        int tileX = ix / 8;
        int tileY = iy / 8;
        uint32_t mapAddr = (scrBase + static_cast<uint32_t>(tileY) * sizeInTiles + tileX) & 0xFFFF;
        uint8_t tileNum = vram[mapAddr];
        uint32_t tileAddr = (charBase + static_cast<uint32_t>(tileNum) * 64 + (iy & 7) * 8 + (ix & 7)) & 0xFFFF;
        uint8_t colourIdx = vram[tileAddr];
        if (colourIdx == 0) continue;

        uint16_t colour = static_cast<uint16_t>(palette[colourIdx * 2])
                        | (static_cast<uint16_t>(palette[colourIdx * 2 + 1]) << 8);
        ctx.bgLine[bg][screenX] = (colour & 0x7FFF) | kPixelOpaqueBit
                               | (static_cast<uint32_t>(priority) << 16);
    }
}

// ── Sprite renderers ───────────────────────────────────────────────────

static constexpr int kSprSizeTable[3][4][2] = {
    {{8,8},  {16,16}, {32,32}, {64,64}},
    {{16,8}, {32,8},  {32,16}, {64,32}},
    {{8,16}, {8,32},  {16,32}, {32,64}},
};

static void renderRegularSprite(ScanlineCtx& ctx, int objIndex, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* oam = snap->oam;
    const uint8_t* vram = snap->vram;
    const uint8_t* palette = snap->palette;
    const uint8_t* io = snap->io;

    uint32_t oamBase = objIndex * 8;
    uint16_t attr0 = static_cast<uint16_t>(oam[oamBase]) | (static_cast<uint16_t>(oam[oamBase + 1]) << 8);
    uint16_t attr1 = static_cast<uint16_t>(oam[oamBase + 2]) | (static_cast<uint16_t>(oam[oamBase + 3]) << 8);
    uint16_t attr2 = static_cast<uint16_t>(oam[oamBase + 4]) | (static_cast<uint16_t>(oam[oamBase + 5]) << 8);

    uint16_t dispcnt = ioReg16(io, PPUReg::kDispcnt);
    bool mapping1D = (dispcnt >> 6) & 1;
    uint32_t objTileBase = (dispcnt & 0x7) >= 3 ? 0x14000u : 0x10000u;

    int sprY = attr0 & 0xFF;
    if (sprY >= 160) sprY -= 256;
    bool color256 = (attr0 >> 13) & 1;
    int shape = (attr0 >> 14) & 3;
    int size  = (attr1 >> 14) & 3;
    int sprW = kSprSizeTable[shape][size][0];
    int sprH = kSprSizeTable[shape][size][1];
    int sprX = attr1 & 0x1FF;
    if (sprX >= 240) sprX -= 512;
    bool hFlip = (attr1 >> 12) & 1;
    bool vFlip = (attr1 >> 13) & 1;
    int tileNum  = attr2 & 0x3FF;
    int priority = (attr2 >> 10) & 3;
    int palNum   = (attr2 >> 12) & 0xF;
    int objMode  = (attr0 >> 10) & 3;  // 0=normal, 1=semi-trans, 2=OBJ window, 3=invalid
    if (objMode == 3) return;
    bool debugWatch = PPU_DEBUG_LOGS && (line == 110) &&
                      (objIndex == 7 || objIndex == 8 || objIndex == 23 || objIndex == 24);
    bool drewAnyPixel = false;
    int localY = line - sprY;
    if (vFlip) localY = sprH - 1 - localY;

    for (int localX = 0; localX < sprW; localX++) {
        int screenX = sprX + localX;
        if (screenX < 0 || screenX >= kPpuScreenWidth) continue;
        int px = hFlip ? (sprW - 1 - localX) : localX;
        int py = localY;
        int tileRow = py / 8, tileCol = px / 8;
        int withinX = px & 7, withinY = py & 7;
        int tileIndex = mapping1D
            ? (color256 ? tileNum + (tileRow * (sprW / 8)) * 2 + tileCol * 2
                        : tileNum + tileRow * (sprW / 8) + tileCol)
            : (color256 ? tileNum + tileRow * 32 + tileCol * 2
                        : tileNum + tileRow * 32 + tileCol);

        uint8_t colourIdx = 0;
        uint16_t colour = 0;
        if (!color256) {
            uint32_t addr = objTileBase + tileIndex * 32 + withinY * 4 + (withinX / 2);
            if (addr >= kVramSize) continue;
            uint8_t byte = vram[addr];
            colourIdx = (withinX & 1) ? (byte >> 4) : (byte & 0xF);
            if (colourIdx == 0) continue;
            uint32_t palAddr = 0x200 + (palNum * 16 + colourIdx) * 2;
            colour = static_cast<uint16_t>(palette[palAddr]) | (static_cast<uint16_t>(palette[palAddr + 1]) << 8);
        } else {
            uint32_t addr = objTileBase + tileIndex * 32 + withinY * 8 + withinX;
            if (addr >= kVramSize) continue;
            colourIdx = vram[addr];
            if (colourIdx == 0) continue;
            uint32_t palAddr = 0x200 + colourIdx * 2;
            colour = static_cast<uint16_t>(palette[palAddr]) | (static_cast<uint16_t>(palette[palAddr + 1]) << 8);
        }
        if (objMode == 2) {
            if (colourIdx != 0) {
                ctx.objLine[screenX] |= kObjWinBit; // Inject window bit safely
                drewAnyPixel = true;
            }
            continue;
        }
        
        if (objMode == 1) {
            uint32_t existing = ctx.objLine[screenX];
            if (!(existing & kPixelOpaqueBit) &&
                (!(existing & kSemiTransBit) || priority <= static_cast<int>((existing >> 16) & 3))) {
                // Draw semi-trans pixel, but PRESERVE the window bit if it was already set
                ctx.objLine[screenX] = (colour & 0x7FFF) | (static_cast<uint32_t>(priority) << 16) | kSemiTransBit | (existing & kObjWinBit);
                drewAnyPixel = true;
            }
            continue;
        }
        
        uint32_t existing = ctx.objLine[screenX];
        if (!(existing & (kPixelOpaqueBit | kSemiTransBit)) || priority < static_cast<int>((existing >> 16) & 3)) {
            // Draw opaque pixel, but PRESERVE the window bit if it was already set
            ctx.objLine[screenX] = (colour & 0x7FFF) | kPixelOpaqueBit | (static_cast<uint32_t>(priority) << 16) | (existing & kObjWinBit);
            drewAnyPixel = true;
        }
    }
    if (debugWatch) {
        static int budget = 40;
        if (budget-- > 0) {
            int dbgTileRow = localY / 8;
            int dbgTileIndex = mapping1D
                ? (color256 ? tileNum + (dbgTileRow * (sprW / 8)) * 2
                            : tileNum + dbgTileRow * (sprW / 8))
                : (color256 ? tileNum + dbgTileRow * 32
                            : tileNum + dbgTileRow * 32);
            uint32_t dbgAddr = objTileBase + dbgTileIndex * 32;
            int dbgNonZero = 0;
            for (int i = 0; i < 64 && (dbgAddr + static_cast<uint32_t>(i)) < kVramSize; i++) {
                dbgNonZero += (vram[dbgAddr + static_cast<uint32_t>(i)] != 0);
            }
            std::ostringstream s;
            s << "[PPU-DBG] SPR-R idx=" << objIndex
              << " line=" << line
              << " mode=" << objMode
              << " x=" << sprX << " y=" << sprY
              << " w=" << sprW << " h=" << sprH
              << " tile=" << tileNum
              << " prio=" << priority
              << " 256col=" << color256
              << " map1D=" << mapping1D
              << " base=0x" << std::hex << objTileBase
              << " dbgAddr=0x" << dbgAddr
              << " dbgNZ64=" << std::dec << dbgNonZero
              << " drew=" << std::dec << (drewAnyPixel ? 1 : 0);
            Logger::log(s.str(), LogLevel::DEBUG);
        }
    }
}

// Fix 2 — OBJ tile base only moves to 0x14000 in modes 3, 4, 5 (bitmap), not mode 2
static void renderAffineSprite(ScanlineCtx& ctx, int objIndex, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* oam = snap->oam;
    const uint8_t* vram = snap->vram;
    const uint8_t* palette = snap->palette;
    const uint8_t* io = snap->io;

    uint32_t oamBase = objIndex * 8;
    uint16_t attr0 = static_cast<uint16_t>(oam[oamBase]) | (static_cast<uint16_t>(oam[oamBase + 1]) << 8);
    uint16_t attr1 = static_cast<uint16_t>(oam[oamBase + 2]) | (static_cast<uint16_t>(oam[oamBase + 3]) << 8);
    uint16_t attr2 = static_cast<uint16_t>(oam[oamBase + 4]) | (static_cast<uint16_t>(oam[oamBase + 5]) << 8);

    uint16_t dispcnt = ioReg16(io, PPUReg::kDispcnt);
    bool mapping1D = (dispcnt >> 6) & 1;
    uint32_t objTileBase = (dispcnt & 0x7) >= 3 ? 0x14000u : 0x10000u;

    int sprY = attr0 & 0xFF;
    if (sprY >= 160) sprY -= 256;
    bool doubleSize = (attr0 >> 9) & 1;
    bool color256   = (attr0 >> 13) & 1;
    int shape = (attr0 >> 14) & 3;
    int size  = (attr1 >> 14) & 3;
    if (shape > 2) return;
    int sprW = kSprSizeTable[shape][size][0];
    int sprH = kSprSizeTable[shape][size][1];
    int boundsW = doubleSize ? sprW * 2 : sprW;
    int boundsH = doubleSize ? sprH * 2 : sprH;
    int sprX = attr1 & 0x1FF;
    if (sprX >= 240) sprX -= 512;
    int rotGroup = (attr1 >> 9) & 0x1F;
    int tileNum  = attr2 & 0x3FF;
    int priority = (attr2 >> 10) & 3;
    int palNum   = (attr2 >> 12) & 0xF;
    int objMode  = (attr0 >> 10) & 3;  // 0=normal, 1=semi-trans, 2=OBJ window, 3=invalid
    if (objMode == 3) return;
    bool debugWatch = PPU_DEBUG_LOGS && (line == 110) &&
                      (objIndex == 7 || objIndex == 8 || objIndex == 23 || objIndex == 24);
    bool drewAnyPixel = false;

    auto readOamParam = [&](int grp, int idx) -> int16_t {
        uint32_t off = grp * 32 + 6 + idx * 8;
        return static_cast<int16_t>(static_cast<uint16_t>(oam[off]) | (static_cast<uint16_t>(oam[off + 1]) << 8));
    };
    int16_t pa = readOamParam(rotGroup, 0);
    int16_t pb = readOamParam(rotGroup, 1);
    int16_t pc = readOamParam(rotGroup, 2);
    int16_t pd = readOamParam(rotGroup, 3);

    int halfBoundsW = boundsW / 2;
    int halfBoundsH = boundsH / 2;
    int halfSprW = sprW / 2;
    int halfSprH = sprH / 2;
    int localY = line - sprY;

    for (int localX = 0; localX < boundsW; localX++) {
        int screenX = sprX + localX;
        if (screenX < 0 || screenX >= kPpuScreenWidth) continue;
        int dx = localX - halfBoundsW;
        int dy = localY - halfBoundsH;
        int texX = ((pa * dx + pb * dy) >> 8) + halfSprW;
        int texY = ((pc * dx + pd * dy) >> 8) + halfSprH;
        if (texX < 0 || texX >= sprW || texY < 0 || texY >= sprH) continue;

        int tileRow = texY / 8, tileCol = texX / 8;
        int withinX = texX & 7, withinY = texY & 7;
        int tileIndex = mapping1D
            ? (color256 ? tileNum + (tileRow * (sprW / 8)) * 2 + tileCol * 2
                        : tileNum + tileRow * (sprW / 8) + tileCol)
            : (color256 ? tileNum + tileRow * 32 + tileCol * 2
                        : tileNum + tileRow * 32 + tileCol);

        uint8_t colourIdx = 0;
        uint16_t colour = 0;
        if (!color256) {
            uint32_t addr = objTileBase + tileIndex * 32 + withinY * 4 + (withinX / 2);
            if (addr >= kVramSize) continue;
            uint8_t byte = vram[addr];
            colourIdx = (withinX & 1) ? (byte >> 4) : (byte & 0xF);
            if (colourIdx == 0) continue;
            uint32_t palAddr = 0x200 + (palNum * 16 + colourIdx) * 2;
            colour = static_cast<uint16_t>(palette[palAddr]) | (static_cast<uint16_t>(palette[palAddr + 1]) << 8);
        } else {
            uint32_t addr = objTileBase + tileIndex * 32 + withinY * 8 + withinX;
            if (addr >= kVramSize) continue;
            colourIdx = vram[addr];
            if (colourIdx == 0) continue;
            uint32_t palAddr = 0x200 + colourIdx * 2;
            colour = static_cast<uint16_t>(palette[palAddr]) | (static_cast<uint16_t>(palette[palAddr + 1]) << 8);
        }
        if (objMode == 2) {
            if (colourIdx != 0) {
                ctx.objLine[screenX] |= kObjWinBit; // Inject window bit safely
                drewAnyPixel = true;
            }
            continue;
        }
        
        if (objMode == 1) {
            uint32_t existing = ctx.objLine[screenX];
            if (!(existing & kPixelOpaqueBit) &&
                (!(existing & kSemiTransBit) || priority <= static_cast<int>((existing >> 16) & 3))) {
                // Draw semi-trans pixel, but PRESERVE the window bit if it was already set
                ctx.objLine[screenX] = (colour & 0x7FFF) | (static_cast<uint32_t>(priority) << 16) | kSemiTransBit | (existing & kObjWinBit);
                drewAnyPixel = true;
            }
            continue;
        }
        
        uint32_t existing = ctx.objLine[screenX];
        if (!(existing & (kPixelOpaqueBit | kSemiTransBit)) || priority < static_cast<int>((existing >> 16) & 3)) {
            // Draw opaque pixel, but PRESERVE the window bit if it was already set
            ctx.objLine[screenX] = (colour & 0x7FFF) | kPixelOpaqueBit | (static_cast<uint32_t>(priority) << 16) | (existing & kObjWinBit);
            drewAnyPixel = true;
        }
    }
    if (debugWatch) {
        static int budget = 40;
        if (budget-- > 0) {
            std::ostringstream s;
            s << "[PPU-DBG] SPR-A idx=" << objIndex
              << " line=" << line
              << " mode=" << objMode
              << " x=" << sprX << " y=" << sprY
              << " w=" << sprW << " h=" << sprH
              << " tile=" << tileNum
              << " prio=" << priority
              << " 256col=" << color256
              << " map1D=" << mapping1D
              << " base=0x" << std::hex << objTileBase
              << " drew=" << std::dec << (drewAnyPixel ? 1 : 0);
            Logger::log(s.str(), LogLevel::DEBUG);
        }
    }
}

static void renderSprites(ScanlineCtx& ctx, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* oam = snap->oam;
    for (int i = 0; i < 128; i++) {
        uint32_t oamBase = i * 8;
        uint16_t attr0 = static_cast<uint16_t>(oam[oamBase]) | (static_cast<uint16_t>(oam[oamBase + 1]) << 8);
        uint16_t attr1 = static_cast<uint16_t>(oam[oamBase + 2]) | (static_cast<uint16_t>(oam[oamBase + 3]) << 8);
        bool rotScale = (attr0 >> 8) & 1;
        bool disableOrDouble = (attr0 >> 9) & 1;
        if (!rotScale && disableOrDouble) continue;
        int shape = (attr0 >> 14) & 3;
        int size  = (attr1 >> 14) & 3;
        if (shape > 2) continue;
        int sprH = kSprSizeTable[shape][size][1];
        int boundsH = (rotScale && disableOrDouble) ? sprH * 2 : sprH;
        int sprY = attr0 & 0xFF;
        if (sprY >= 160) sprY -= 256;
        if (line < sprY || line >= sprY + boundsH) continue;
        if (rotScale) renderAffineSprite(ctx, i, line);
        else          renderRegularSprite(ctx, i, line);
    }
}

// ── Window mask (WIN0H/V, WIN1H/V, WININ, WINOUT) ─────────────────────────
// windowMask bits: [0]=BG0 [1]=BG1 [2]=BG2 [3]=BG3 [4]=OBJ [5]=colour effects
// When no windows are enabled, all bits are set (everything visible everywhere).
static void buildWindowMask(ScanlineCtx& ctx, int line) {
    const uint8_t* io = ctx.snap->io;
    uint16_t dispcnt = ioReg16(io, PPUReg::kDispcnt);
    bool win0Enabled   = (dispcnt >> 13) & 1;
    bool win1Enabled   = (dispcnt >> 14) & 1;
    bool objWinEnabled = (dispcnt >> 15) & 1;

    // Fast path: no windows active at all — every layer visible, effects on.
    if (!win0Enabled && !win1Enabled && !objWinEnabled) {
        std::memset(ctx.windowMask, 0x3F, kPpuScreenWidth);
        return;
    }

    // GBATEK WIN0H/WIN1H layout: bits [15:8] = X1 (left), bits [7:0] = X2 (right).
    // Stored little-endian, so byte[0] = X2 (right), byte[1] = X1 (left).
    uint8_t win0h_l = io[PPUReg::kWin0h + 1];
    uint8_t win0h_r = io[PPUReg::kWin0h + 0];
    uint8_t win1h_l = io[PPUReg::kWin1h + 1];
    uint8_t win1h_r = io[PPUReg::kWin1h + 0];
    uint8_t win0v_t = io[PPUReg::kWin0v + 1];
    uint8_t win0v_b = io[PPUReg::kWin0v + 0];
    uint8_t win1v_t = io[PPUReg::kWin1v + 1];
    uint8_t win1v_b = io[PPUReg::kWin1v + 0];
    uint16_t winin  = ioReg16(io, PPUReg::kWinin);
    uint16_t winout = ioReg16(io, PPUReg::kWinout);

    // 6-bit masks including colour-effects bit (bit 5).
    uint8_t mask_win0   = static_cast<uint8_t>(winin & 0x3F);
    uint8_t mask_win1   = static_cast<uint8_t>((winin >> 8) & 0x3F);
    uint8_t mask_out    = static_cast<uint8_t>(winout & 0x3F);
    uint8_t mask_objwin = static_cast<uint8_t>((winout >> 8) & 0x3F);

    // Vertical in-range: if top > bottom, window wraps (0..bottom-1 and top..159).
    bool inWin0V = win0Enabled && ((win0v_t <= win0v_b)
        ? (line >= win0v_t && line < win0v_b)
        : (line < win0v_b || line >= win0v_t));
    bool inWin1V = win1Enabled && ((win1v_t <= win1v_b)
        ? (line >= win1v_t && line < win1v_b)
        : (line < win1v_b || line >= win1v_t));

    for (int x = 0; x < kPpuScreenWidth; x++) {
        // Horizontal: if left > right, window wraps (0..right-1 and left..239).
        bool inWin0H = (win0h_l <= win0h_r) ? (x >= win0h_l && x < win0h_r) : (x < win0h_r || x >= win0h_l);
        bool inWin1H = (win1h_l <= win1h_r) ? (x >= win1h_l && x < win1h_r) : (x < win1h_r || x >= win1h_l);
        // WIN0 has highest priority per GBATEK, then WIN1, then OBJ window, then outside.
        if (inWin0V && inWin0H) {
            ctx.windowMask[x] = mask_win0;
        } else if (inWin1V && inWin1H) {
            ctx.windowMask[x] = mask_win1;
        } else if (objWinEnabled && (ctx.objLine[x] & kObjWinBit)) {
            ctx.windowMask[x] = mask_objwin;
        } else {
            ctx.windowMask[x] = mask_out;
        }
    }
}

// ── Fix 3 + Fix 4: Composite with LayerPixel sort and blend target masks ─

struct LayerPixel {
    uint16_t colour;
    int priority;
    int layer_id;
};

static void compositeScanline(ScanlineCtx& ctx, Framebuffer& fb, int line) {
    const FrameSnapshot* snap = ctx.snap;
    const uint8_t* palette = snap->palette;
    const uint8_t* io = snap->io;

    uint16_t backdrop = static_cast<uint16_t>(palette[0]) | (static_cast<uint16_t>(palette[1]) << 8);
    uint16_t bldcnt   = ioReg16(io, PPUReg::kBldcnt);
    uint16_t bldalpha = ioReg16(io, PPUReg::kBldalpha);
    uint8_t  bldy     = io[PPUReg::kBldy] & 0x1F;

    uint8_t* dst = &fb.pixels[line][0];

    for (int x = 0; x < kPpuScreenWidth; x++) {
        LayerPixel candidates[5];
        int n = 0;
        uint8_t wmask = ctx.windowMask[x];

        for (int bg = 0; bg < 4; bg++) {
            if (!(wmask & (1 << bg))) continue;
            uint32_t p = ctx.bgLine[bg][x];
            if (p & kPixelOpaqueBit)
                candidates[n++] = { static_cast<uint16_t>(p & 0x7FFF), static_cast<int>((p >> 16) & 3), bg };
        }
        if ((wmask & (1 << 4)) != 0) {
            uint32_t obj = ctx.objLine[x];
            if (obj & kPixelOpaqueBit)
                candidates[n++] = { static_cast<uint16_t>(obj & 0x7FFF), static_cast<int>((obj >> 16) & 3), 4 };
        }

        // Tie-break: OBJ on top, then BG0 > BG1 > BG2 > BG3 (smaller layer_id = on top after OBJ).
        std::sort(candidates, candidates + n, [](const LayerPixel& a, const LayerPixel& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            int key_a = (a.layer_id == 4) ? -1 : a.layer_id;
            int key_b = (b.layer_id == 4) ? -1 : b.layer_id;
            return key_a < key_b;
        });

        // Determine top/bot pixel colours and BLDCNT layer IDs.
        // Layer IDs: 0-3 = BG0-BG3, 4 = OBJ, 5 = Backdrop (maps to BLDCNT bit positions).
        uint16_t topColour = (n > 0) ? candidates[0].colour : backdrop;
        int      topLayer  = (n > 0) ? candidates[0].layer_id : 5;
        uint16_t botColour = (n > 1) ? candidates[1].colour : backdrop;
        int      botLayer  = (n > 1) ? candidates[1].layer_id : 5;
        uint16_t finalColour = topColour;

        // ── Semi-transparent OBJ (OAM mode 1) ──────────────────────────
        // Per GBATEK: always selected as 1st target (regardless of BLDCNT bit 4)
        // and always uses alpha blending mode (regardless of BLDCNT bits 6-7).
        // However the pixel below must still be in the BLDCNT 2nd-target list.
        // If no 2nd target, fall through to normal BLDCNT effects (brightness).
        bool semiTransOnTop = false;
        uint16_t semiColour = 0;
        bool effectApplied = false;

        if ((wmask & (1 << 4)) != 0) {
            uint32_t obj = ctx.objLine[x];
            if ((obj & kSemiTransBit) &&
                (n == 0 || static_cast<int>((obj >> 16) & 3) <= candidates[0].priority)) {
                semiTransOnTop = true;
                semiColour = obj & 0x7FFF;
                finalColour = semiColour;

                if ((wmask & (1 << 5)) != 0) {
                    // Check BLDCNT 2nd-target for the layer below the OBJ.
                    int belowLayer = (n > 0) ? candidates[0].layer_id : 5;
                    bool belowIs2nd = (bldcnt >> (8 + belowLayer)) & 1;
                    uint16_t belowCol = (n > 0) ? candidates[0].colour : backdrop;

                    if (belowIs2nd) {
                        // Clamp EVA/EVB to 16 (GBATEK: 17-31 = 16/16).
                        int eva = std::min(static_cast<int>(bldalpha & 0x1F), 16);
                        int evb = std::min(static_cast<int>((bldalpha >> 8) & 0x1F), 16);
                        int r = std::min(31, ((semiColour & 0x1F) * eva + (belowCol & 0x1F) * evb) >> 4);
                        int g = std::min(31, (((semiColour >> 5) & 0x1F) * eva + ((belowCol >> 5) & 0x1F) * evb) >> 4);
                        int b = std::min(31, (((semiColour >> 10) & 0x1F) * eva + ((belowCol >> 10) & 0x1F) * evb) >> 4);
                        finalColour = static_cast<uint16_t>(r | (g << 5) | (b << 10));
                        effectApplied = true;
                    }
                    // If not 2nd target, fall through to normal BLDCNT below.
                }
            }
        }

        // ── Normal BLDCNT colour special effects ────────────────────────
        // Gated by window colour-effects bit (bit 5 of wmask).
        if (!effectApplied && (wmask & (1 << 5)) != 0) {
            int effect = (bldcnt >> 6) & 3;

            // When semi-trans OBJ is on top, it overrides the effective
            // top pixel and is always 1st target (ignoring BLDCNT bit 4).
            int effTopLayer, effBotLayer;
            uint16_t effTopColour, effBotColour;
            if (semiTransOnTop) {
                effTopLayer  = 4;  // OBJ
                effTopColour = semiColour;
                effBotLayer  = (n > 0) ? candidates[0].layer_id : 5;
                effBotColour = (n > 0) ? candidates[0].colour   : backdrop;
            } else {
                effTopLayer  = topLayer;
                effTopColour = topColour;
                effBotLayer  = botLayer;
                effBotColour = botColour;
            }

            // Semi-trans OBJ is always 1st target; otherwise check BLDCNT.
            bool topIs1st = semiTransOnTop || ((bldcnt >> effTopLayer) & 1);

            if (effect == 1 && topIs1st) {
                bool botIs2nd = (bldcnt >> (8 + effBotLayer)) & 1;
                if (botIs2nd) {
                    int eva = std::min(static_cast<int>(bldalpha & 0x1F), 16);
                    int evb = std::min(static_cast<int>((bldalpha >> 8) & 0x1F), 16);
                    int r = std::min(31, ((effTopColour & 0x1F) * eva + (effBotColour & 0x1F) * evb) >> 4);
                    int g = std::min(31, (((effTopColour >> 5) & 0x1F) * eva + ((effBotColour >> 5) & 0x1F) * evb) >> 4);
                    int b = std::min(31, (((effTopColour >> 10) & 0x1F) * eva + ((effBotColour >> 10) & 0x1F) * evb) >> 4);
                    finalColour = static_cast<uint16_t>(r | (g << 5) | (b << 10));
                }
            } else if ((effect == 2 || effect == 3) && topIs1st) {
                int evy = std::min(static_cast<int>(bldy), 16);
                int r = effTopColour & 0x1F;
                int g = (effTopColour >> 5) & 0x1F;
                int b = (effTopColour >> 10) & 0x1F;
                if (effect == 2) {
                    r = r + (((31 - r) * evy) >> 4);
                    g = g + (((31 - g) * evy) >> 4);
                    b = b + (((31 - b) * evy) >> 4);
                } else {
                    r = (r * (16 - evy)) >> 4;
                    g = (g * (16 - evy)) >> 4;
                    b = (b * (16 - evy)) >> 4;
                }
                finalColour = static_cast<uint16_t>(r | (g << 5) | (b << 10));
            }
        }

        bgr555toRgb888(finalColour, &dst[x * 3]);
    }
}

// ── PPU implementation ──────────────────────────────────────────────────

PPU::PPU() = default;

void PPU::OnVBlank(const MemoryBus* bus) {
    // Write into whichever snapshot the renderer is NOT currently using.
    // pending_idx_ starts at 0. After publishing, it flips so next VBlank
    // writes to the other buffer. The renderer holds a const* to the published
    // one for the entire frame — we never touch it again until two VBlanks later.
    FrameSnapshot& target = snapshots_[pending_idx_];

    MemoryMap map;
    map.ioRegs   = target.io;
    map.vRam     = target.vram;
    map.paletteRam = target.palette;
    map.oam      = target.oam;
    bus->CopyToMemoryMap(&map);

    // Log PPU state for this frame (DISPCNT snapshot taken after VBlank updates).
    [[maybe_unused]] uint16_t dispcnt = ioReg16(target.io, PPUReg::kDispcnt);
    [[maybe_unused]] uint8_t bldy = target.io[PPUReg::kBldy] & 0x1F;
    [[maybe_unused]] uint16_t bldcnt = ioReg16(target.io, PPUReg::kBldcnt);

#if PPU_FRAME_INFO_LOGS
    int mode = dispcnt & 0x7;
    int forcedBlank = (dispcnt >> 7) & 1;
    int bg0 = (dispcnt >> 8) & 1, bg1 = (dispcnt >> 9) & 1;
    int bg2 = (dispcnt >> 10) & 1, bg3 = (dispcnt >> 11) & 1;
    int obj = (dispcnt >> 12) & 1;

    // Compress repeated identical frames.
    static uint16_t last_dispcnt = 0xFFFF;
    static uint8_t  last_bldy = 0xFF;
    static uint16_t last_bldcnt = 0xFFFF;
    static int      frame_suppress_count = 0;

    bool regs_same = (dispcnt == last_dispcnt && bldy == last_bldy && bldcnt == last_bldcnt);
    if (regs_same) {
        ++frame_suppress_count;
    } else {
        if (frame_suppress_count > 0) {
            Logger::log("[PPU] (" + std::to_string(frame_suppress_count)
                        + " identical frames)", LogLevel::INFO);
        }
        frame_suppress_count = 0;
        last_dispcnt = dispcnt;
        last_bldy = bldy;
        last_bldcnt = bldcnt;
        std::ostringstream msg;
        msg << "[PPU] Frame " << frame_count_
            << " DISPCNT=0x" << std::hex << std::setw(4) << std::setfill('0') << dispcnt
            << std::dec << " mode=" << mode << " forcedBlank=" << forcedBlank
            << " BGs=" << bg0 << bg1 << bg2 << bg3 << " OBJ=" << obj
            << " BLDY=" << static_cast<int>(bldy)
            << " BLDCNT=0x" << std::hex << std::setw(4) << std::setfill('0') << bldcnt;
        Logger::log(msg.str(), LogLevel::INFO);
    }
#endif
    frame_count_++;

    int32_t x2 = readAffineRef(target.io, PPUReg::kBg2x);
    int32_t y2 = readAffineRef(target.io, PPUReg::kBg2y);
    int32_t x3 = readAffineRef(target.io, PPUReg::kBg3x);
    int32_t y3 = readAffineRef(target.io, PPUReg::kBg3y);
    int16_t pb2 = static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg2pb));
    int16_t pd2 = static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg2pd));
    int16_t pb3 = static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg3pb));
    int16_t pd3 = static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg3pd));

    for (int i = 0; i < 160; i++) {
        target.bg2x_latch[i] = x2; x2 += pb2;
        target.bg2y_latch[i] = y2; y2 += pd2;
        target.bg3x_latch[i] = x3; x3 += pb3;
        target.bg3y_latch[i] = y3; y3 += pd3;
    }

#if PPU_DEBUG_LOGS
    // White-screen diagnostics on selected frames to limit log volume.
    if (frame_count_ <= 8 || frame_count_ == 60 || frame_count_ == 115
        || frame_count_ == 116 || frame_count_ == 120 || frame_count_ == 180) {
        // 1) Backdrop color from palette[0].
        uint16_t backdrop_bgr = static_cast<uint16_t>(target.palette[0])
                              | (static_cast<uint16_t>(target.palette[1]) << 8);
        // 2) Count non-zero bytes in VRAM, palette, and OAM.
        int vram_nonzero = 0, pal_nonzero = 0, oam_nonzero = 0;
        for (size_t i = 0; i < sizeof(target.vram); i++) vram_nonzero += (target.vram[i] != 0);
        for (size_t i = 0; i < sizeof(target.palette); i++) pal_nonzero += (target.palette[i] != 0);
        for (size_t i = 0; i < sizeof(target.oam); i++) oam_nonzero += (target.oam[i] != 0);
        // 3) Break VRAM usage into BG-tile and OBJ ranges.
        int vram_bg_tiles = 0, vram_obj = 0;
        for (size_t i = 0; i < 0x10000u; i++) vram_bg_tiles += (target.vram[i] != 0);
        for (size_t i = 0x10000u; i < 0x18000u; i++) vram_obj += (target.vram[i] != 0);
        {
            std::ostringstream dbg;
            dbg << "[PPU-DBG] Frame " << frame_count_
                << " backdrop=0x" << std::hex << std::setw(4) << std::setfill('0') << backdrop_bgr
                << std::dec
                << " VRAM(nonzero=" << vram_nonzero
                << " bg_tile=" << vram_bg_tiles
                << " obj=" << vram_obj << ")"
                << " PAL(nonzero=" << pal_nonzero << ")"
                << " OAM(nonzero=" << oam_nonzero << ")";
            Logger::log(dbg.str(), LogLevel::DEBUG);
        }
        // 4) Log BG2/BG3 control values.
        uint16_t bg2cnt = ioReg16(target.io, 0x00C);
        uint16_t bg3cnt = ioReg16(target.io, 0x00E);
        {
            auto dumpBGCNT = [](const char* name, uint16_t cnt) {
                int prio      = cnt & 3;
                int charBase  = (cnt >> 2) & 3;
                bool mosaic   = (cnt >> 6) & 1;
                bool color256 = (cnt >> 7) & 1;
                int scrBase   = (cnt >> 8) & 0x1F;
                bool wrap     = (cnt >> 13) & 1;
                int size      = (cnt >> 14) & 3;
                std::ostringstream s;
                s << "[PPU-DBG] " << name << "=0x" << std::hex << std::setw(4) << std::setfill('0') << cnt
                  << std::dec << " prio=" << prio
                  << " charBase=" << charBase << " (0x" << std::hex << (charBase * 0x4000) << ")"
                  << std::dec << " scrBase=" << scrBase << " (0x" << std::hex << (scrBase * 0x800) << ")"
                  << std::dec << " size=" << size << " wrap=" << wrap
                  << " 256col=" << color256 << " mosaic=" << mosaic;
                Logger::log(s.str(), LogLevel::DEBUG);
            };
            dumpBGCNT("BG2CNT", bg2cnt);
            dumpBGCNT("BG3CNT", bg3cnt);
        }
        // 5. Affine reference points and params (line 0 and line 80)
        {
            std::ostringstream s;
            s << "[PPU-DBG] Affine BG2 pa=" << static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg2pa))
              << " pb=" << pb2 << " pc=" << static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg2pc))
              << " pd=" << pd2
              << " refX=" << readAffineRef(target.io, PPUReg::kBg2x)
              << " refY=" << readAffineRef(target.io, PPUReg::kBg2y)
              << " | latch[0]=(" << target.bg2x_latch[0] << "," << target.bg2y_latch[0] << ")"
              << " latch[80]=(" << target.bg2x_latch[80] << "," << target.bg2y_latch[80] << ")";
            Logger::log(s.str(), LogLevel::DEBUG);
            std::ostringstream s3;
            s3 << "[PPU-DBG] Affine BG3 pa=" << static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg3pa))
               << " pb=" << pb3 << " pc=" << static_cast<int16_t>(ioReg16(target.io, PPUReg::kBg3pc))
               << " pd=" << pd3
               << " refX=" << readAffineRef(target.io, PPUReg::kBg3x)
               << " refY=" << readAffineRef(target.io, PPUReg::kBg3y)
               << " | latch[0]=(" << target.bg3x_latch[0] << "," << target.bg3y_latch[0] << ")"
               << " latch[80]=(" << target.bg3x_latch[80] << "," << target.bg3y_latch[80] << ")";
            Logger::log(s3.str(), LogLevel::DEBUG);
        }
        // 6. Sample VRAM at BG3's charBase and scrBase to see if there's actual tile data
        {
            uint32_t cBase = ((bg3cnt >> 2) & 3) * 0x4000;
            uint32_t sBase = ((bg3cnt >> 8) & 0x1F) * 0x800;
            int cNonZero = 0, sNonZero = 0;
            for (uint32_t i = 0; i < 0x4000 && (cBase + i) < sizeof(target.vram); i++)
                cNonZero += (target.vram[cBase + i] != 0);
            for (uint32_t i = 0; i < 0x800 && (sBase + i) < sizeof(target.vram); i++)
                sNonZero += (target.vram[sBase + i] != 0);
            std::ostringstream s;
            s << "[PPU-DBG] BG3 charBase VRAM[0x" << std::hex << cBase << "] nonzero=" << std::dec << cNonZero
              << " / scrBase VRAM[0x" << std::hex << sBase << "] nonzero=" << std::dec << sNonZero;
            Logger::log(s.str(), LogLevel::DEBUG);
            // Same for BG2
            uint32_t c2Base = ((bg2cnt >> 2) & 3) * 0x4000;
            uint32_t s2Base = ((bg2cnt >> 8) & 0x1F) * 0x800;
            int c2NZ = 0, s2NZ = 0;
            for (uint32_t i = 0; i < 0x4000 && (c2Base + i) < sizeof(target.vram); i++)
                c2NZ += (target.vram[c2Base + i] != 0);
            for (uint32_t i = 0; i < 0x800 && (s2Base + i) < sizeof(target.vram); i++)
                s2NZ += (target.vram[s2Base + i] != 0);
            std::ostringstream s2;
            s2 << "[PPU-DBG] BG2 charBase VRAM[0x" << std::hex << c2Base << "] nonzero=" << std::dec << c2NZ
               << " / scrBase VRAM[0x" << std::hex << s2Base << "] nonzero=" << std::dec << s2NZ;
            Logger::log(s2.str(), LogLevel::DEBUG);
        }
        // 7. First 8 OAM entries attr0/attr1/attr2
        {
            std::ostringstream s;
            s << "[PPU-DBG] OAM first 4 sprites:";
            for (int i = 0; i < 4; i++) {
                uint32_t base = i * 8;
                uint16_t a0 = target.oam[base] | (target.oam[base+1] << 8);
                uint16_t a1 = target.oam[base+2] | (target.oam[base+3] << 8);
                uint16_t a2 = target.oam[base+4] | (target.oam[base+5] << 8);
                s << " [" << i << "](" << std::hex << a0 << "," << a1 << "," << a2 << ")";
            }
            Logger::log(s.str(), LogLevel::DEBUG);
        }
        // 8. Palette first 16 BG entries + first 4 OBJ entries (GBATEK: 0x000-0x01F BG, 0x200-0x21F OBJ)
        {
            std::ostringstream s;
            s << "[PPU-DBG] PAL BG[0..7]:";
            for (int i = 0; i < 8; i++) {
                uint16_t c = target.palette[i*2] | (target.palette[i*2+1] << 8);
                s << " " << std::hex << std::setw(4) << std::setfill('0') << c;
            }
            s << " | OBJ[0..3]:";
            for (int i = 0; i < 4; i++) {
                uint16_t c = target.palette[0x200 + i*2] | (target.palette[0x200 + i*2+1] << 8);
                s << " " << std::hex << std::setw(4) << std::setfill('0') << c;
            }
            Logger::log(s.str(), LogLevel::DEBUG);
        }        // 9. CPU write counts since last reset (DMA writes also go through Write32/16)
        {
            std::ostringstream s;
            s << "[PPU-DBG] GfxWrites since last frame: VRAM=" << DebugGetVramWriteCount()
              << " PAL=" << DebugGetPalWriteCount()
              << " OAM=" << DebugGetOamWriteCount();
            Logger::log(s.str(), LogLevel::DEBUG);
            DebugResetGfxWriteCounts();
        }
        // 10. OAM full scan — count by mode and state
        {
            int disabled = 0, normal = 0, semi = 0, objwin = 0, affine_cnt = 0;
            std::ostringstream win_s;
            win_s << "[PPU-DBG] OBJWin sprites:";
            int win_count = 0;
            for (int i = 0; i < 128; i++) {
                uint32_t base = i * 8;
                uint16_t a0 = target.oam[base] | (target.oam[base+1] << 8);
                uint16_t a1 = target.oam[base+2] | (target.oam[base+3] << 8);
                bool rs = (a0 >> 8) & 1;
                bool b9 = (a0 >> 9) & 1;
                if (!rs && b9) { disabled++; continue; }
                int mode = (a0 >> 10) & 3;
                if (mode == 3) { disabled++; continue; }
                if (rs) affine_cnt++;
                if (mode == 0) normal++;
                else if (mode == 1) semi++;
                else if (mode == 2) {
                    objwin++;
                    int shape = (a0 >> 14) & 3;
                    int size = (a1 >> 14) & 3;
                    if (shape <= 2) {
                        int sprY = a0 & 0xFF;
                        if (sprY >= 160) sprY -= 256;
                        int sprH = kSprSizeTable[shape][size][1];
                        int sprW = kSprSizeTable[shape][size][0];
                        int sprX = a1 & 0x1FF;
                        if (sprX >= 240) sprX -= 512;
                        if (win_count < 6) {
                            win_s << " [" << i << "](y=" << sprY << " h=" << sprH
                                  << " x=" << sprX << " w=" << sprW << ")";
                            win_count++;
                        }
                    }
                }
            }
            {
                std::ostringstream s;
                s << "[PPU-DBG] OAM scan: disabled=" << disabled
                  << " normal=" << normal << " semi=" << semi
                  << " objwin=" << objwin << " affine=" << affine_cnt;
                Logger::log(s.str(), LogLevel::DEBUG);
            }
            if (objwin > 0) Logger::log(win_s.str(), LogLevel::DEBUG);
            else Logger::log("[PPU-DBG] OAM scan: NO mode-2 (OBJ Window) sprites found!", LogLevel::DEBUG);
        }
        // 11. WININ / WINOUT raw registers (GBATEK §Window Feature)
        {
            uint16_t winin = ioReg16(target.io, 0x048);
            uint16_t winout = ioReg16(target.io, 0x04A);
            std::ostringstream s;
            s << "[PPU-DBG] WININ=0x" << std::hex << std::setw(4) << std::setfill('0') << winin
              << " WINOUT=0x" << std::setw(4) << std::setfill('0') << winout
              << " (outside=0x" << std::setw(2) << (winout & 0x3F)
              << " objwin=0x" << std::setw(2) << ((winout >> 8) & 0x3F) << ")";
            Logger::log(s.str(), LogLevel::DEBUG);
        }
        // 12. VRAM region scanner — find where non-zero data actually lives
        {
            const uint8_t* v = target.vram;
            // Scan BG tile area (0x0000-0xFFFF)
            std::ostringstream bg_s;
            bg_s << "[PPU-DBG] VRAM non-zero regions (BG tile 0x0000-0xFFFF):";
            int rcount = 0;
            bool in_r = false;
            uint32_t rstart = 0;
            for (uint32_t i = 0; i <= 0x10000 && rcount < 12; i++) {
                bool nz = (i < 0x10000) && (v[i] != 0);
                if (nz && !in_r) { rstart = i; in_r = true; }
                else if (!nz && in_r) {
                    bg_s << " [0x" << std::hex << rstart << "-0x" << (i-1) << " " << std::dec << (i-rstart) << "B]";
                    in_r = false; rcount++;
                }
            }
            Logger::log(bg_s.str(), LogLevel::DEBUG);

            // Scan OBJ tile area (0x10000-0x17FFF)
            std::ostringstream obj_s;
            obj_s << "[PPU-DBG] VRAM non-zero regions (OBJ tile 0x10000-0x17FFF):";
            rcount = 0; in_r = false;
            for (uint32_t i = 0x10000; i <= 0x18000 && rcount < 12; i++) {
                bool nz = (i < 0x18000) && (v[i] != 0);
                if (nz && !in_r) { rstart = i; in_r = true; }
                else if (!nz && in_r) {
                    obj_s << " [0x" << std::hex << rstart << "-0x" << (i-1) << " " << std::dec << (i-rstart) << "B]";
                    in_r = false; rcount++;
                }
            }
            Logger::log(obj_s.str(), LogLevel::DEBUG);

            // Hex dump scrBase map first 64 bytes
            uint16_t bg3cnt = ioReg16(target.io, 0x00E);
            uint32_t scrBase = ((bg3cnt >> 8) & 0x1F) * 0x800;
            {
                std::ostringstream m;
                m << "[PPU-DBG] scrBase map hex (vram+0x" << std::hex << scrBase << "):";
                for (int j = 0; j < 64; j++) {
                    if (j % 16 == 0) m << "\n  +" << std::hex << std::setw(3) << std::setfill('0') << j << ": ";
                    m << std::hex << std::setw(2) << std::setfill('0') << (unsigned)v[scrBase + j] << " ";
                }
                Logger::log(m.str(), LogLevel::DEBUG);
            }

            // Find first non-zero tile and dump it
            uint32_t charBase = ((bg3cnt >> 2) & 3) * 0x4000;
            for (int t = 0; t < 256; t++) {
                bool has_data = false;
                for (int b = 0; b < 64; b++) {
                    if (v[charBase + t * 64 + b] != 0) { has_data = true; break; }
                }
                if (has_data) {
                    std::ostringstream ts;
                    ts << "[PPU-DBG] First non-zero tile #" << t << " @charBase+0x" << std::hex << (t * 64) << ":";
                    for (int b = 0; b < 64; b++) {
                        if (b % 16 == 0) ts << "\n  ";
                        ts << std::hex << std::setw(2) << std::setfill('0') << (unsigned)v[charBase + t * 64 + b] << " ";
                    }
                    Logger::log(ts.str(), LogLevel::DEBUG);
                    break;
                }
            }

            // Dump all non-trivial sprites (non-zero attrs that are not disabled)
            {
                std::ostringstream spr;
                spr << "[PPU-DBG] Active sprites covering Y=80 or Y=110:";
                int hit_count = 0;
                for (int i = 0; i < 128 && hit_count < 20; i++) {
                    uint32_t base = i * 8;
                    uint16_t a0 = target.oam[base] | (target.oam[base+1] << 8);
                    uint16_t a1 = target.oam[base+2] | (target.oam[base+3] << 8);
                    uint16_t a2 = target.oam[base+4] | (target.oam[base+5] << 8);
                    bool rs = (a0 >> 8) & 1;
                    bool b9 = (a0 >> 9) & 1;
                    if (!rs && b9) continue; // disabled
                    int mode = (a0 >> 10) & 3;
                    if (mode == 3) continue; // forbidden
                    int shape = (a0 >> 14) & 3;
                    int size = (a1 >> 14) & 3;
                    if (shape > 2) continue;
                    int sprY = a0 & 0xFF;
                    if (sprY >= 160) sprY -= 256;
                    int sprH = kSprSizeTable[shape][size][1];
                    int sprX = a1 & 0x1FF;
                    if (sprX >= 240) sprX -= 512;
                    int tileNum = a2 & 0x3FF;
                    int pal = (a2 >> 12) & 0xF;
                    bool covers80 = (sprY <= 80 && sprY + sprH > 80);
                    bool covers110 = (sprY <= 110 && sprY + sprH > 110);
                    if (covers80 || covers110) {
                        spr << "\n  [" << i << "] y=" << sprY << " h=" << sprH
                            << " x=" << sprX << " tile=" << tileNum << " pal=" << pal
                            << " mode=" << mode << (covers80 ? " @80" : "") << (covers110 ? " @110" : "");
                        hit_count++;
                    }
                }
                if (hit_count == 0) spr << " (none)";
                Logger::log(spr.str(), LogLevel::DEBUG);
            }
        }
    }
    // End frame diagnostics.
#endif

    // Publish atomically, then flip so we write to the other buffer next frame.
    active_snapshot_.store(&target, std::memory_order_release);
    pending_idx_ ^= 1;

    // Swap the pixel framebuffers so display shows the completed back buffer.
    display_fb_idx_.store(render_fb_idx_, std::memory_order_release);
    render_fb_idx_ ^= 1;
}

void PPU::RenderScanline(ScanlineCtx& ctx, Framebuffer& fb, int line) {
    const FrameSnapshot* snap = ctx.snap;
    if (!snap) return;
    if (line < 0 || line >= kPpuScreenHeight) return;

    const uint8_t* io = snap->io;
    uint16_t dispcnt = ioReg16(io, PPUReg::kDispcnt);
    int bgMode = dispcnt & 0x7;
    bool forcedBlank = (dispcnt >> 7) & 1;

    if (forcedBlank) {
        std::memset(&fb.pixels[line][0], 0xFF, kPpuScreenWidth * 3);
        return;
    }

    bool bgEnabled[4] = {
        (dispcnt & (1 << 8))  != 0,
        (dispcnt & (1 << 9))  != 0,
        (dispcnt & (1 << 10)) != 0,
        (dispcnt & (1 << 11)) != 0,
    };
    bool objEnabled = (dispcnt & (1 << 12)) != 0;

    for (int bg = 0; bg < 4; bg++)
        std::memset(ctx.bgLine[bg], 0, sizeof(ctx.bgLine[bg]));
    std::memset(ctx.objLine, 0, sizeof(ctx.objLine));

    // GBATEK: OBJ Window uses sprite mode-2 pixels to define window regions.
    // Sprites must be rendered BEFORE the window mask is built so that
    // kObjWinBit is set in objLine[] for buildWindowMask() to check.
    if (objEnabled) renderSprites(ctx, line);
    buildWindowMask(ctx, line);

    switch (bgMode) {
        case 0:
            for (int bg = 0; bg < 4; bg++)
                if (bgEnabled[bg]) renderRegularBG(ctx, bg, line);
            break;
        case 1:
            if (bgEnabled[0]) renderRegularBG(ctx, 0, line);
            if (bgEnabled[1]) renderRegularBG(ctx, 1, line);
            if (bgEnabled[2]) renderAffineBG(ctx, 2, line);
            break;
        case 2:
            if (bgEnabled[2]) renderAffineBG(ctx, 2, line);
            if (bgEnabled[3]) renderAffineBG(ctx, 3, line);
            break;
        case 3:
            if (bgEnabled[2]) {
                const uint8_t* vram = snap->vram;
                for (int x = 0; x < kPpuScreenWidth; x++) {
                    int off = (line * kPpuScreenWidth + x) * 2;
                    uint16_t pixel = static_cast<uint16_t>(vram[off]) | (static_cast<uint16_t>(vram[off + 1]) << 8);
                    ctx.bgLine[2][x] = (pixel & 0x7FFF) | kPixelOpaqueBit | (0u << 16);
                }
            }
            break;
        case 4:
            if (bgEnabled[2]) {
                uint32_t base = ((dispcnt >> 4) & 1) ? 0xA000u : 0x0000u;
                const uint8_t* vram = snap->vram;
                const uint8_t* palette = snap->palette;
                for (int x = 0; x < kPpuScreenWidth; x++) {
                    uint8_t idx = vram[base + line * kPpuScreenWidth + x];
                    if (idx == 0) continue;
                    uint16_t colour = static_cast<uint16_t>(palette[idx * 2]) | (static_cast<uint16_t>(palette[idx * 2 + 1]) << 8);
                    ctx.bgLine[2][x] = (colour & 0x7FFF) | kPixelOpaqueBit | (0u << 16);
                }
            }
            break;
        case 5:
            if (bgEnabled[2] && line < 128) {
                constexpr int kMode5Width = 160;
                uint32_t base = ((dispcnt >> 4) & 1) ? 0xA000u : 0x0000u;
                const uint8_t* vram = snap->vram;
                for (int x = 0; x < kMode5Width; x++) {
                    int off = base + (line * kMode5Width + x) * 2;
                    uint16_t pixel = static_cast<uint16_t>(vram[off]) | (static_cast<uint16_t>(vram[off + 1]) << 8);
                    ctx.bgLine[2][x] = (pixel & 0x7FFF) | kPixelOpaqueBit | (0u << 16);
                }
            }
            break;
        default:
            break;
    }

    // Sprites already rendered above (before buildWindowMask).

#if PPU_DEBUG_LOGS
    // Per-scanline pixel counts for diagnostic frames.
    if (line == 80 || line == 110) {
        static int dbg_frame_ctr = 0;
        if (line == 80) dbg_frame_ctr++;
        // Reuse the line-80 frame counter for line-110 logging in the same frame.
        bool log_this_frame = (dbg_frame_ctr <= 8 || dbg_frame_ctr == 60 || dbg_frame_ctr == 115
            || dbg_frame_ctr == 116 || dbg_frame_ctr == 120 || dbg_frame_ctr == 180);
        if (log_this_frame) {
            int bgOpaque[4] = {}, objOpaque = 0, objSemiTrans = 0, objWinBits = 0;
            for (int x = 0; x < kPpuScreenWidth; x++) {
                for (int b = 0; b < 4; b++)
                    if (ctx.bgLine[b][x] & kPixelOpaqueBit) bgOpaque[b]++;
                if (ctx.objLine[x] & kPixelOpaqueBit) objOpaque++;
                if (ctx.objLine[x] & kSemiTransBit)   objSemiTrans++;
                if (ctx.objLine[x] & kObjWinBit)      objWinBits++;
            }
            uint16_t dbg_winout = ioReg16(snap->io, 0x04A);
            std::ostringstream s;
            s << "[PPU-DBG] Scanline " << line << " (render #" << dbg_frame_ctr << ") opaquePixels:"
              << " BG0=" << bgOpaque[0] << " BG1=" << bgOpaque[1]
              << " BG2=" << bgOpaque[2] << " BG3=" << bgOpaque[3]
              << " OBJ=" << objOpaque << " OBJ_semi=" << objSemiTrans
              << " OBJ_win=" << objWinBits
              << " mode=" << bgMode << " forcedBlank=" << forcedBlank
              << " WINOUT=0x" << std::hex << dbg_winout;
            Logger::log(s.str(), LogLevel::DEBUG);
            // Also dump first 8 window-mask values.
            std::ostringstream px;
            px << "[PPU-DBG] Scanline " << line << " window[0..7]:";
            for (int x = 0; x < 8; x++)
                px << " " << std::hex << static_cast<int>(ctx.windowMask[x]);
            Logger::log(px.str(), LogLevel::DEBUG);
        }
    }
    // End scanline diagnostics.
#endif

    compositeScanline(ctx, fb, line);
}

void PPU::RenderScanline(int line) {
    const FrameSnapshot* snap = active_snapshot_.load(std::memory_order_acquire);
    if (!snap) return;
    ScanlineCtx ctx;
    ctx.snap = snap;
    RenderScanline(ctx, framebuffers_[render_fb_idx_], line);
}

const Framebuffer* PPU::GetFramebuffer() const {
    return &framebuffers_[display_fb_idx_.load(std::memory_order_acquire)];
}
