#include "system/eeprom_backup.h"

#include <algorithm>

bool EepromBackup::IsAddress(uint32_t addr) {
    return (addr >> 24) == 0x0D;
}

void EepromBackup::Reset() {
    addr_bits_ = 6;
    read_pending_ = false;
    read_valid_ = false;
    read_block_ = 0;
    busy_polls_ = 0;
    data_.clear();
}

void EepromBackup::InitializeDefaultStorage() {
    if (data_.empty()) data_.resize(512, 0xFF);
}

uint32_t EepromBackup::ReadReady32() const {
    const uint32_t ready = (busy_polls_ == 0) ? 1u : 0u;
    if (busy_polls_ > 0) --busy_polls_;
    return (ready << 16) | ready;
}

uint16_t EepromBackup::ReadReady16() const {
    const uint16_t ready = (busy_polls_ == 0) ? 0x0001u : 0x0000u;
    if (busy_polls_ > 0) --busy_polls_;
    return ready;
}

uint8_t EepromBackup::ReadReady8() const {
    const uint8_t ready = (busy_polls_ == 0) ? 0x01u : 0x00u;
    if (busy_polls_ > 0) --busy_polls_;
    return ready;
}

void EepromBackup::EnsureSize(uint32_t addr_bits) {
    addr_bits = std::clamp(addr_bits, 6u, 14u);
    addr_bits_ = static_cast<uint8_t>(addr_bits);
    const size_t target_size = static_cast<size_t>(8u) << addr_bits;
    if (data_.size() != target_size) data_.resize(target_size, 0xFF);
}

bool EepromBackup::HandleDma3Transfer(bool is_32bit,
                                      uint32_t count,
                                      int src_step,
                                      int dst_step,
                                      uint32_t& src,
                                      uint32_t& dst,
                                      const std::function<uint16_t(uint32_t)>& read16,
                                      const std::function<void(uint32_t, uint16_t)>& write16) {
    if (is_32bit) return false;

    const bool src_is_eeprom = IsAddress(src);
    const bool dst_is_eeprom = IsAddress(dst);
    if (!src_is_eeprom && !dst_is_eeprom) return false;

    auto read_addr_bits = [](const std::vector<uint8_t>& bits, size_t start, uint32_t n) -> uint32_t {
        uint32_t out = 0;
        for (uint32_t i = 0; i < n; ++i) {
            out = (out << 1) | (bits[start + i] & 1u);
        }
        return out;
    };

    EnsureSize(addr_bits_);

    if (dst_is_eeprom && !src_is_eeprom) {
        std::vector<uint8_t> bits;
        bits.reserve(count);

        uint32_t src_cur = src;
        for (uint32_t i = 0; i < count; ++i) {
            bits.push_back(static_cast<uint8_t>(read16(src_cur) & 1u));
            src_cur += src_step;
        }

        if (bits.size() >= 3 && bits[0] == 1u) {
            if (bits[1] == 1u) {
                // Read command: "11 + addr + 0" (9 or 17 halfwords)
                const uint32_t addr_bits = static_cast<uint32_t>(bits.size() - 3u);
                if ((addr_bits == 6u || addr_bits == 14u) && bits.back() == 0u) {
                    EnsureSize(addr_bits);
                    const uint32_t addr = read_addr_bits(bits, 2, addr_bits);
                    const uint32_t blocks = static_cast<uint32_t>(data_.size() / 8u);
                    read_block_ = static_cast<uint16_t>(addr % std::max(1u, blocks));
                    read_pending_ = true;
                    read_valid_ = true;
                }
            } else {
                // Write command: "10 + addr + 64 data bits + 0" (73 or 81 halfwords)
                if (bits.size() >= 67u) {
                    const uint32_t addr_bits = static_cast<uint32_t>(bits.size() - 67u);
                    if ((addr_bits == 6u || addr_bits == 14u) && bits.back() == 0u) {
                        EnsureSize(addr_bits);
                        const uint32_t addr = read_addr_bits(bits, 2, addr_bits);
                        const uint32_t blocks = static_cast<uint32_t>(data_.size() / 8u);
                        const size_t base = static_cast<size_t>(addr % std::max(1u, blocks)) * 8u;
                        for (size_t i = 0; i < 8; ++i) {
                            uint8_t out = 0;
                            for (size_t b = 0; b < 8; ++b) {
                                const size_t bit_index = 2u + addr_bits + i * 8u + b;
                                if (bit_index >= bits.size()) break;
                                out = static_cast<uint8_t>((out << 1) | (bits[bit_index] & 1u));
                            }
                            data_[base + i] = out;
                        }
                        read_pending_ = false;
                        read_valid_ = false;
                        busy_polls_ = 8;
                    }
                }
            }
        }

        src = src_cur;
        dst += static_cast<uint32_t>(dst_step * static_cast<int32_t>(count));
        return true;
    }

    if (src_is_eeprom && !dst_is_eeprom) {
        // EEPROM -> CPU read stream: 4 dummy bits then 64 data bits.
        uint32_t dst_cur = dst;
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t bit = 1;
            if (read_valid_) {
                if (i < 4u) {
                    bit = 0;
                } else if (i < 68u && !data_.empty()) {
                    const size_t blocks = data_.size() / 8u;
                    const size_t block = static_cast<size_t>(read_block_) % std::max<size_t>(1u, blocks);
                    const size_t base = block * 8u;
                    const size_t data_bit = static_cast<size_t>(i - 4u);
                    const uint8_t byte = data_[base + (data_bit >> 3)];
                    bit = static_cast<uint16_t>((byte >> (7u - (data_bit & 7u))) & 1u);
                }
            }
            write16(dst_cur, bit);
            dst_cur += dst_step;
        }

        if (read_pending_ && count >= 68u) read_pending_ = false;

        src += static_cast<uint32_t>(src_step * static_cast<int32_t>(count));
        dst = dst_cur;
        return true;
    }

    return false;
}
