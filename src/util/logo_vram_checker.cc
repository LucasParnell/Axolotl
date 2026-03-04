#include "util/logo_vram_checker.h"

#include "system/memory_bus.h"
#include "util/logger.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

static bool ParseHexOrDec(const std::string& s, uint32_t* out) {
    if (!out) return false;
    const char* begin = s.c_str();
    char* end = nullptr;
    unsigned long long v = std::strtoull(begin, &end, 0);
    if (end == begin || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

static std::string Trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

static bool ParseBool(const std::string& s, bool* out) {
    if (!out) return false;
    const std::string t = Trim(s);
    if (t == "1" || t == "true" || t == "TRUE" || t == "yes" || t == "YES") {
        *out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "FALSE" || t == "no" || t == "NO" || t.empty()) {
        *out = false;
        return true;
    }
    return false;
}

static std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : line) {
        if (ch == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

bool LogoVramChecker::Load(const std::string& expected_csv_path, const std::string& asset_dir) {
    expectations_.clear();
    loaded_ = false;

    std::ifstream f(expected_csv_path);
    if (!f.is_open()) {
        Logger::log("[LogoVram] expected CSV not found: " + expected_csv_path, LogLevel::WARNING);
        return false;
    }

    std::string line;
    bool first = true;
    size_t line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        if (first) {
            first = false;
            continue;  // header
        }

        auto cols = SplitCsvLine(line);
        if (cols.size() < 4) continue;
        for (auto& c : cols) c = Trim(c);

        LogoVramExpectation e;
        e.region_name = cols[0];
        e.asset_file = cols[1];
        if (!ParseHexOrDec(cols[2], &e.vram_offset) || !ParseHexOrDec(cols[3], &e.size)) {
            std::ostringstream msg;
            msg << "[LogoVram] bad numeric field in " << expected_csv_path << ":" << line_no;
            Logger::log(msg.str(), LogLevel::WARNING);
            continue;
        }
        if (cols.size() >= 5) {
            bool requires_rom = false;
            if (ParseBool(cols[4], &requires_rom)) e.requires_rom = requires_rom;
        }

        std::ifstream bin(asset_dir + "/" + e.asset_file, std::ios::binary);
        if (!bin.is_open()) {
            Logger::log("[LogoVram] missing asset: " + asset_dir + "/" + e.asset_file, LogLevel::WARNING);
            continue;
        }
        e.expected.assign(std::istreambuf_iterator<char>(bin), std::istreambuf_iterator<char>());
        if (e.expected.empty()) continue;
        if (e.size == 0) e.size = static_cast<uint32_t>(e.expected.size());
        if (e.size > e.expected.size()) e.size = static_cast<uint32_t>(e.expected.size());

        expectations_.push_back(std::move(e));
    }

    loaded_ = !expectations_.empty();
    if (loaded_) {
        Logger::log("[LogoVram] loaded " + std::to_string(expectations_.size()) + " VRAM expectations", LogLevel::INFO);
    } else {
        Logger::log("[LogoVram] no usable VRAM expectations loaded", LogLevel::WARNING);
    }
    return loaded_;
}

void LogoVramChecker::VerifyFrame(const MemoryBus& bus, int frame_number) const {
    if (!loaded_) return;
    const uint8_t* vram = bus.GetVramPtr();
    const size_t vram_size = bus.GetVramSize();
    if (!vram || vram_size == 0) return;

    for (const auto& e : expectations_) {
        if (e.requires_rom && !bus.HasRomLoaded()) {
            std::ostringstream msg;
            msg << "[LogoVram] frame=" << frame_number
                << " region=" << e.region_name
                << " skipped(no ROM loaded)";
            Logger::log(msg.str(), LogLevel::INFO);
            continue;
        }
        if (e.vram_offset >= vram_size) continue;
        size_t n = std::min<size_t>(e.size, vram_size - e.vram_offset);
        n = std::min<size_t>(n, e.expected.size());
        if (n == 0) continue;

        size_t mismatch = 0;
        size_t first_bad = static_cast<size_t>(-1);
        uint8_t first_exp = 0;
        uint8_t first_got = 0;
        for (size_t i = 0; i < n; ++i) {
            uint8_t got = vram[e.vram_offset + i];
            uint8_t exp = e.expected[i];
            if (got != exp) {
                if (first_bad == static_cast<size_t>(-1)) {
                    first_bad = i;
                    first_exp = exp;
                    first_got = got;
                }
                ++mismatch;
            }
        }

        std::ostringstream msg;
        msg << "[LogoVram] frame=" << frame_number
            << " region=" << e.region_name
            << " off=0x" << std::hex << e.vram_offset
            << " size=0x" << n
            << std::dec << " mismatches=" << mismatch;
        if (first_bad != static_cast<size_t>(-1)) {
            msg << " first_bad=+0x" << std::hex << first_bad
                << " exp=0x" << static_cast<unsigned>(first_exp)
                << " got=0x" << static_cast<unsigned>(first_got);
        }
        Logger::log(msg.str(), mismatch ? LogLevel::WARNING : LogLevel::INFO);
    }
}
