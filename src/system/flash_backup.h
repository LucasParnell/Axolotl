#pragma once

#include <cstddef>
#include <cstdint>

class FlashBackup {
public:
    void Reset();

    uint8_t Read8(uint32_t addr, const uint8_t* backing, size_t backing_size) const;
    uint16_t Read16(uint32_t addr, const uint8_t* backing, size_t backing_size) const;
    uint32_t Read32(uint32_t addr, const uint8_t* backing, size_t backing_size) const;

    bool Write8(uint32_t addr, uint8_t value, uint8_t* backing, size_t backing_size);

private:
    enum class PendingCommand : uint8_t {
        kNone = 0,
        kWriteByte,
        kErase,
        kSelectBank,
    };

    uint8_t ManufacturerId(size_t backing_size) const;
    uint8_t DeviceId(size_t backing_size) const;
    size_t EffectiveBankOffset(size_t backing_size) const;
    void ClearCommandState();

    bool id_mode_ = false;
    uint8_t unlock_step_ = 0;
    PendingCommand pending_command_ = PendingCommand::kNone;
    uint8_t bank_ = 0;
};
