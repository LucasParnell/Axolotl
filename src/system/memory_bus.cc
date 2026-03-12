#include "memory_bus.h"
#include "data/cpu_state.h"
#include "data/memory_map.h"
#include "util/logger.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#ifndef VRAM_DEBUG_LOGS
#define VRAM_DEBUG_LOGS 0
#endif

#ifndef LOGO_TILE_WATCH
#define LOGO_TILE_WATCH 0
#endif

#ifndef LOGO_SCRATCH_WATCH
#define LOGO_SCRATCH_WATCH 0
#endif

#ifndef LOGO_SCRATCH_RANGE_WATCH
#define LOGO_SCRATCH_RANGE_WATCH 0
#endif

#ifndef BIOS_PROTECT_TRACE
#define BIOS_PROTECT_TRACE 0
#endif

#ifndef LOGO_LINK_WATCH
#define LOGO_LINK_WATCH 0
#endif

#ifndef LOGO_LINK_WATCH_PAUSE
#define LOGO_LINK_WATCH_PAUSE 0
#endif

#ifdef B_DEBUG
#include "util/debug_tools.h"
#include <atomic>
// Set from main() after both dispatcher and bus are constructed.
static WatchpointSet*    g_watchpoints    = nullptr;
static std::atomic<bool>* g_pause_flag   = nullptr;

void MemoryBus_SetDebugState(WatchpointSet* wp, std::atomic<bool>* flag) {
    g_watchpoints = wp;
    g_pause_flag  = flag;
}
#endif

#ifdef B_DEBUG
#ifndef IRQ_WAITWORD_WATCH
#define IRQ_WAITWORD_WATCH 0
#endif
#ifndef FLASH_READ_WATCH
#define FLASH_READ_WATCH 1
#endif
#endif

// Track how often software writes to graphics memory regions.
static std::atomic<uint32_t> g_vram_write_count{0};
static std::atomic<uint32_t> g_pal_write_count{0};
static std::atomic<uint32_t> g_oam_write_count{0};

uint32_t DebugGetVramWriteCount()  { return g_vram_write_count.load(std::memory_order_relaxed); }
uint32_t DebugGetPalWriteCount()   { return g_pal_write_count.load(std::memory_order_relaxed); }
uint32_t DebugGetOamWriteCount()   { return g_oam_write_count.load(std::memory_order_relaxed); }
void     DebugResetGfxWriteCounts() {
    g_vram_write_count.store(0, std::memory_order_relaxed);
    g_pal_write_count.store(0, std::memory_order_relaxed);
    g_oam_write_count.store(0, std::memory_order_relaxed);
}
// End graphics write counters.

namespace {

const char* BackupTypeName(MemoryBus::BackupType type) {
    switch (type) {
        case MemoryBus::BackupType::kSram: return "SRAM";
        case MemoryBus::BackupType::kFlash: return "FLASH";
        case MemoryBus::BackupType::kEeprom: return "EEPROM";
        default: return "NONE";
    }
}

MemoryBus::BackupType DetectBackupTypeFromRom(const std::vector<uint8_t>& rom) {
    if (rom.empty()) return MemoryBus::BackupType::kNone;
    const std::string haystack(reinterpret_cast<const char*>(rom.data()), rom.size());

    // Most commercial GBA ROMs embed one of these ASCII IDs.
    if (haystack.find("EEPROM_V") != std::string::npos) return MemoryBus::BackupType::kEeprom;
    if (haystack.find("FLASH1M_V") != std::string::npos) return MemoryBus::BackupType::kFlash;
    if (haystack.find("FLASH512_V") != std::string::npos) return MemoryBus::BackupType::kFlash;
    if (haystack.find("FLASH_V") != std::string::npos) return MemoryBus::BackupType::kFlash;
    if (haystack.find("SRAM_V") != std::string::npos) return MemoryBus::BackupType::kSram;
    return MemoryBus::BackupType::kNone;
}

size_t DetectFlashStorageSizeFromRom(const std::vector<uint8_t>& rom) {
    if (rom.empty()) return 64 * 1024;
    const std::string haystack(reinterpret_cast<const char*>(rom.data()), rom.size());
    if (haystack.find("FLASH1M_V") != std::string::npos) return 128 * 1024;
    return 64 * 1024;
}

#ifdef B_DEBUG
#if FLASH_READ_WATCH
inline void MaybeLogFlashRead(const char* op,
                              uint32_t addr,
                              uint32_t value,
                              uint32_t pc,
                              MemoryBus::BackupType backup_type) {
    static int flash_read_log_count = 0;
    if (++flash_read_log_count > 1024) return;

    std::ostringstream msg;
    msg << "[FLASH-READ] " << op
        << " @0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << addr
        << " -> 0x";
    if (std::strcmp(op, "R8 ") == 0) {
        msg << std::setw(2) << (value & 0xFFu);
    } else if (std::strcmp(op, "R16") == 0) {
        msg << std::setw(4) << (value & 0xFFFFu);
    } else {
        msg << std::setw(8) << value;
    }
    msg << " PC=0x" << std::setw(8) << pc
        << " backup=" << BackupTypeName(backup_type);
    Logger::log(msg.str(), LogLevel::WARNING);
}
#endif

struct LogoFirstNzState {
    bool chunk_a = false;   // 0x06002440..+0xA8
    bool chunk_b = false;   // 0x06002840..+0xA8
    bool chunk_c = false;   // 0x06002C40..+0xA8
    bool obj_tiles = false; // 0x06017400..+0x200
};
static LogoFirstNzState g_logo_first_nz;

[[maybe_unused]] inline void MaybeLogLogoFirstNonZero(uint32_t aligned_addr, uint32_t value, uint32_t pc) {
    if (value == 0) return;
    uint32_t vram_off = aligned_addr & 0x1FFFF;
    if (vram_off >= 0x18000) vram_off -= 0x8000;

    auto log_hit = [&](const char* region) {
        std::printf("[LOGO-FIRSTNZ] %s @vram+0x%04X = 0x%08X (GBA 0x%08X) PC=0x%08X\n",
                    region, vram_off, value, aligned_addr, pc);
    };

    if (!g_logo_first_nz.chunk_a && vram_off >= 0x2440 && vram_off < 0x2440 + 0xA8) {
        g_logo_first_nz.chunk_a = true;
        log_hit("chunk_a");
    } else if (!g_logo_first_nz.chunk_b && vram_off >= 0x2840 && vram_off < 0x2840 + 0xA8) {
        g_logo_first_nz.chunk_b = true;
        log_hit("chunk_b");
    } else if (!g_logo_first_nz.chunk_c && vram_off >= 0x2C40 && vram_off < 0x2C40 + 0xA8) {
        g_logo_first_nz.chunk_c = true;
        log_hit("chunk_c");
    } else if (!g_logo_first_nz.obj_tiles && vram_off >= 0x17400 && vram_off < 0x17400 + 0x200) {
        g_logo_first_nz.obj_tiles = true;
        log_hit("obj_tiles");
    }
}
#endif

// Hidden internal hardware latches for DMA and BIOS Open Bus
struct DmaLatch {
    uint32_t sad;
    uint32_t dad;
    uint32_t count;
} g_dma_latch[4];

uint32_t g_bios_latch = 0;

// Human-readable labels for GBA I/O (0x04000000 + off). Returns nullptr for unknown.
[[maybe_unused]] const char* IoLabel(uint32_t off) {
    switch (off) {
        case 0x000: return "DISPCNT";
        case 0x004: return "DISPSTAT";
        case 0x006: return "VCOUNT";
        case 0x008: return "BG0CNT";
        case 0x00A: return "BG1CNT";
        case 0x00C: return "BG2CNT";
        case 0x00E: return "BG3CNT";
        case 0x010: return "BG0HOFS";
        case 0x012: return "BG0VOFS";
        case 0x014: return "BG1HOFS";
        case 0x016: return "BG1VOFS";
        case 0x018: return "BG2HOFS";
        case 0x01A: return "BG2VOFS";
        case 0x01C: return "BG3HOFS";
        case 0x01E: return "BG3VOFS";
        case 0x020: return "BG2PA";
        case 0x022: return "BG2PB";
        case 0x024: return "BG2PC";
        case 0x026: return "BG2PD";
        case 0x028: return "BG2X_L";
        case 0x02A: return "BG2X_H";
        case 0x02C: return "BG2Y_L";
        case 0x02E: return "BG2Y_H";
        case 0x030: return "BG3PA";
        case 0x032: return "BG3PB";
        case 0x034: return "BG3PC";
        case 0x036: return "BG3PD";
        case 0x038: return "BG3X_L";
        case 0x03A: return "BG3X_H";
        case 0x03C: return "BG3Y_L";
        case 0x03E: return "BG3Y_H";
        case 0x040: return "WIN0H";
        case 0x042: return "WIN1H";
        case 0x044: return "WIN0V";
        case 0x046: return "WIN1V";
        case 0x048: return "WININ";
        case 0x04A: return "WINOUT";
        case 0x050: return "BLDCNT";
        case 0x052: return "BLDALPHA";
        case 0x054: return "BLDY";
        case 0x0B0: return "DMA0SAD_L";
        case 0x0B2: return "DMA0SAD_H";
        case 0x0B4: return "DMA0DAD_L";
        case 0x0B6: return "DMA0DAD_H";
        case 0x0B8: return "DMA0CNT_L";
        case 0x0BA: return "DMA0CNT_H";
        case 0x0BC: return "DMA1SAD_L";
        case 0x0BE: return "DMA1SAD_H";
        case 0x0C0: return "DMA1DAD_L";
        case 0x0C2: return "DMA1DAD_H";
        case 0x0C4: return "DMA1CNT_L";
        case 0x0C6: return "DMA1CNT_H";
        case 0x0C8: return "DMA2SAD_L";
        case 0x0CA: return "DMA2SAD_H";
        case 0x0CC: return "DMA2DAD_L";
        case 0x0CE: return "DMA2DAD_H";
        case 0x0D0: return "DMA2CNT_L";
        case 0x0D2: return "DMA2CNT_H";
        case 0x0D4: return "DMA3SAD_L";
        case 0x0D6: return "DMA3SAD_H";
        case 0x0D8: return "DMA3DAD_L";
        case 0x0DA: return "DMA3DAD_H";
        case 0x0DC: return "DMA3CNT_L";
        case 0x0DE: return "DMA3CNT_H";
        case 0x100: return "TM0CNT_L";
        case 0x102: return "TM0CNT_H";
        case 0x104: return "TM1CNT_L";
        case 0x106: return "TM1CNT_H";
        case 0x108: return "TM2CNT_L";
        case 0x10A: return "TM2CNT_H";
        case 0x10C: return "TM3CNT_L";
        case 0x10E: return "TM3CNT_H";
        case 0x200: return "IE";
        case 0x202: return "IF";
        case 0x204: return "WAITCNT";
        case 0x208: return "IME";
        case 0x300: return "HALTCNT_L";
        case 0x301: return "HALTCNT";  
        case 0x302: return "HALTCNT_H";
        default:   return nullptr;
    }
}

#ifdef B_DEBUG
#if LOGO_LINK_WATCH
const char* LogoLinkWatchLabel(uint32_t addr) {
    switch (addr) {
        case 0x03003BB0u: return "obj_next_0";
        case 0x03003BF0u: return "obj_next_1";
        case 0x03003C30u: return "obj_next_2";
        case 0x03003C70u: return "obj_next_3";
        case 0x03003CB0u: return "obj_next_4";
        case 0x03003CF0u: return "obj_next_5";
        case 0x0300374Cu: return "node_link_0";
        case 0x0300379Cu: return "node_link_1";
        case 0x030037ECu: return "node_link_2";
        case 0x0300383Cu: return "node_link_3";
        case 0x0300388Cu: return "node_link_4";
        case 0x030038DCu: return "node_link_5";
        default: return nullptr;
    }
}

void MaybeLogLogoLinkWatchRW(const char* op, uint32_t addr, uint32_t value, uint32_t pc) {
    const char* label = LogoLinkWatchLabel(addr);
    if (!label) return;
    static int log_count = 0;
    if (++log_count <= 4096) {
        std::printf("[LOGO-LINK] %s %-11s @0x%08X = 0x%08X PC=0x%08X\n",
                    op, label, addr, value, pc);
    }
#if LOGO_LINK_WATCH_PAUSE
    if (g_pause_flag) g_pause_flag->store(true, std::memory_order_release);
#endif
}
#endif
#endif

#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
inline void MaybeLogIrqWaitWordWrite(const char* op,
                                     uint32_t addr,
                                     uint32_t old_val,
                                     uint32_t new_val,
                                     uint32_t pc,
                                     uint16_t ie,
                                     uint16_t if_reg,
                                     uint16_t ime) {
    // BIOS/game wait word for IntrWait/VBlankIntrWait in this title.
    if (addr != 0x0300310Cu) return;
    static int waitword_log_count = 0;
    if (++waitword_log_count > 512) return;
    std::printf("[IRQ-WAIT] %s @0x%08X old=0x%04X new=0x%04X PC=0x%08X IE=0x%04X IF=0x%04X IME=0x%04X\n",
                op, addr,
                static_cast<unsigned>(old_val & 0xFFFFu),
                static_cast<unsigned>(new_val & 0xFFFFu),
                pc, ie, if_reg, ime);
}
#endif
#endif

}  // namespace

