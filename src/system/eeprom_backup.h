#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class EepromBackup {
public:
    static bool IsAddress(uint32_t addr);

    void Reset();
    void InitializeDefaultStorage();

    uint32_t ReadReady32() const;
    uint16_t ReadReady16() const;
    uint8_t ReadReady8() const;

    bool HandleDma3Transfer(bool is_32bit,
                            uint32_t count,
                            int src_step,
                            int dst_step,
                            uint32_t& src,
                            uint32_t& dst,
                            const std::function<uint16_t(uint32_t)>& read16,
                            const std::function<void(uint32_t, uint16_t)>& write16);

private:
    void EnsureSize(uint32_t addr_bits);

    std::vector<uint8_t> data_;  // 8 bytes per EEPROM block.
    uint8_t addr_bits_ = 6;      // 6=512B EEPROM, 14=8KB EEPROM.
    bool read_pending_ = false;
    bool read_valid_ = false;
    uint16_t read_block_ = 0;
    mutable uint8_t busy_polls_ = 0;
};
