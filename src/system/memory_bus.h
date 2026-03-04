#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "system/eeprom_backup.h"
#include "system/flash_backup.h"

struct CpuState;
struct MemoryMap;

class MemoryBus {
    public:
        using AudioBatchCallback = void(*)(void*, const int16_t*, size_t);

        enum class BackupType : uint8_t {
            kNone = 0,
            kSram,
            kFlash,
            kEeprom,
        };

        struct IrqDebugSnapshot {
            uint64_t vblank_raised = 0;
            uint64_t hblank_raised = 0;
            uint64_t vcount_raised = 0;
            uint64_t if_clears = 0;
        };

        MemoryBus();

        bool LoadBios(const std::string& filepath);
        bool LoadRom(const std::string& filepath);

        void** GetPageTablePtr(){ return page_table_.get(); }

        /** Direct read-only access to the raw VRAM backing store (96 KB). */
        const uint8_t* GetVramPtr() const { return vram_.data(); }
        size_t         GetVramSize() const { return vram_.size(); }

        /** Direct read-only access to Palette RAM (1 KB). */
        const uint8_t* GetPalettePtr() const { return palette_.data(); }
        size_t         GetPaletteSize() const { return palette_.size(); }

        /** Direct read-only access to OAM (1 KB). */
        const uint8_t* GetOamPtr() const { return oam_.data(); }
        size_t         GetOamSize() const { return oam_.size(); }
        bool HasRomLoaded() const { return rom_loaded_size_ != 0; }
        uint32_t GetRomLoadedSize() const { return rom_loaded_size_; }
        BackupType GetBackupType() const { return backup_type_; }

        /** Copies I/O, VRAM, palette, OAM into the buffers pointed to by mm. Lock-free: no shared refs. */
        void CopyToMemoryMap(MemoryMap* mm) const;

        /** Sets the VBlank bit in IF (0x04000202). Call once per frame from the render thread. Lock-free. */
        void RaiseVBlank();
        /** Sets the H-Blank IRQ bit in IF (bit 1). Called from JIT timing. */
        void RaiseHBlankIRQ();
        /** Sets the V-Count match IRQ bit in IF (bit 2). Called from JIT timing. */
        void RaiseVCountIRQ();
        IrqDebugSnapshot GetIrqDebugSnapshot() const;
        void ResetIrqDebugCounters();

        /** Execute any DMA channels with VBlank timing (start_timing==1). Called at scanline 160. */
        void TriggerVBlankDma();
        /** Execute any DMA channels with HBlank timing (start_timing==2). Called at each HBlank. */
        void TriggerHBlankDma();
        /** Advance hardware timers by CPU cycles. Called from dispatcher timing. */
        void StepTimers(uint32_t cpu_cycles);
        void SetAudioSampleCallback(AudioBatchCallback callback, void* user);

        // CpuState* for I/O side effects (HALTCNT, etc.); may be nullptr for non-JIT callers.
        void Write32(CpuState* s, uint32_t addr, uint32_t value);
        void Write16(CpuState* s, uint32_t addr, uint16_t value);
        void Write8(CpuState* s, uint32_t addr, uint8_t value);

        uint32_t Read32(uint32_t addr, uint32_t current_pc) const;

        uint16_t Read16(uint32_t addr, uint32_t current_pc) const;

        uint8_t Read8(uint32_t addr, uint32_t current_pc = 0) const;

    private:

        void MapRegion(uint32_t virtual_start, uint32_t virtual_end, uint32_t physical_size, uint8_t* host_ptr);

        uint32_t GetOpenBus(uint32_t current_pc) const;

        static constexpr uint32_t kPageOffsetMask = 0x3FFFu;
        uint32_t MaskedAddr(uint32_t page, uint32_t addr) const {
            return (addr & ~kPageOffsetMask) | (addr & page_mask_[page]);
        }

        void ExecuteDmaTransfer(int ch);
        void OnTimerOverflow(int timer_ch, uint32_t overflow_count);
        void EmitAudioForCycles(uint32_t cpu_cycles);
        void RecomputeSoundControlCache();
        void TriggerSoundDma(uint32_t fifo_addr);
        void PushFifoWord(int channel, uint32_t value);
        int8_t PopFifoSample(int channel);
        void ResetFifo(int channel);

        std::unique_ptr<void*[]> page_table_;
        std::unique_ptr<uint32_t[]> page_mask_;  // Intra-page offset mask (0x3FFF = full 16KB page)

        // I/O register shadow: 0x04000000–0x040003FF for PPU and CPU read-back.
        std::array<uint8_t, 0x400> io_regs_{};
        // Special I/O semantics (Bug 5.3, 5.4). IF is atomic so render thread can RaiseVBlank() lock-free.
        std::atomic<uint16_t> io_IF_{0};  // 0x04000202 — write-1-to-clear; bit 0 = VBlank
        uint16_t io_IME_ = 0;              // 0x04000208 — interrupt master enable
        uint16_t io_WAITCNT_ = 0; // 0x04000204 — waitstate control
        std::array<uint16_t, 4> timer_reload_{0, 0, 0, 0};
        std::array<uint32_t, 4> timer_subcycles_{0, 0, 0, 0};
        std::atomic<uint64_t> irq_vblank_raised_{0};
        std::atomic<uint64_t> irq_hblank_raised_{0};
        std::atomic<uint64_t> irq_vcount_raised_{0};
        std::atomic<uint64_t> irq_if_clears_{0};
        AudioBatchCallback audio_sample_callback_{nullptr};
        void* audio_sample_callback_user_{nullptr};

        struct DirectSoundFifo {
            std::array<int8_t, 32> data{};
            uint8_t read_idx = 0;
            uint8_t size = 0;
            int8_t last_sample = 0;
        };
        DirectSoundFifo ds_fifo_[2];
        int16_t ds_hold_left_ = 0;
        int16_t ds_hold_right_ = 0;
        uint64_t audio_resample_accum_ = 0;

        static constexpr uint32_t kAudioHostRate = 48000;
        static constexpr uint32_t kAudioCpuHz = 16777216;

        struct SoundControlCache {
            bool master_enabled = false;
            bool a_timer1 = false;
            bool b_timer1 = false;
            bool a_r = false;
            bool a_l = false;
            bool b_r = false;
            bool b_l = false;
            int16_t a_gain = 0; // 128 (50%) or 256 (100%)
            int16_t b_gain = 0; // 128 (50%) or 256 (100%)
        };
        SoundControlCache snd_cache_{};

        std::vector<uint8_t> bios_;
        std::vector<uint8_t> ewram_;
        std::vector<uint8_t> iwram_;
        std::vector<uint8_t> palette_;
        std::vector<uint8_t> vram_;
        std::vector<uint8_t> oam_;
        std::vector<uint8_t> rom_;
        uint32_t rom_loaded_size_{0};  // Actual ROM bytes loaded; 0 = no cartridge
        std::vector<uint8_t> sram_;
        EepromBackup eeprom_backup_;
        FlashBackup flash_backup_;
        BackupType backup_type_{BackupType::kNone};

};