constexpr uint32_t kBiosSize    = 16 * 1024;   
constexpr uint32_t kEwramSize   = 256 * 1024;  
constexpr uint32_t kIwramSize   = 32 * 1024;   
constexpr uint32_t kPaletteSize = 1 * 1024;    
constexpr uint32_t kVramSize    = 96 * 1024;   
constexpr uint32_t kOamSize     = 1 * 1024;    
constexpr uint32_t kRomSize     = 32 * 1024 * 1024; 
constexpr uint32_t kSramSize    = 64 * 1024;   

MemoryBus::MemoryBus()
    : page_table_(std::make_unique<void*[]>(262144)),
      page_mask_(std::make_unique<uint32_t[]>(262144))
{
    std::fill_n(page_mask_.get(), 262144, kPageOffsetMask);
    bios_.resize(kBiosSize, 0);
    ewram_.resize(kEwramSize, 0);
    iwram_.resize(kIwramSize, 0);
    palette_.resize(kPaletteSize, 0);
    vram_.resize(kVramSize, 0);
    oam_.resize(kOamSize, 0);
    rom_.reserve(kRomSize);          // reserve address space; no page faults
    sram_.resize(kSramSize, 0xFF);

    MapRegion(0x02000000, 0x02FFFFFF, ewram_.size(), ewram_.data());
    MapRegion(0x03000000, 0x03FFFFFF, kIwramSize, iwram_.data());
    MapRegion(0x05000000, 0x05FFFFFF, kPaletteSize, palette_.data());

    // Map 96 KB VRAM with the hardware 32 KB mirror window.
    for (uint32_t base = 0x06000000; base < 0x07000000; base += 0x20000) {
        MapRegion(base, base + 0x17FFF, 0x18000, vram_.data());
        MapRegion(base + 0x18000, base + 0x1FFFF, 0x8000, vram_.data() + 0x10000);
    }

    MapRegion(0x07000000, 0x07FFFFFF, kOamSize, oam_.data());
    // ROM region (0x08-0x0D) is NOT mapped here — mapped by LoadRom().
    // Unmapped Game Pak reads return open-bus: (addr/2) & 0xFFFF (per GBATEK).
    MapRegion(0x0E000000, 0x0E00FFFF, kSramSize, sram_.data());
}

void MemoryBus::CopyToMemoryMap(MemoryMap* mm) const {
    if (mm->ioRegs) std::memcpy(mm->ioRegs, io_regs_.data(), io_regs_.size());
    if (mm->vRam) std::memcpy(mm->vRam, vram_.data(), vram_.size());
    if (mm->paletteRam) std::memcpy(mm->paletteRam, palette_.data(), palette_.size());
    if (mm->oam) std::memcpy(mm->oam, oam_.data(), oam_.size());
}

void MemoryBus::SetAudioSampleCallback(AudioBatchCallback callback, void* user) {
    audio_sample_callback_ = callback;
    audio_sample_callback_user_ = user;
}

void MemoryBus::RecomputeSoundControlCache() {
    const uint16_t soundcnt_h = static_cast<uint16_t>(io_regs_[0x082]) |
                                (static_cast<uint16_t>(io_regs_[0x083]) << 8);
    const uint16_t soundcnt_x = static_cast<uint16_t>(io_regs_[0x084]) |
                                (static_cast<uint16_t>(io_regs_[0x085]) << 8);

    snd_cache_.master_enabled = (soundcnt_x & 0x0080u) != 0;
    snd_cache_.a_timer1 = (soundcnt_h & (1u << 10)) != 0;
    snd_cache_.b_timer1 = (soundcnt_h & (1u << 14)) != 0;
    snd_cache_.a_gain = (soundcnt_h & (1u << 2)) ? 256 : 128;
    snd_cache_.b_gain = (soundcnt_h & (1u << 3)) ? 256 : 128;
    snd_cache_.a_r = (soundcnt_h & (1u << 8)) != 0;
    snd_cache_.a_l = (soundcnt_h & (1u << 9)) != 0;
    snd_cache_.b_r = (soundcnt_h & (1u << 12)) != 0;
    snd_cache_.b_l = (soundcnt_h & (1u << 13)) != 0;

    if (!snd_cache_.master_enabled) {
        ds_hold_left_ = 0;
        ds_hold_right_ = 0;
    }
}

void MemoryBus::ResetFifo(int channel) {
    if (channel < 0 || channel > 1) return;
    ds_fifo_[channel].read_idx = 0;
    ds_fifo_[channel].size = 0;
    ds_fifo_[channel].last_sample = 0;
}

void MemoryBus::PushFifoWord(int channel, uint32_t value) {
    if (channel < 0 || channel > 1) return;
    auto& fifo = ds_fifo_[channel];
    for (int i = 0; i < 4; ++i) {
        if (fifo.size >= fifo.data.size()) break;
        const uint8_t write_idx = static_cast<uint8_t>((fifo.read_idx + fifo.size) & 31u);
        fifo.data[write_idx] = static_cast<int8_t>((value >> (i * 8)) & 0xFFu);
        ++fifo.size;
    }
}

int8_t MemoryBus::PopFifoSample(int channel) {
    if (channel < 0 || channel > 1) return 0;
    auto& fifo = ds_fifo_[channel];
    int8_t sample = 0;
    if (fifo.size > 0) {
        sample = fifo.data[fifo.read_idx];
        fifo.read_idx = static_cast<uint8_t>((fifo.read_idx + 1) & 31u);
        --fifo.size;
        fifo.last_sample = sample;
    } else {
        fifo.last_sample = 0;
    }

    // Refill threshold: request DMA when FIFO is half-empty or below.
    if (fifo.size <= 16) {
        TriggerSoundDma(channel == 0 ? 0x040000A0u : 0x040000A4u);
    }
    return fifo.last_sample;
}

