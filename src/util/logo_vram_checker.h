#pragma once

#include <cstdint>
#include <string>
#include <vector>

class MemoryBus;

struct LogoVramExpectation {
    std::string region_name;
    std::string asset_file;
    uint32_t vram_offset = 0;
    uint32_t size = 0;
    bool requires_rom = false;
    std::vector<uint8_t> expected;
};

class LogoVramChecker {
  public:
    bool Load(const std::string& expected_csv_path, const std::string& asset_dir);
    void VerifyFrame(const MemoryBus& bus, int frame_number) const;
    bool IsLoaded() const { return loaded_; }

  private:
    bool loaded_ = false;
    std::vector<LogoVramExpectation> expectations_;
};
