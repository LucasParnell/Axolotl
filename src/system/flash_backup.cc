#include "system/flash_backup.h"

#include <algorithm>
#include <cstring>

void FlashBackup::Reset() {
    id_mode_ = false;
    unlock_step_ = 0;
    pending_command_ = PendingCommand::kNone;
    bank_ = 0;
}

uint8_t FlashBackup::ManufacturerId(size_t backing_size) const {
    // Common IDs used by Nintendo flash libraries.
    return (backing_size > 0x10000u) ? 0x62u : 0x32u;
}

uint8_t FlashBackup::DeviceId(size_t backing_size) const {
    // 128KB (FLASH1M): Sanyo-style 0x13. 64KB (FLASH512): Panasonic-style 0x1B.
    return (backing_size > 0x10000u) ? 0x13u : 0x1Bu;
}

size_t FlashBackup::EffectiveBankOffset(size_t backing_size) const {
    if (backing_size <= 0x10000u) return 0;
    const size_t bank_count = std::max<size_t>(1u, backing_size / 0x10000u);
    return static_cast<size_t>(bank_ % bank_count) * 0x10000u;
}

void FlashBackup::ClearCommandState() {
    unlock_step_ = 0;
    pending_command_ = PendingCommand::kNone;
}

uint8_t FlashBackup::Read8(uint32_t addr, const uint8_t* backing, size_t backing_size) const {
    if (!backing || backing_size == 0) return 0xFF;
    const size_t off = static_cast<size_t>(addr & 0xFFFFu);

    if (id_mode_) {
        if (off == 0x0000u) return ManufacturerId(backing_size);
        if (off == 0x0001u) return DeviceId(backing_size);
        return 0xFF;
    }

    const size_t bank_off = EffectiveBankOffset(backing_size);
    const size_t index = std::min(backing_size - 1u, bank_off + off);
    return backing[index];
}

uint16_t FlashBackup::Read16(uint32_t addr, const uint8_t* backing, size_t backing_size) const {
    const uint8_t b = Read8(addr & ~1u, backing, backing_size);
    return static_cast<uint16_t>(b) | (static_cast<uint16_t>(b) << 8);
}

uint32_t FlashBackup::Read32(uint32_t addr, const uint8_t* backing, size_t backing_size) const {
    const uint8_t b = Read8(addr & ~3u, backing, backing_size);
    return static_cast<uint32_t>(b) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(b) << 24);
}

bool FlashBackup::Write8(uint32_t addr, uint8_t value, uint8_t* backing, size_t backing_size) {
    if (!backing || backing_size == 0) return true;

    const uint32_t off = addr & 0xFFFFu;

    // Reset command: accepted at any address by many chips.
    if (value == 0xF0u) {
        id_mode_ = false;
        ClearCommandState();
        return true;
    }

    if (pending_command_ == PendingCommand::kWriteByte) {
        const size_t bank_off = EffectiveBankOffset(backing_size);
        const size_t index = std::min(backing_size - 1u, bank_off + static_cast<size_t>(off));
        backing[index] = value;
        ClearCommandState();
        return true;
    }

    if (pending_command_ == PendingCommand::kSelectBank) {
        bank_ = static_cast<uint8_t>(value & 0x01u);
        ClearCommandState();
        return true;
    }

    // Erase sequence: AA 55 80, then AA 55, then 10(chip) or 30(sector).
    if (pending_command_ == PendingCommand::kErase) {
        if (unlock_step_ == 0 && off == 0x5555u && value == 0xAAu) {
            unlock_step_ = 1;
            return true;
        }
        if (unlock_step_ == 1 && off == 0x2AAAu && value == 0x55u) {
            unlock_step_ = 2;
            return true;
        }
        if (unlock_step_ == 2) {
            if (off == 0x5555u && value == 0x10u) {
                std::memset(backing, 0xFF, backing_size);
                ClearCommandState();
                return true;
            }
            if (value == 0x30u) {
                const size_t bank_off = EffectiveBankOffset(backing_size);
                const size_t sector_base = (static_cast<size_t>(off) & ~size_t(0x0FFFu));
                const size_t start = std::min(backing_size, bank_off + sector_base);
                const size_t end = std::min(backing_size, start + 0x1000u);
                std::fill(backing + start, backing + end, 0xFFu);
                ClearCommandState();
                return true;
            }
        }
        ClearCommandState();
        return true;
    }

    // Unlock sequence preamble.
    if (unlock_step_ == 0) {
        if (off == 0x5555u && value == 0xAAu) {
            unlock_step_ = 1;
            return true;
        }
        return true;  // Ignore non-command writes in read mode.
    }
    if (unlock_step_ == 1) {
        if (off == 0x2AAAu && value == 0x55u) {
            unlock_step_ = 2;
            return true;
        }
        ClearCommandState();
        return true;
    }

    // unlock_step_ == 2: command byte at 0x5555.
    if (off == 0x5555u) {
        switch (value) {
            case 0x90u:
                id_mode_ = true;
                ClearCommandState();
                return true;
            case 0xA0u:
                pending_command_ = PendingCommand::kWriteByte;
                unlock_step_ = 0;
                return true;
            case 0x80u:
                pending_command_ = PendingCommand::kErase;
                unlock_step_ = 0;
                return true;
            case 0xB0u:
                pending_command_ = PendingCommand::kSelectBank;
                unlock_step_ = 0;
                return true;
            default:
                ClearCommandState();
                return true;
        }
    }

    ClearCommandState();
    return true;
}