void MemoryBus::TriggerSoundDma(uint32_t fifo_addr) {
    for (int ch = 1; ch <= 2; ++ch) {
        const uint32_t cnt_off = 0x0BA + static_cast<uint32_t>(ch) * 12u;
        const uint16_t cnt_h = static_cast<uint16_t>(io_regs_[cnt_off]) |
                               (static_cast<uint16_t>(io_regs_[cnt_off + 1]) << 8);
        if ((cnt_h & 0x8000u) == 0) continue;
        const uint32_t timing = (cnt_h >> 12) & 3u;
        if (timing != 3u) continue;
        if (g_dma_latch[ch].dad != fifo_addr) continue;
        ExecuteDmaTransfer(ch);
    }
}

void MemoryBus::OnTimerOverflow(int timer_ch, uint32_t overflow_count) {
    if (overflow_count == 0) return;

    auto sat16 = [](int32_t x) -> int16_t {
        if (x > 32767) return 32767;
        if (x < -32768) return -32768;
        return static_cast<int16_t>(x);
    };

    if (!snd_cache_.master_enabled) {  // Master sound off.
        ds_hold_left_ = 0;
        ds_hold_right_ = 0;
        return;
    }

    const bool a_on_this_timer = ((snd_cache_.a_timer1 ? 1 : 0) == timer_ch);
    const bool b_on_this_timer = ((snd_cache_.b_timer1 ? 1 : 0) == timer_ch);
    if (!a_on_this_timer && !b_on_this_timer) return;

    for (uint32_t i = 0; i < overflow_count; ++i) {
        int32_t left = 0;
        int32_t right = 0;

        if (a_on_this_timer) {
            const int32_t s = static_cast<int32_t>(PopFifoSample(0)) * snd_cache_.a_gain;
            if (snd_cache_.a_l) left += s;
            if (snd_cache_.a_r) right += s;
        }
        if (b_on_this_timer) {
            const int32_t s = static_cast<int32_t>(PopFifoSample(1)) * snd_cache_.b_gain;
            if (snd_cache_.b_l) left += s;
            if (snd_cache_.b_r) right += s;
        }

        ds_hold_left_ = sat16(left);
        ds_hold_right_ = sat16(right);
    }
}

void MemoryBus::EmitAudioForCycles(uint32_t cpu_cycles) {
    if (!audio_sample_callback_ || cpu_cycles == 0) return;

    audio_resample_accum_ += static_cast<uint64_t>(cpu_cycles) * kAudioHostRate;
    constexpr size_t kBatchFrames = 512;
    int16_t batch[kBatchFrames * 2];
    size_t batch_frames = 0;

    while (audio_resample_accum_ >= kAudioCpuHz) {
        audio_resample_accum_ -= kAudioCpuHz;
        batch[batch_frames * 2] = ds_hold_left_;
        batch[batch_frames * 2 + 1] = ds_hold_right_;
        ++batch_frames;
        if (batch_frames == kBatchFrames) {
            audio_sample_callback_(audio_sample_callback_user_, batch, batch_frames);
            batch_frames = 0;
        }
    }

    if (batch_frames > 0) {
        audio_sample_callback_(audio_sample_callback_user_, batch, batch_frames);
    }
}

void MemoryBus::RaiseVBlank() {
    io_IF_.fetch_or(1, std::memory_order_relaxed);
    irq_vblank_raised_.fetch_add(1, std::memory_order_relaxed);
}

void MemoryBus::RaiseHBlankIRQ() {
    io_IF_.fetch_or(2, std::memory_order_relaxed);
    irq_hblank_raised_.fetch_add(1, std::memory_order_relaxed);
#ifdef B_DEBUG
    uint16_t if_ = io_IF_.load(std::memory_order_relaxed);
    std::ostringstream msg;
    msg << "[IRQ] raise HBlank -> IF=0x" << std::hex << if_;
    Logger::log(msg.str(), LogLevel::DEBUG);
#endif
}

void MemoryBus::RaiseVCountIRQ() {
    io_IF_.fetch_or(4, std::memory_order_relaxed);
    irq_vcount_raised_.fetch_add(1, std::memory_order_relaxed);
#ifdef B_DEBUG
    uint16_t if_ = io_IF_.load(std::memory_order_relaxed);
    std::ostringstream msg;
    msg << "[IRQ] raise VCount -> IF=0x" << std::hex << if_;
    Logger::log(msg.str(), LogLevel::DEBUG);
#endif
}

MemoryBus::IrqDebugSnapshot MemoryBus::GetIrqDebugSnapshot() const {
    IrqDebugSnapshot s;
    s.vblank_raised = irq_vblank_raised_.load(std::memory_order_relaxed);
    s.hblank_raised = irq_hblank_raised_.load(std::memory_order_relaxed);
    s.vcount_raised = irq_vcount_raised_.load(std::memory_order_relaxed);
    s.if_clears = irq_if_clears_.load(std::memory_order_relaxed);
    return s;
}

void MemoryBus::ResetIrqDebugCounters() {
    irq_vblank_raised_.store(0, std::memory_order_relaxed);
    irq_hblank_raised_.store(0, std::memory_order_relaxed);
    irq_vcount_raised_.store(0, std::memory_order_relaxed);
    irq_if_clears_.store(0, std::memory_order_relaxed);
}

// DMA timing: 0=Immediate, 1=VBlank, 2=HBlank, 3=Special.
// Repeat-mode reload is handled in ExecuteDmaTransfer.
void MemoryBus::TriggerVBlankDma() {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt_off = 0x0BA + ch * 12;
        uint16_t cnt_h = io_regs_[cnt_off] | (io_regs_[cnt_off + 1] << 8);
        if (!(cnt_h & 0x8000)) continue;            // not enabled
        uint32_t timing = (cnt_h >> 12) & 3;
        if (timing != 1) continue;                   // not VBlank timing
        ExecuteDmaTransfer(ch);
    }
}

void MemoryBus::TriggerHBlankDma() {
    for (int ch = 0; ch < 4; ch++) {
        uint32_t cnt_off = 0x0BA + ch * 12;
        uint16_t cnt_h = io_regs_[cnt_off] | (io_regs_[cnt_off + 1] << 8);
        if (!(cnt_h & 0x8000)) continue;            // not enabled
        uint32_t timing = (cnt_h >> 12) & 3;
        if (timing != 2) continue;                   // not HBlank timing
        ExecuteDmaTransfer(ch);
    }
}

void MemoryBus::StepTimers(uint32_t cpu_cycles) {
    if (cpu_cycles == 0) return;

    static constexpr uint32_t kPrescalerCycles[4] = {1u, 64u, 256u, 1024u};
    // Audio timing is driven by timer overflows. Build two arithmetic streams
    // (timer0 and timer1) and merge them in-order without allocation/sort.
    struct AudioOverflowStream {
        bool active = false;
        uint32_t next_cycle = 0;
        uint32_t step_cycle = 0;
        int timer_ch = 0;
    };
    AudioOverflowStream stream[2];
    for (int ch = 0; ch <= 1; ++ch) {
        const uint32_t low_off = 0x100 + ch * 4;
        const uint32_t high_off = low_off + 2;
        const uint16_t cnt_h = static_cast<uint16_t>(io_regs_[high_off]) |
                               (static_cast<uint16_t>(io_regs_[high_off + 1]) << 8);
        const bool enabled = (cnt_h & 0x80u) != 0;
        const bool count_up = (cnt_h & 0x04u) != 0;
        if (!enabled || count_up) continue;

        const uint32_t prescale = kPrescalerCycles[cnt_h & 0x3u];
        const uint32_t phase = timer_subcycles_[ch];
        const uint64_t total = static_cast<uint64_t>(phase) + cpu_cycles;
        const uint64_t ticks = total / prescale;
        if (ticks == 0) continue;

        const uint16_t counter = static_cast<uint16_t>(io_regs_[low_off]) |
                                 (static_cast<uint16_t>(io_regs_[low_off + 1]) << 8);
        const uint64_t ticks_to_overflow = 0x10000ull - static_cast<uint64_t>(counter);
        if (ticks < ticks_to_overflow) continue;

        uint64_t period_ticks = 0x10000ull - static_cast<uint64_t>(timer_reload_[ch]);
        if (period_ticks == 0) period_ticks = 0x10000ull;

        const uint64_t first_abs = ticks_to_overflow * prescale;
        if (first_abs <= phase) continue;
        const uint64_t first_rel = first_abs - phase;
        if (first_rel > cpu_cycles) continue;

        stream[ch].active = true;
        stream[ch].next_cycle = static_cast<uint32_t>(first_rel);
        stream[ch].step_cycle = static_cast<uint32_t>(period_ticks * prescale);
        stream[ch].timer_ch = ch;
    }

    uint32_t cursor = 0;
    while (stream[0].active || stream[1].active) {
        uint32_t at = cpu_cycles + 1;
        if (stream[0].active && stream[0].next_cycle < at) at = stream[0].next_cycle;
        if (stream[1].active && stream[1].next_cycle < at) at = stream[1].next_cycle;
        if (at > cpu_cycles) break;

        if (at > cursor) {
            EmitAudioForCycles(at - cursor);
            cursor = at;
        }

        for (int idx = 0; idx < 2; ++idx) {
            if (!stream[idx].active || stream[idx].next_cycle != at) continue;
            OnTimerOverflow(stream[idx].timer_ch, 1);
            const uint64_t next = static_cast<uint64_t>(stream[idx].next_cycle) +
                                  static_cast<uint64_t>(stream[idx].step_cycle);
            if (next > cpu_cycles || stream[idx].step_cycle == 0) {
                stream[idx].active = false;
            } else {
                stream[idx].next_cycle = static_cast<uint32_t>(next);
            }
        }
    }
    if (cpu_cycles > cursor) {
        EmitAudioForCycles(cpu_cycles - cursor);
    }

    uint32_t overflow_count[4] = {0, 0, 0, 0};

    for (int ch = 0; ch < 4; ++ch) {
        const uint32_t low_off = 0x100 + ch * 4;
        const uint32_t high_off = low_off + 2;
        const uint16_t cnt_h = static_cast<uint16_t>(io_regs_[high_off]) |
                               (static_cast<uint16_t>(io_regs_[high_off + 1]) << 8);
        const bool enabled = (cnt_h & 0x80u) != 0;
        if (!enabled) {
            timer_subcycles_[ch] = 0;
            continue;
        }

        uint32_t ticks = 0;
        const bool count_up = (cnt_h & 0x04u) != 0;
        if (count_up && ch > 0) {
            ticks = overflow_count[ch - 1];
        } else {
            const uint32_t prescale = kPrescalerCycles[cnt_h & 0x3u];
            timer_subcycles_[ch] += cpu_cycles;
            ticks = timer_subcycles_[ch] / prescale;
            timer_subcycles_[ch] %= prescale;
        }
        if (ticks == 0) continue;

        uint16_t counter = static_cast<uint16_t>(io_regs_[low_off]) |
                           (static_cast<uint16_t>(io_regs_[low_off + 1]) << 8);
        uint32_t overflows = 0;
        while (ticks-- > 0) {
            ++counter;
            if (counter == 0) {
                ++overflows;
                counter = timer_reload_[ch];
            }
        }

        io_regs_[low_off] = static_cast<uint8_t>(counter & 0xFF);
        io_regs_[low_off + 1] = static_cast<uint8_t>(counter >> 8);
        overflow_count[ch] = overflows;
        if (overflows > 0 && (ch > 1 || count_up)) {
            OnTimerOverflow(ch, overflows);
        }

        if (overflows > 0 && (cnt_h & 0x40u)) {
            io_IF_.fetch_or(static_cast<uint16_t>(1u << (3 + ch)), std::memory_order_relaxed);
        }
    }
}

