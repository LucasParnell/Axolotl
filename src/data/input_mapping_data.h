#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class GbaButton : uint8_t {
    kA = 0,
    kB,
    kSelect,
    kStart,
    kRight,
    kLeft,
    kUp,
    kDown,
    kR,
    kL,
    kCount,
};

constexpr size_t kGbaButtonCount = static_cast<size_t>(GbaButton::kCount);

enum class BindingKind : uint8_t {
    kNone = 0,
    kKeyboardKey,
    kGamepadButton,
    kGamepadAxis,
};

enum class AxisDirection : uint8_t {
    kPositive = 0,
    kNegative,
};

struct InputBinding {
    BindingKind kind = BindingKind::kNone;
    int code = -1;
    AxisDirection axis_direction = AxisDirection::kPositive;
};

struct DeviceProfile {
    std::string profile_id;
    std::string kind;  // keyboard | gamepad
    std::string name;
    std::string guid;
    std::array<InputBinding, kGbaButtonCount> bindings{};
};

struct InputConfigData {
    uint32_t version = 1;
    std::string active_profile = "keyboard";
    std::vector<DeviceProfile> profiles;
    bool dirty = false;
};

inline const char* GbaButtonName(GbaButton button) {
    switch (button) {
        case GbaButton::kA: return "A";
        case GbaButton::kB: return "B";
        case GbaButton::kSelect: return "Select";
        case GbaButton::kStart: return "Start";
        case GbaButton::kRight: return "Right";
        case GbaButton::kLeft: return "Left";
        case GbaButton::kUp: return "Up";
        case GbaButton::kDown: return "Down";
        case GbaButton::kR: return "R";
        case GbaButton::kL: return "L";
        case GbaButton::kCount: break;
    }
    return "Unknown";
}

inline uint16_t GbaButtonBitMask(GbaButton button) {
    return static_cast<uint16_t>(1u << static_cast<uint8_t>(button));
}

inline DeviceProfile* FindProfile(InputConfigData* input, const std::string& profile_id) {
    if (!input) return nullptr;
    for (auto& profile : input->profiles) {
        if (profile.profile_id == profile_id) return &profile;
    }
    return nullptr;
}

inline const DeviceProfile* FindProfile(const InputConfigData& input, const std::string& profile_id) {
    for (const auto& profile : input.profiles) {
        if (profile.profile_id == profile_id) return &profile;
    }
    return nullptr;
}
