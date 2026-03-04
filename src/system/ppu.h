//
// ppu.h — GBA Pixel Processing Unit (scanline renderer)
//
// Implements per-scanline rendering of tiled backgrounds (Modes 0–2),
// bitmap backgrounds (Modes 3–5), and sprites (OBJ layer).
// Uses FrameSnapshot (read at VBlank) + ScanlineCtx (per-line scratch) + Framebuffer (output).
// RenderScanline(ScanlineCtx&, Framebuffer&, int) is a pure function for thread-pool dispatch.
//

#pragma once

#include "data/memory_map.h"
#include "data/ppu_state.h"

#include <atomic>

class MemoryBus;

class PPU {
public:
    static constexpr int kScreenWidth  = kPpuScreenWidth;
    static constexpr int kScreenHeight = kPpuScreenHeight;

    PPU();

    /** Snapshot bus and precompute affine latches; call once per frame at VBlank. */
    void OnVBlank(const MemoryBus* bus);

    /** Render a single visible scanline (0–159) into the internal framebuffer. */
    void RenderScanline(int line);

    /** Pointer to the completed RGB888 framebuffer (240×160×3 bytes). */
    const Framebuffer* GetFramebuffer() const;

    /** Free-function entry point: pure function for parallel scanline dispatch. */
    static void RenderScanline(ScanlineCtx& ctx, Framebuffer& fb, int line);

private:
    FrameSnapshot snapshots_[2];
    int pending_idx_{0};
    int frame_count_{0};
    std::atomic<const FrameSnapshot*> active_snapshot_{nullptr};

    Framebuffer framebuffers_[2];
    int render_fb_idx_ = 0;
    std::atomic<int> display_fb_idx_{1};
};