void MemoryBus::MapRegion(uint32_t virtual_start, uint32_t virtual_end,
                          uint32_t physical_size, uint8_t* host_ptr) {
    uint32_t start_page = virtual_start >> 14;
    uint32_t end_page = virtual_end >> 14;

    const uint32_t kPageSize = 1u << 14;
    uint32_t mask = (physical_size < kPageSize) ? (physical_size - 1u) : kPageOffsetMask;

    for (uint32_t i = start_page; i <= end_page; ++i) {
        uint32_t page_virtual_addr = i << 14;
        uint32_t mirror_offset = (page_virtual_addr - virtual_start) % physical_size;
        uint8_t* offset_ptr = (host_ptr + mirror_offset) - page_virtual_addr;
        page_table_[i] = static_cast<void*>(offset_ptr);
        page_mask_[i] = mask;
    }
}

uint32_t MemoryBus::GetOpenBus(uint32_t current_pc) const {
    // BIOS open-bus protection.
    if (current_pc >= 0x4000 && current_pc < 0x0E000000 && (current_pc >> 24) == 0x00) {
        return g_bios_latch;
    }

    const bool thumb = (current_pc & 1) != 0;
    const uint32_t prefetch_addr = thumb ? ((current_pc & ~1u) + 4) : ((current_pc & ~3u) + 8);
    const uint32_t page = prefetch_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);
    
    if (host_ptr == nullptr) return 0;
    
    if (thumb) {
        const uint32_t eff = MaskedAddr(page, prefetch_addr);
        const uint16_t val = *reinterpret_cast<uint16_t*>(host_ptr + eff);
        return (static_cast<uint32_t>(val) << 16) | val;
    }
    const uint32_t eff = MaskedAddr(page, prefetch_addr);
    return *reinterpret_cast<uint32_t*>(host_ptr + eff);
}

void MemoryBus::ExecuteDmaTransfer(int ch) {
    uint32_t sad_off = 0x0B0 + (ch * 12);
    uint16_t cnt_h = io_regs_[sad_off + 10] | (io_regs_[sad_off + 11] << 8);

    // Read from the channel latch state.
    uint32_t src = g_dma_latch[ch].sad;
    uint32_t dst = g_dma_latch[ch].dad;
    uint32_t count = g_dma_latch[ch].count;

    bool is_32bit = (cnt_h >> 10) & 1;
    uint32_t src_adj = (cnt_h >> 7) & 3;
    uint32_t dst_adj = (cnt_h >> 5) & 3;
    uint32_t timing = (cnt_h >> 12) & 3;
    bool repeat = (cnt_h >> 9) & 1;

    // Sound FIFO mode uses fixed 4x32-bit transfers.
    if ((ch == 1 || ch == 2) && timing == 3) {
        count = 4;
        is_32bit = true;
        dst_adj = 2; // Fixed destination
    }

    // Always log DMA transfers that target graphics memory.
    {
        uint32_t dst_region = dst >> 24;
        bool targets_gfx = (dst_region == 0x05 || dst_region == 0x06 || dst_region == 0x07);
        const char* timing_names[] = {"Immediate", "VBlank", "HBlank", "Special"};
        const char* adj_names[] = {"Inc", "Dec", "Fixed", "Inc/Reload"};
        if (targets_gfx) {
            uint32_t bytes = count * (is_32bit ? 4 : 2);
            const char* dst_name = (dst_region == 0x05) ? "PAL" :
                                   (dst_region == 0x06) ? "VRAM" : "OAM";
            std::ostringstream msg;
            msg << "[DMA" << ch << "] → " << dst_name << ": "
                << "src=0x" << std::hex << std::setfill('0') << std::setw(8) << src
                << " dst=0x" << std::setw(8) << dst
                << " count=" << std::dec << count
                << " (" << bytes << " bytes)"
                << (is_32bit ? " 32bit" : " 16bit")
                << " srcAdj=" << adj_names[src_adj]
                << " dstAdj=" << adj_names[dst_adj]
                << " timing=" << timing_names[timing];
            Logger::log(msg.str(), LogLevel::DEBUG);
        }
#ifdef B_DEBUG
        else {
            std::ostringstream msg;
            msg << "[DMA" << ch << "] transfer: "
                << "src=0x" << std::hex << std::setfill('0') << std::setw(8) << src
                << " dst=0x" << std::setw(8) << dst
                << " count=" << std::dec << count
                << (is_32bit ? " 32bit" : " 16bit")
                << " srcAdj=" << adj_names[src_adj]
                << " dstAdj=" << adj_names[dst_adj]
                << " timing=" << timing_names[timing]
                << (repeat ? " REPEAT" : "")
                << ((cnt_h & 0x4000) ? " IRQ" : "");
            Logger::log(msg.str(), LogLevel::DEBUG);
        }
#endif
    }

    int src_step = is_32bit ? 4 : 2;
    if (src_adj == 1) src_step = -src_step;
    else if (src_adj == 2) src_step = 0;

    int dst_step = is_32bit ? 4 : 2;
    if (dst_adj == 1) dst_step = -dst_step;
    else if (dst_adj == 2) dst_step = 0;

    bool handled_eeprom_dma = false;
    if (backup_type_ == BackupType::kEeprom && ch == 3) {
        handled_eeprom_dma = eeprom_backup_.HandleDma3Transfer(
            is_32bit, count, src_step, dst_step, src, dst,
            [this](uint32_t dma_addr) -> uint16_t {
                return this->Read16(dma_addr, 0);
            },
            [this](uint32_t dma_addr, uint16_t dma_value) {
                this->Write16(nullptr, dma_addr, dma_value);
            });
    }

    if (!handled_eeprom_dma) {
        for (uint32_t i = 0; i < count; i++) {
            if (is_32bit) Write32(nullptr, dst, Read32(src, 0));
            else Write16(nullptr, dst, Read16(src, 0));
            src += src_step;
            dst += dst_step;
        }
    }

    // Persist updated addresses in the DMA latch.
    g_dma_latch[ch].sad = src;
    g_dma_latch[ch].dad = dst;

    if (cnt_h & 0x4000) {
        io_IF_.fetch_or(1 << (8 + ch), std::memory_order_relaxed);
#ifdef B_DEBUG
        uint16_t if_ = io_IF_.load(std::memory_order_relaxed);
        std::ostringstream msg;
        msg << "[IRQ] raise DMA" << ch << " -> IF=0x" << std::hex << if_;
        Logger::log(msg.str(), LogLevel::DEBUG);
#endif
    }

    // Reload count and optional destination in repeat mode.
    if (repeat && timing != 0) {
        uint32_t orig_count = io_regs_[sad_off + 8] | (io_regs_[sad_off + 9] << 8);
        g_dma_latch[ch].count = orig_count ? orig_count : ((ch == 3) ? 0x10000 : 0x4000);
        
        if (dst_adj == 3) { // Increment & Reload
            uint32_t orig_dad = io_regs_[sad_off + 4] | (io_regs_[sad_off + 5] << 8) | 
                               (io_regs_[sad_off + 6] << 16) | (io_regs_[sad_off + 7] << 24);
            uint32_t dst_mask = (ch == 3) ? 0x0FFFFFFF : 0x07FFFFFF;
            g_dma_latch[ch].dad = orig_dad & dst_mask;
        }
    } else {
        cnt_h &= ~0x8000;
        io_regs_[sad_off + 10] = cnt_h & 0xFF;
        io_regs_[sad_off + 11] = cnt_h >> 8;
    }
}

void MemoryBus::Write32(CpuState* s, uint32_t addr, uint32_t value) {
    uint32_t aligned_addr = addr & ~3;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) return;
        // BIOS and Game Pak ROM (0x08..0x0D) are read-only on GBA.
        if (region == 0x00 || (region >= 0x08 && region <= 0x0D)) return;
        if (region == 0x0E) {
            if (backup_type_ == BackupType::kFlash) {
                flash_backup_.Write8(aligned_addr, static_cast<uint8_t>(value & 0xFFu), sram_.data(), sram_.size());
            } else {
                uint32_t eff = MaskedAddr(page, aligned_addr);
                *(host_ptr + eff) = static_cast<uint8_t>(value & 0xFFu);
            }
            return;
        }
#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
        uint16_t old_wait_hw = 0;
        bool log_waitword = false;
        if (region == 0x03 && aligned_addr == 0x0300310Cu) {
            uint32_t eff_pre = MaskedAddr(page, aligned_addr);
            old_wait_hw = *reinterpret_cast<uint16_t*>(host_ptr + eff_pre);
            log_waitword = true;
        }
#endif
#endif
        uint32_t eff = MaskedAddr(page, aligned_addr);
        *reinterpret_cast<uint32_t*>(host_ptr + eff) = value;
#ifdef B_DEBUG
#if LOGO_LINK_WATCH
        {
            const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
            MaybeLogLogoLinkWatchRW("W32", aligned_addr, value, pc);
        }
#endif
#endif
        // Probe BIOS scratch writes used by the 0xC04 fill path.
        if (region == 0x03) {
#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
            if (log_waitword) {
                const uint16_t ie = static_cast<uint16_t>(io_regs_[0x200]) |
                                    (static_cast<uint16_t>(io_regs_[0x201]) << 8);
                const uint16_t if_reg = io_IF_.load(std::memory_order_relaxed);
                const uint16_t ime = io_IME_;
                const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                MaybeLogIrqWaitWordWrite("W32", aligned_addr, old_wait_hw, value & 0xFFFFu, pc, ie, if_reg, ime);
            }
#endif
#if LOGO_SCRATCH_WATCH
            const bool is_logo_scratch_word = (aligned_addr == 0x03007EA0u);
            const bool is_logo_scratch_range =
                (aligned_addr >= 0x03007E00u && aligned_addr < 0x03008000u);
            if (is_logo_scratch_word ||
#if LOGO_SCRATCH_RANGE_WATCH
                is_logo_scratch_range
#else
                false
#endif
            ) {
                static int scratch_w32 = 0;
                if (++scratch_w32 <= 220) {
                    const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                    std::printf("[LOGO-SCRATCH] W32 @0x%08X = 0x%08X PC=0x%08X\n",
                                aligned_addr, value, pc);
                }
            }
#endif
#endif
        }
        // Update graphics write counters.
        if (region == 0x06) {
            g_vram_write_count.fetch_add(1, std::memory_order_relaxed);
#ifdef B_DEBUG
#if LOGO_TILE_WATCH
            {
                const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                MaybeLogLogoFirstNonZero(aligned_addr, value, pc);
            }
#endif
#if LOGO_TILE_WATCH
            {
                uint32_t vram_off = aligned_addr & 0x1FFFF;
                if (vram_off >= 0x18000) vram_off -= 0x8000;
                if (vram_off >= 0x2C40 && vram_off < 0x3000) {
                    static int logo_src_w32 = 0;
                    if (++logo_src_w32 <= 120) {
                        const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                        std::printf("[LOGO-SRC] W32 @vram+0x%04X = 0x%08X (GBA 0x%08X) PC=0x%08X\n",
                                    vram_off, value, aligned_addr, pc);
                    }
                }
            }
#endif
#if VRAM_DEBUG_LOGS
            // Watchpoint: BG3 tile map scrBase region (VRAM 0xB800-0xBBFF)
            {
                uint32_t vram_off = aligned_addr & 0x1FFFF;
                if (vram_off >= 0x18000) vram_off -= 0x8000;
                if (vram_off >= 0xB800 && vram_off < 0xBC00) {
                    static int map_w32 = 0;
                    if (++map_w32 <= 100)
                        std::printf("[VRAM-MAP] W32 @vram+0x%04X = 0x%08X (GBA 0x%08X)\n",
                                    vram_off, value, aligned_addr);
                }
                // Probe OBJ high tile writes used by BIOS sprite setup.
                if (vram_off >= 0x16000 && vram_off < 0x17000) {
                    static int obj_hi_w32 = 0;
                    static uint32_t last_pc = 0xFFFFFFFFu;
                    const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                    const bool important = (value != 0) || (pc != last_pc);
                    if (important && ++obj_hi_w32 <= 200) {
                        std::printf("[VRAM-OBJHI] W32 @vram+0x%05X = 0x%08X (GBA 0x%08X) PC=0x%08X\n",
                                    vram_off, value, aligned_addr, pc);
                    }
                    last_pc = pc;
                }
            }
#endif
#endif
        }
        else if (region == 0x05) g_pal_write_count.fetch_add(1, std::memory_order_relaxed);
        else if (region == 0x07) g_oam_write_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        if ((aligned_addr >> 24) == 0x04) {
            const uint32_t off = aligned_addr & 0x3FF;
            if (off == 0x0A0) {  // FIFO_A
                PushFifoWord(0, value);
                return;
            }
            if (off == 0x0A4) {  // FIFO_B
                PushFifoWord(1, value);
                return;
            }
            Write16(s, aligned_addr, static_cast<uint16_t>(value));
            Write16(s, aligned_addr + 2, static_cast<uint16_t>(value >> 16));
        }
    }
}

void MemoryBus::Write16(CpuState* s, uint32_t addr, uint16_t value) {
    uint32_t aligned_addr = addr & ~1;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) return;
        // BIOS and Game Pak ROM (0x08..0x0D) are read-only on GBA.
        if (region == 0x00 || (region >= 0x08 && region <= 0x0D)) return;
        if (region == 0x0E) {
            if (backup_type_ == BackupType::kFlash) {
                flash_backup_.Write8(aligned_addr, static_cast<uint8_t>(value & 0xFFu), sram_.data(), sram_.size());
            } else {
                uint32_t eff = MaskedAddr(page, aligned_addr);
                *(host_ptr + eff) = static_cast<uint8_t>(value & 0xFFu);
            }
            return;
        }
#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
        uint16_t old_wait_hw = 0;
        bool log_waitword = false;
        if (region == 0x03 && aligned_addr == 0x0300310Cu) {
            uint32_t eff_pre = MaskedAddr(page, aligned_addr);
            old_wait_hw = *reinterpret_cast<uint16_t*>(host_ptr + eff_pre);
            log_waitword = true;
        }
#endif
#endif
        uint32_t eff = MaskedAddr(page, aligned_addr);
        *reinterpret_cast<uint16_t*>(host_ptr + eff) = value;
        if (region == 0x03) {
#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
            if (log_waitword) {
                const uint16_t ie = static_cast<uint16_t>(io_regs_[0x200]) |
                                    (static_cast<uint16_t>(io_regs_[0x201]) << 8);
                const uint16_t if_reg = io_IF_.load(std::memory_order_relaxed);
                const uint16_t ime = io_IME_;
                const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                MaybeLogIrqWaitWordWrite("W16", aligned_addr, old_wait_hw, value, pc, ie, if_reg, ime);
            }
#endif
#if LOGO_SCRATCH_WATCH
            const bool is_logo_scratch_halfword =
                (aligned_addr == 0x03007EA0u || aligned_addr == 0x03007EA2u);
            const bool is_logo_scratch_range =
                (aligned_addr >= 0x03007E00u && aligned_addr < 0x03008000u);
            if (is_logo_scratch_halfword ||
#if LOGO_SCRATCH_RANGE_WATCH
                is_logo_scratch_range
#else
                false
#endif
            ) {
                static int scratch_w16 = 0;
                if (++scratch_w16 <= 360) {
                    const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                    std::printf("[LOGO-SCRATCH] W16 @0x%08X = 0x%04X PC=0x%08X\n",
                                aligned_addr, (unsigned)value, pc);
                }
            }
#endif
#endif
        }
        // Update graphics write counters.
        if (region == 0x06) {
            g_vram_write_count.fetch_add(1, std::memory_order_relaxed);
#ifdef B_DEBUG
#if LOGO_TILE_WATCH
            {
                const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                MaybeLogLogoFirstNonZero(aligned_addr, static_cast<uint32_t>(value), pc);
            }
#endif
#if LOGO_TILE_WATCH
            {
                uint32_t vram_off = aligned_addr & 0x1FFFF;
                if (vram_off >= 0x18000) vram_off -= 0x8000;
                if (vram_off >= 0x2C40 && vram_off < 0x3000) {
                    static int logo_src_w16 = 0;
                    if (++logo_src_w16 <= 160) {
                        const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                        std::printf("[LOGO-SRC] W16 @vram+0x%04X = 0x%04X (GBA 0x%08X) PC=0x%08X\n",
                                    vram_off, (unsigned)value, aligned_addr, pc);
                    }
                }
            }
#endif
#if VRAM_DEBUG_LOGS
            // Watchpoint: BG3 tile map scrBase region (VRAM 0xB800-0xBBFF)
            {
                uint32_t vram_off = aligned_addr & 0x1FFFF;
                if (vram_off >= 0x18000) vram_off -= 0x8000;
                if (vram_off >= 0xB800 && vram_off < 0xBC00) {
                    static int map_w16 = 0;
                    if (++map_w16 <= 100)
                        std::printf("[VRAM-MAP] W16 @vram+0x%04X = 0x%04X (GBA 0x%08X)\n",
                                    vram_off, (unsigned)value, aligned_addr);
                }
                // Probe OBJ high tile writes used by BIOS sprite setup.
                if (vram_off >= 0x16000 && vram_off < 0x17000) {
                    static int obj_hi_w16 = 0;
                    static uint32_t last_pc = 0xFFFFFFFFu;
                    const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                    const bool important = (value != 0) || (pc != last_pc);
                    if (important && ++obj_hi_w16 <= 300) {
                        std::printf("[VRAM-OBJHI] W16 @vram+0x%05X = 0x%04X (GBA 0x%08X) PC=0x%08X\n",
                                    vram_off, (unsigned)value, aligned_addr, pc);
                    }
                    last_pc = pc;
                }
            }
#endif
#endif
        }
        else if (region == 0x05) g_pal_write_count.fetch_add(1, std::memory_order_relaxed);
        else if (region == 0x07) g_oam_write_count.fetch_add(1, std::memory_order_relaxed);
    } else {
        if ((aligned_addr >> 24) != 0x04) return;
        uint32_t off = aligned_addr & 0x3FF;
        switch (off) {
            case 0x004: { 
                uint16_t old = io_regs_[4] | (io_regs_[5] << 8);
                uint16_t updated = (s == nullptr) ? value : (old & 0x0007) | (value & 0xFFF8);
                io_regs_[4] = updated & 0xFF;
                io_regs_[5] = updated >> 8;
                break;
            }
            case 0x202: {
                io_IF_.fetch_and(~value, std::memory_order_relaxed);
                irq_if_clears_.fetch_add(1, std::memory_order_relaxed);
#ifdef B_DEBUG
                {
                    // Only compare the write value for dedup — the resulting
                    // if_ races with VBlank/HBlank setting bits and would
                    // defeat suppression.
                    static uint16_t last_if_write = 0xFFFF;
                    static int if_clear_repeat = 0;
                    if (value == last_if_write) {
                        ++if_clear_repeat;
                    } else {
                        if (if_clear_repeat > 0) {
                            Logger::log("[IRQ] IF clear 0x" + std::to_string(last_if_write) +
                                        " (x" + std::to_string(if_clear_repeat) + " suppressed)", LogLevel::DEBUG);
                        }
                        if_clear_repeat = 0;
                        last_if_write = value;
                        uint16_t if_ = io_IF_.load(std::memory_order_relaxed);
                        std::ostringstream msg;
                        msg << "[IRQ] IF clear: write 0x" << std::hex << value << " -> IF=0x" << if_;
                        Logger::log(msg.str(), LogLevel::DEBUG);
                    }
                }
#endif
                break;
            }
            case 0x204: io_WAITCNT_ = value; break;
            case 0x208: {
                io_IME_ = value;
                break;
            }
            case 0x082: {  // SOUNDCNT_H
                io_regs_[0x082] = static_cast<uint8_t>(value & 0xFF);
                io_regs_[0x083] = static_cast<uint8_t>(value >> 8);
                RecomputeSoundControlCache();
                if (value & (1u << 11)) ResetFifo(0);
                if (value & (1u << 15)) ResetFifo(1);
                return;
            }
            case 0x084: {  // SOUNDCNT_X (master)
                io_regs_[0x084] = static_cast<uint8_t>(value & 0x80u);
                io_regs_[0x085] = 0;
                RecomputeSoundControlCache();
                return;
            }
            case 0x088: {  // SOUNDBIAS
                io_regs_[0x088] = static_cast<uint8_t>(value & 0xFF);
                io_regs_[0x089] = static_cast<uint8_t>(value >> 8);
                return;
            }
            case 0x0A0: {  // FIFO_A low half
                PushFifoWord(0, static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 16));
                return;
            }
            case 0x0A2: {  // FIFO_A high half
                PushFifoWord(0, static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 16));
                return;
            }
            case 0x0A4: {  // FIFO_B low half
                PushFifoWord(1, static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 16));
                return;
            }
            case 0x0A6: {  // FIFO_B high half
                PushFifoWord(1, static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 16));
                return;
            }
            case 0x100: case 0x104: case 0x108: case 0x10C: {
                int ch = (off - 0x100) / 4;
                timer_reload_[ch] = value;
                uint16_t cnt_h = static_cast<uint16_t>(io_regs_[off + 2]) |
                                 (static_cast<uint16_t>(io_regs_[off + 3]) << 8);
                if ((cnt_h & 0x80u) == 0) {
                    io_regs_[off] = static_cast<uint8_t>(value & 0xFF);
                    io_regs_[off + 1] = static_cast<uint8_t>(value >> 8);
                }
                return;
            }
            case 0x102: case 0x106: case 0x10A: case 0x10E: {
                int ch = (off - 0x102) / 4;
                uint16_t old_h = static_cast<uint16_t>(io_regs_[off]) |
                                 (static_cast<uint16_t>(io_regs_[off + 1]) << 8);
                io_regs_[off] = static_cast<uint8_t>(value & 0xFF);
                io_regs_[off + 1] = static_cast<uint8_t>(value >> 8);
                const bool was_enabled = (old_h & 0x80u) != 0;
                const bool now_enabled = (value & 0x80u) != 0;
                if (!was_enabled && now_enabled) {
                    const uint32_t low_off = 0x100 + ch * 4;
                    io_regs_[low_off] = static_cast<uint8_t>(timer_reload_[ch] & 0xFF);
                    io_regs_[low_off + 1] = static_cast<uint8_t>(timer_reload_[ch] >> 8);
                    timer_subcycles_[ch] = 0;
                } else if (!now_enabled) {
                    timer_subcycles_[ch] = 0;
                }
                return;
            }
            case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE: {
                int ch = (off - 0x0BA) / 12;
                uint16_t old_cnt = io_regs_[off] | (io_regs_[off + 1] << 8);
                io_regs_[off] = static_cast<uint8_t>(value);
                io_regs_[off + 1] = static_cast<uint8_t>(value >> 8);
                
                // Capture source, destination, and count when DMA is enabled.
                if (!(old_cnt & 0x8000) && (value & 0x8000)) {
                    uint32_t sad = io_regs_[0x0B0 + ch*12] | (io_regs_[0x0B1 + ch*12] << 8) | 
                                  (io_regs_[0x0B2 + ch*12] << 16) | (io_regs_[0x0B3 + ch*12] << 24);
                    uint32_t dad = io_regs_[0x0B4 + ch*12] | (io_regs_[0x0B5 + ch*12] << 8) | 
                                  (io_regs_[0x0B6 + ch*12] << 16) | (io_regs_[0x0B7 + ch*12] << 24);
                    uint32_t count = io_regs_[0x0B8 + ch*12] | (io_regs_[0x0B9 + ch*12] << 8);
                    
                    uint32_t src_mask = (ch == 0) ? 0x07FFFFFF : 0x0FFFFFFF;
                    uint32_t dst_mask = (ch == 3) ? 0x0FFFFFFF : 0x07FFFFFF;
                    
                    g_dma_latch[ch].sad = sad & src_mask;
                    g_dma_latch[ch].dad = dad & dst_mask;
                    g_dma_latch[ch].count = count ? count : ((ch == 3) ? 0x10000 : 0x4000);
                    
                    if (((value >> 12) & 3) == 0) { 
                        ExecuteDmaTransfer(ch);
                    }
                }
                return;
            }
            case 0x300: {  // Handle 16-bit stores to HALTCNT.
                if (s) s->halted = true;
                break;
            }
            default: break;
        }
        if (off != 0x202 && off != 0x004) {
            io_regs_[off] = static_cast<uint8_t>(value);
            io_regs_[off + 1] = static_cast<uint8_t>(value >> 8);

#ifdef B_DEBUG
            if (g_watchpoints && g_pause_flag) {
                // Generic I/O-offset watchpoint
                if (ShouldPauseOnIOOffset(*g_watchpoints, off)) {
                    std::ostringstream msg;
                    msg << "[MemoryBus] I/O watchpoint: offset=0x" << std::hex << off
                        << " value=0x" << value;
                    Logger::log(msg.str(), LogLevel::DEBUG);
                    g_pause_flag->store(true, std::memory_order_release);
                }
                // BG affine reference registers: BG2X/Y (0x028-0x02E) and
                // BG3X/Y (0x038-0x03E).  Both halves of a 32-bit register
                // arrive as separate 16-bit writes, so check both.
                const bool is_bg_affine = (off >= 0x028 && off <= 0x02E) ||
                                          (off >= 0x038 && off <= 0x03E);
                if (is_bg_affine) {
                    if (g_watchpoints->log_bg_writes) {
                        std::ostringstream msg;
                        msg << "[BG] affine write off=0x" << std::hex << off
                            << " val=0x" << value;
                        Logger::log(msg.str(), LogLevel::DEBUG);
                    }
                    if (ShouldPauseOnBGWrite(*g_watchpoints, off & ~1u))
                        g_pause_flag->store(true, std::memory_order_release);
                }
            }
#endif  // B_DEBUG
        }
        (void)s;
    }
}

void MemoryBus::Write8(CpuState* s, uint32_t addr, uint8_t value) {
    uint32_t page = addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    if (host_ptr != nullptr) {
        uint32_t region = addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(addr)) return;

        // BIOS and Game Pak ROM (0x08..0x0D) are read-only on GBA.
        if (region == 0x00 || (region >= 0x08 && region <= 0x0D)) return;
        if (region == 0x0E) {
            if (backup_type_ == BackupType::kFlash) {
                flash_backup_.Write8(addr, value, sram_.data(), sram_.size());
            } else {
                uint32_t eff = MaskedAddr(page, addr);
                *(host_ptr + eff) = value;
            }
            return;
        }

        if (region == 0x05 || region == 0x06 || region == 0x07) {
            if (region == 0x07) return; // OAM: byte writes ignored
            if (region == 0x06) {
                // OBJ VRAM byte writes are ignored.
                // Boundary depends on BG mode: 0x06014000 for bitmap (modes 3-5),
                // 0x06010000 for tile modes (modes 0-2).
                uint16_t dispcnt = io_regs_[0] | (io_regs_[1] << 8);
                uint32_t bg_mode = dispcnt & 0x07;
                uint32_t obj_base = (bg_mode >= 3) ? 0x06014000 : 0x06010000;
                uint32_t vram_offset = addr & 0x1FFFF; // mirror within 128KB
                if (vram_offset >= (obj_base & 0x1FFFF)) return;
            }
            uint32_t aligned_addr = addr & ~1;
            uint32_t eff = MaskedAddr(page, aligned_addr);
            uint16_t forced_16bit = value | (value << 8);
            *reinterpret_cast<uint16_t*>(host_ptr + eff) = forced_16bit;
            return;
        }

        uint32_t eff = MaskedAddr(page, addr);
        if (region == 0x03) {
#ifdef B_DEBUG
#if IRQ_WAITWORD_WATCH
            // Handle byte writes that touch 0x0300310C/0x0300310D.
            if ((addr & ~1u) == 0x0300310Cu) {
                const uint16_t old_hw = *reinterpret_cast<uint16_t*>(host_ptr + (eff & ~1u));
                uint16_t new_hw = old_hw;
                if (addr & 1u) {
                    new_hw = static_cast<uint16_t>((new_hw & 0x00FFu) | (static_cast<uint16_t>(value) << 8));
                } else {
                    new_hw = static_cast<uint16_t>((new_hw & 0xFF00u) | value);
                }
                const uint16_t ie = static_cast<uint16_t>(io_regs_[0x200]) |
                                    (static_cast<uint16_t>(io_regs_[0x201]) << 8);
                const uint16_t if_reg = io_IF_.load(std::memory_order_relaxed);
                const uint16_t ime = io_IME_;
                const uint32_t pc = s ? s->registers[15] : 0xFFFFFFFFu;
                MaybeLogIrqWaitWordWrite("W8 ", 0x0300310Cu, old_hw, new_hw, pc, ie, if_reg, ime);
            }
#endif
#endif
        }
        *(host_ptr + eff) = value;
    } else {
        if ((addr >> 24) == 0x04) {
            uint32_t off = addr & 0x3FF;
            if (off == 0x202) {
                io_IF_.fetch_and(~value, std::memory_order_relaxed);
                irq_if_clears_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (off == 0x203) {
                io_IF_.fetch_and(~(value << 8), std::memory_order_relaxed);
                irq_if_clears_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (off == 0x004) { io_regs_[4] = (io_regs_[4] & 0x07) | (value & 0xF8); return; }
            if (off == 0x204) { io_WAITCNT_ = static_cast<uint16_t>((io_WAITCNT_ & 0xFF00u) | value); io_regs_[off] = value; return; }
            if (off == 0x205) { io_WAITCNT_ = static_cast<uint16_t>((io_WAITCNT_ & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); io_regs_[off] = value; return; }
            if (off == 0x208) { io_IME_ = static_cast<uint16_t>((io_IME_ & 0xFF00u) | value); io_regs_[off] = value; return; }
            if (off == 0x209) { io_IME_ = static_cast<uint16_t>((io_IME_ & 0x00FFu) | (static_cast<uint16_t>(value) << 8)); io_regs_[off] = value; return; }
            if (off == 0x082 || off == 0x083) {
                io_regs_[off] = value;
                uint16_t full = static_cast<uint16_t>(io_regs_[0x082]) |
                                (static_cast<uint16_t>(io_regs_[0x083]) << 8);
                RecomputeSoundControlCache();
                if (full & (1u << 11)) ResetFifo(0);
                if (full & (1u << 15)) ResetFifo(1);
                return;
            }
            if (off == 0x084 || off == 0x085) {
                if (off == 0x084) io_regs_[0x084] = static_cast<uint8_t>(value & 0x80u);
                else io_regs_[0x085] = 0;
                RecomputeSoundControlCache();
                return;
            }
            if (off == 0x088 || off == 0x089) {
                io_regs_[off] = value;
                return;
            }
            if (off >= 0x0A0 && off <= 0x0A7) {
                const int channel = (off < 0x0A4) ? 0 : 1;
                PushFifoWord(channel, static_cast<uint32_t>(value) * 0x01010101u);
                return;
            }

            // Timer reload bytes (TMxCNT_L). Writes always update reload; running
            // counters keep counting and only copy reload when disabled/started.
            if (off == 0x100 || off == 0x101 || off == 0x104 || off == 0x105 ||
                off == 0x108 || off == 0x109 || off == 0x10C || off == 0x10D) {
                const int ch = static_cast<int>((off - 0x100) / 4);
                const uint32_t low_off = 0x100 + static_cast<uint32_t>(ch) * 4u;
                if ((off & 1u) == 0u) {
                    timer_reload_[ch] = static_cast<uint16_t>((timer_reload_[ch] & 0xFF00u) | value);
                } else {
                    timer_reload_[ch] = static_cast<uint16_t>((timer_reload_[ch] & 0x00FFu) |
                                                              (static_cast<uint16_t>(value) << 8));
                }
                io_regs_[off] = value;
                uint16_t cnt_h = static_cast<uint16_t>(io_regs_[low_off + 2]) |
                                 (static_cast<uint16_t>(io_regs_[low_off + 3]) << 8);
                if ((cnt_h & 0x80u) == 0) {
                    io_regs_[low_off] = static_cast<uint8_t>(timer_reload_[ch] & 0xFF);
                    io_regs_[low_off + 1] = static_cast<uint8_t>(timer_reload_[ch] >> 8);
                }
                return;
            }

            // Timer control bytes (TMxCNT_H). Handle enable edge and stop semantics
            // identically to Write16 so byte-wise setup works.
            if (off == 0x102 || off == 0x103 || off == 0x106 || off == 0x107 ||
                off == 0x10A || off == 0x10B || off == 0x10E || off == 0x10F) {
                const int ch = static_cast<int>((off - 0x102) / 4);
                const uint32_t high_off = 0x102 + static_cast<uint32_t>(ch) * 4u;
                const uint32_t low_off = 0x100 + static_cast<uint32_t>(ch) * 4u;
                uint16_t old_h = static_cast<uint16_t>(io_regs_[high_off]) |
                                 (static_cast<uint16_t>(io_regs_[high_off + 1]) << 8);
                io_regs_[off] = value;
                uint16_t new_h = static_cast<uint16_t>(io_regs_[high_off]) |
                                 (static_cast<uint16_t>(io_regs_[high_off + 1]) << 8);
                const bool was_enabled = (old_h & 0x80u) != 0;
                const bool now_enabled = (new_h & 0x80u) != 0;
                if (!was_enabled && now_enabled) {
                    io_regs_[low_off] = static_cast<uint8_t>(timer_reload_[ch] & 0xFF);
                    io_regs_[low_off + 1] = static_cast<uint8_t>(timer_reload_[ch] >> 8);
                    timer_subcycles_[ch] = 0;
                } else if (!now_enabled) {
                    timer_subcycles_[ch] = 0;
                }
                return;
            }
            
            io_regs_[off] = value;
            
            // HALTCNT write enters stop/halt state.
            if (addr == 0x04000301 && s) s->halted = true;
        }
    }
}

uint32_t MemoryBus::Read32(uint32_t addr, uint32_t current_pc) const {
    uint32_t aligned_addr = addr & ~3;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    uint32_t data = 0;

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) {
            data = eeprom_backup_.ReadReady32();
        } else
        if (region == 0x0E) {
            data = flash_backup_.Read32(aligned_addr, sram_.data(), sram_.size());
#ifdef B_DEBUG
#if FLASH_READ_WATCH
            MaybeLogFlashRead("R32", aligned_addr, data, current_pc, backup_type_);
#endif
#endif
        } else if (region == 0x00 && current_pc >= 0x4000) {
            data = g_bios_latch; // BIOS protection open-bus read.
#ifdef B_DEBUG
#if BIOS_PROTECT_TRACE
            static int bios_protect_r32 = 0;
            if (++bios_protect_r32 <= 180) {
                std::printf("[BIOS-PROTECT] R32 addr=0x%08X pc=0x%08X -> latch=0x%08X\n",
                            aligned_addr, current_pc, data);
            }
#endif
#endif
        } else {
            uint32_t eff = MaskedAddr(page, aligned_addr);
            data = *reinterpret_cast<uint32_t*>(host_ptr + eff);
            if (region == 0x00) g_bios_latch = data; // Update BIOS latch on real BIOS reads.
        }
    } else {
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) {
            data = eeprom_backup_.ReadReady32();
        } else if ((aligned_addr >> 24) == 0x04) {
            data = static_cast<uint32_t>(Read16(aligned_addr, current_pc))
                 | (static_cast<uint32_t>(Read16(aligned_addr + 2, current_pc)) << 16);
        } else if ((aligned_addr >> 24) >= 0x08 && (aligned_addr >> 24) <= 0x0D) {
            // GBATEK "Unpredictable Things": Game Pak open bus = (addr/2) & FFFFh
            uint16_t lo = static_cast<uint16_t>((aligned_addr >> 1) & 0xFFFF);
            uint16_t hi = static_cast<uint16_t>(((aligned_addr + 2) >> 1) & 0xFFFF);
            data = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
        } else {
            data = GetOpenBus(current_pc);
        }
    }

    uint32_t shift_bits = (addr & 3) * 8;
    const uint32_t rotated = std::rotr(data, shift_bits);
#ifdef B_DEBUG
#if LOGO_LINK_WATCH
    MaybeLogLogoLinkWatchRW("R32", addr & ~3u, rotated, current_pc);
#endif
#endif
    return rotated;
}

uint16_t MemoryBus::Read16(uint32_t addr, uint32_t current_pc) const {
    uint32_t aligned_addr = addr & ~1;
    uint32_t page = aligned_addr >> 14;
    uint8_t* host_ptr = static_cast<uint8_t*>(page_table_[page]);

    uint16_t data = 0;

    if (host_ptr != nullptr) {
        uint32_t region = aligned_addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) {
            data = eeprom_backup_.ReadReady16();
        } else
        if (region == 0x0E) {
            data = flash_backup_.Read16(aligned_addr, sram_.data(), sram_.size());
#ifdef B_DEBUG
#if FLASH_READ_WATCH
            MaybeLogFlashRead("R16", aligned_addr, data, current_pc, backup_type_);
#endif
#endif
        } else if (region == 0x00 && current_pc >= 0x4000) {
            data = static_cast<uint16_t>(g_bios_latch >> ((addr & 2) * 8)); // BIOS protection open-bus read.
#ifdef B_DEBUG
#if BIOS_PROTECT_TRACE
            static int bios_protect_r16 = 0;
            if (++bios_protect_r16 <= 220) {
                std::printf("[BIOS-PROTECT] R16 addr=0x%08X pc=0x%08X -> latch=0x%04X\n",
                            aligned_addr, current_pc, data);
            }
#endif
#endif
        } else {
            uint32_t eff = MaskedAddr(page, aligned_addr);
            data = *reinterpret_cast<uint16_t*>(host_ptr + eff);
            // Keep the 32-bit BIOS latch unchanged on halfword reads.
        }
    } else {
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(aligned_addr)) {
            data = eeprom_backup_.ReadReady16();
        } else if ((aligned_addr >> 24) == 0x04) {
            uint32_t off = aligned_addr & 0x3FF;
            if (off == 0x202) data = io_IF_.load(std::memory_order_relaxed);
            else if (off == 0x204) data = io_WAITCNT_;
            else if (off == 0x208) data = io_IME_;
            else if (off == 0x130) data = io_KEYINPUT_.load(std::memory_order_relaxed);
            else if (off == 0x084) data = io_regs_[off] & 0x80;  // SOUNDCNT_X
            else data = static_cast<uint16_t>(io_regs_[off]) | (static_cast<uint16_t>(io_regs_[off + 1]) << 8);
        } else if ((aligned_addr >> 24) >= 0x08 && (aligned_addr >> 24) <= 0x0D) {
            // GBATEK "Unpredictable Things": Game Pak open bus = (addr/2) & FFFFh
            data = static_cast<uint16_t>((aligned_addr >> 1) & 0xFFFF);
        } else {
            data = static_cast<uint16_t>(GetOpenBus(current_pc));
        }
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
        uint32_t region = addr >> 24;
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(addr)) {
            return eeprom_backup_.ReadReady8();
        }
        if (region == 0x00 && current_pc >= 0x4000) {
            uint8_t data = static_cast<uint8_t>(g_bios_latch >> ((addr & 3) * 8)); // BIOS protection open-bus read.
#ifdef B_DEBUG
#if BIOS_PROTECT_TRACE
            static int bios_protect_r8 = 0;
            if (++bios_protect_r8 <= 320) {
                std::printf("[BIOS-PROTECT] R8  addr=0x%08X pc=0x%08X -> latch=0x%02X\n",
                            addr, current_pc, data);
            }
#endif
#endif
            return data;
        } else if (region == 0x0E) {
            const uint8_t data = flash_backup_.Read8(addr, sram_.data(), sram_.size());
#ifdef B_DEBUG
#if FLASH_READ_WATCH
            MaybeLogFlashRead("R8 ", addr, data, current_pc, backup_type_);
#endif
#endif
            return data;
        } else {
            uint32_t eff = MaskedAddr(page, addr);
            return *(host_ptr + eff);
        }
    } else {
        if (backup_type_ == BackupType::kEeprom && EepromBackup::IsAddress(addr)) {
            return eeprom_backup_.ReadReady8();
        }
        if ((addr >> 24) == 0x04) {
            uint32_t off = addr & 0x3FF;
            if (off == 0x202) return static_cast<uint8_t>(io_IF_.load(std::memory_order_relaxed));
            if (off == 0x203) return static_cast<uint8_t>(io_IF_.load(std::memory_order_relaxed) >> 8);
            if (off == 0x204) return static_cast<uint8_t>(io_WAITCNT_);
            if (off == 0x205) return static_cast<uint8_t>(io_WAITCNT_ >> 8);
            if (off == 0x208) return static_cast<uint8_t>(io_IME_);
            if (off == 0x209) return static_cast<uint8_t>(io_IME_ >> 8);
            
            // KEYINPUT and SOUNDCNT are synthesized on demand; others use io_regs_.
            if (off == 0x130 || off == 0x131 || off == 0x084 || off == 0x085) {
                uint16_t live_val = Read16(addr & ~1u, current_pc);
                return static_cast<uint8_t>((addr & 1) ? (live_val >> 8) : live_val);
            }
            
            return io_regs_[off];
        } else {
            uint32_t region = addr >> 24;
            if (region >= 0x08 && region <= 0x0D) {
                // GBATEK "Unpredictable Things": Game Pak open bus = (addr/2) & FFFFh
                uint16_t hw = static_cast<uint16_t>(((addr & ~1u) >> 1) & 0xFFFF);
                return static_cast<uint8_t>((addr & 1) ? (hw >> 8) : hw);
            }
            uint32_t open_bus = GetOpenBus(current_pc);
            return static_cast<uint8_t>(open_bus >> ((addr & 3) * 8));
        }
    }
}

void MemoryBus::SetKeyInputState(uint16_t keyinput) {
    io_KEYINPUT_.store(static_cast<uint16_t>(keyinput & 0x03FFu), std::memory_order_relaxed);
}


bool MemoryBus::LoadBios(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);

    if (!file.is_open()) {
        return false;
    }

    file.read(reinterpret_cast<char*>(bios_.data()), kBiosSize);
    file.close();

    MapRegion(0x00000000, 0x00003FFF, kBiosSize, bios_.data());
    return true;
}

bool MemoryBus::LoadRom(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    auto file_size = file.tellg();
    if (file_size <= 0) return false;
    file.seekg(0, std::ios::beg);

    uint32_t size = static_cast<uint32_t>(std::min<std::streamoff>(file_size, kRomSize));
    rom_.resize(size, 0);  // allocate only what the ROM actually needs
    file.read(reinterpret_cast<char*>(rom_.data()), size);
    file.close();
    rom_loaded_size_ = size;
    backup_type_ = DetectBackupTypeFromRom(rom_);
    size_t save_size = kSramSize;
    if (backup_type_ == BackupType::kFlash) {
        save_size = DetectFlashStorageSizeFromRom(rom_);
    }
    sram_.assign(save_size, 0xFF);
    // Re-map to the current SRAM/FLASH backing buffer.
    MapRegion(0x0E000000, 0x0E00FFFF, kSramSize, sram_.data());
    eeprom_backup_.Reset();
    flash_backup_.Reset();
    if (backup_type_ == BackupType::kEeprom) eeprom_backup_.InitializeDefaultStorage();

    // Map across all three Game Pak wait-state mirrors (GBATEK: Memory Map).
    // Round up to 16 KB page boundary so the page table covers the entire ROM.
    uint32_t map_end = (size + 0x3FFFu) & ~0x3FFFu;
    MapRegion(0x08000000, 0x08000000 + map_end - 1, size, rom_.data());
    MapRegion(0x0A000000, 0x0A000000 + map_end - 1, size, rom_.data());
    MapRegion(0x0C000000, 0x0C000000 + map_end - 1, size, rom_.data());

    {
        std::ostringstream msg;
        msg << "[MemoryBus] ROM loaded: " << size << " bytes ("
            << (size / 1024) << " KB) mapped to 0x08/0A/0C"
            << " backup=" << BackupTypeName(backup_type_);
        Logger::log(msg.str(), LogLevel::INFO);
    }

    return true;
}
