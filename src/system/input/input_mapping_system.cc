#include "system/input/input_mapping_system.h"

#include <array>
#include <cstdlib>
#include <cstring>

#include "system/display.h"
#include "system/memory_bus.h"

namespace {

std::string FormatKeyCode(int key) {
    switch (key) {
        case GLFW_KEY_SPACE: return "Space";
        case GLFW_KEY_APOSTROPHE: return "'";
        case GLFW_KEY_COMMA: return ",";
        case GLFW_KEY_MINUS: return "-";
        case GLFW_KEY_PERIOD: return ".";
        case GLFW_KEY_SLASH: return "/";
        case GLFW_KEY_SEMICOLON: return ";";
        case GLFW_KEY_EQUAL: return "=";
        case GLFW_KEY_LEFT_BRACKET: return "[";
        case GLFW_KEY_BACKSLASH: return "\\";
        case GLFW_KEY_RIGHT_BRACKET: return "]";
        case GLFW_KEY_GRAVE_ACCENT: return "`";
        case GLFW_KEY_ESCAPE: return "Esc";
        case GLFW_KEY_ENTER: return "Enter";
        case GLFW_KEY_TAB: return "Tab";
        case GLFW_KEY_BACKSPACE: return "Backspace";
        case GLFW_KEY_INSERT: return "Insert";
        case GLFW_KEY_DELETE: return "Delete";
        case GLFW_KEY_RIGHT: return "Right";
        case GLFW_KEY_LEFT: return "Left";
        case GLFW_KEY_DOWN: return "Down";
        case GLFW_KEY_UP: return "Up";
        case GLFW_KEY_PAGE_UP: return "Page Up";
        case GLFW_KEY_PAGE_DOWN: return "Page Down";
        case GLFW_KEY_HOME: return "Home";
        case GLFW_KEY_END: return "End";
        case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
        case GLFW_KEY_SCROLL_LOCK: return "Scroll Lock";
        case GLFW_KEY_NUM_LOCK: return "Num Lock";
        case GLFW_KEY_PRINT_SCREEN: return "Print Screen";
        case GLFW_KEY_PAUSE: return "Pause";
        case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
        case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
        case GLFW_KEY_LEFT_ALT: return "Left Alt";
        case GLFW_KEY_LEFT_SUPER: return "Left Super";
        case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
        case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
        case GLFW_KEY_RIGHT_ALT: return "Right Alt";
        case GLFW_KEY_RIGHT_SUPER: return "Right Super";
        case GLFW_KEY_MENU: return "Menu";
        default: break;
    }

    const char* name = glfwGetKeyName(key, 0);
    if (name && *name) return name;

    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) {
        return "F" + std::to_string((key - GLFW_KEY_F1) + 1);
    }

    return "key:" + std::to_string(key);
}

}  // namespace

InputMappingSystem::InputMappingSystem(GlDisplay* display) : display_(display) {
    if (display_) window_ = display_->GetWindow();
    prev_key_state_.fill(GLFW_RELEASE);
    RefreshDevices();
    SnapshotJoystickStates();
}

void InputMappingSystem::RefreshDevices() {
    devices_.clear();
    devices_.push_back({"keyboard", "keyboard", "Keyboard", "", -1, true});

    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        if (!glfwJoystickPresent(jid)) continue;
        if (!glfwJoystickIsGamepad(jid)) continue;

        const char* guid = glfwGetJoystickGUID(jid);
        const char* name = glfwGetGamepadName(jid);
        RuntimeDevice device{};
        device.kind = "gamepad";
        device.guid = guid ? guid : "unknown";
        device.profile_id = "gamepad:" + device.guid;
        device.name = name ? name : device.profile_id;
        device.joystick_id = jid;
        device.connected = true;
        devices_.push_back(std::move(device));
    }
}

void InputMappingSystem::SyncProfilesWithDevices(InputConfigData* input) {
    if (!input) return;

    auto ensure_profile = [&](const RuntimeDevice& device) {
        if (FindProfile(*input, device.profile_id)) return;
        DeviceProfile profile{};
        profile.profile_id = device.profile_id;
        profile.kind = device.kind;
        profile.name = device.name;
        profile.guid = device.guid;
        input->profiles.push_back(std::move(profile));
        input->dirty = true;
    };

    for (const RuntimeDevice& device : devices_) {
        ensure_profile(device);
    }

    if (input->active_profile.empty() && !input->profiles.empty()) {
        input->active_profile = input->profiles.front().profile_id;
        input->dirty = true;
    }
}

const InputMappingSystem::RuntimeDevice* InputMappingSystem::ResolveActiveDevice(
    const InputConfigData& input) const {
    const DeviceProfile* profile = FindProfile(input, input.active_profile);
    if (!profile) return nullptr;

    for (const auto& device : devices_) {
        if (device.profile_id == profile->profile_id) return &device;
    }

    if (profile->kind == "keyboard") {
        for (const auto& device : devices_) {
            if (device.kind == "keyboard") return &device;
        }
    }

    return nullptr;
}

bool InputMappingSystem::BindingPressed(const InputBinding& binding,
                                        const RuntimeDevice* device) const {
    if (binding.kind == BindingKind::kNone || !window_) return false;

    switch (binding.kind) {
        case BindingKind::kKeyboardKey:
            if (binding.code < 0 || binding.code > GLFW_KEY_LAST) return false;
            return glfwGetKey(window_, binding.code) == GLFW_PRESS;
        case BindingKind::kGamepadButton: {
            if (!device || device->kind != "gamepad" || !device->connected) return false;
            GLFWgamepadstate state{};
            if (!glfwGetGamepadState(device->joystick_id, &state)) return false;
            if (binding.code < 0 || binding.code > GLFW_GAMEPAD_BUTTON_LAST) return false;
            return state.buttons[binding.code] == GLFW_PRESS;
        }
        case BindingKind::kGamepadAxis: {
            if (!device || device->kind != "gamepad" || !device->connected) return false;
            GLFWgamepadstate state{};
            if (!glfwGetGamepadState(device->joystick_id, &state)) return false;
            if (binding.code < 0 || binding.code > GLFW_GAMEPAD_AXIS_LAST) return false;
            const float value = state.axes[binding.code];
            if (binding.axis_direction == AxisDirection::kNegative) {
                return value <= -kAxisPressedThreshold;
            }
            return value >= kAxisPressedThreshold;
        }
        case BindingKind::kNone:
            return false;
    }
    return false;
}

void InputMappingSystem::PollAndApply(const InputConfigData& input, MemoryBus* bus) {
    if (!bus) return;

    const DeviceProfile* profile = FindProfile(input, input.active_profile);
    if (!profile) {
        bus->SetKeyInputState(0x03FFu);
        return;
    }

    const RuntimeDevice* active_device = ResolveActiveDevice(input);
    if (profile->kind == "gamepad" && (!active_device || !active_device->connected)) {
        bus->SetKeyInputState(0x03FFu);
        return;
    }

    uint16_t keyinput = 0x03FFu;
    for (size_t i = 0; i < kGbaButtonCount; ++i) {
        const GbaButton button = static_cast<GbaButton>(i);
        if (BindingPressed(profile->bindings[i], active_device)) {
            keyinput = static_cast<uint16_t>(keyinput & ~GbaButtonBitMask(button));
        }
    }

    const char* auto_keys = std::getenv("AXOLOTL_AUTOKEYS");
    if (auto_keys && std::strcmp(auto_keys, "suite_start") == 0) {
        static uint64_t frame = 0;
        ++frame;
        if (frame == 20) {
            keyinput = static_cast<uint16_t>(0x03FFu & ~GbaButtonBitMask(GbaButton::kA));
        }
    }

    bus->SetKeyInputState(keyinput);
}

void InputMappingSystem::SnapshotJoystickStates() {
    for (const auto& device : devices_) {
        if (device.kind != "gamepad") continue;

        JoystickState state{};
        GLFWgamepadstate gamepad{};
        state.valid = glfwGetGamepadState(device.joystick_id, &gamepad);
        if (state.valid) {
            for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) {
                state.buttons[i] = gamepad.buttons[i];
            }
            for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; ++i) {
                state.axes[i] = gamepad.axes[i];
            }
        }
        prev_joystick_state_[device.joystick_id] = state;
    }

    if (!window_) return;
    for (int key = 0; key <= GLFW_KEY_LAST; ++key) {
        prev_key_state_[key] = static_cast<unsigned char>(glfwGetKey(window_, key));
    }
}

std::optional<InputBinding> InputMappingSystem::PollCaptureBinding() {
    if (!window_) return std::nullopt;

    for (int key = 0; key <= GLFW_KEY_LAST; ++key) {
        const unsigned char now = static_cast<unsigned char>(glfwGetKey(window_, key));
        const bool rising = (prev_key_state_[key] != GLFW_PRESS) && (now == GLFW_PRESS);
        prev_key_state_[key] = now;
        if (rising) {
            return InputBinding{BindingKind::kKeyboardKey, key, AxisDirection::kPositive};
        }
    }

    for (const auto& device : devices_) {
        if (device.kind != "gamepad") continue;

        GLFWgamepadstate gamepad{};
        const bool ok = glfwGetGamepadState(device.joystick_id, &gamepad);
        JoystickState& prev = prev_joystick_state_[device.joystick_id];
        if (!ok) {
            prev.valid = false;
            continue;
        }

        for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) {
            const bool rising = (!prev.valid || prev.buttons[i] != GLFW_PRESS) &&
                                gamepad.buttons[i] == GLFW_PRESS;
            prev.buttons[i] = gamepad.buttons[i];
            if (rising) {
                return InputBinding{BindingKind::kGamepadButton, i, AxisDirection::kPositive};
            }
        }

        for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; ++i) {
            const float value = gamepad.axes[i];
            const float last = prev.valid ? prev.axes[i] : 0.0f;
            const bool rising_pos = (last < kCaptureAxisThreshold) && (value >= kCaptureAxisThreshold);
            const bool rising_neg = (last > -kCaptureAxisThreshold) && (value <= -kCaptureAxisThreshold);
            prev.axes[i] = value;
            if (rising_pos) {
                prev.valid = true;
                return InputBinding{BindingKind::kGamepadAxis, i, AxisDirection::kPositive};
            }
            if (rising_neg) {
                prev.valid = true;
                return InputBinding{BindingKind::kGamepadAxis, i, AxisDirection::kNegative};
            }
        }
        prev.valid = true;
    }

    return std::nullopt;
}

bool InputMappingSystem::IsActiveProfileConnected(const InputConfigData& input) const {
    const DeviceProfile* profile = FindProfile(input, input.active_profile);
    if (!profile) return false;
    if (profile->kind == "keyboard") return true;

    const RuntimeDevice* device = ResolveActiveDevice(input);
    return device && device->connected;
}

std::string InputMappingSystem::ActiveProfileStatus(const InputConfigData& input) const {
    const DeviceProfile* profile = FindProfile(input, input.active_profile);
    if (!profile) return "No active profile";
    if (profile->kind == "keyboard") return "Keyboard profile active";
    return IsActiveProfileConnected(input) ? "Gamepad connected" : "Gamepad disconnected";
}

std::string InputMappingSystem::BindingDisplayText(const InputBinding& binding) const {
    switch (binding.kind) {
        case BindingKind::kNone:
            return "none";
        case BindingKind::kKeyboardKey:
            return FormatKeyCode(binding.code);
        case BindingKind::kGamepadButton:
            return "Gamepad Button " + std::to_string(binding.code);
        case BindingKind::kGamepadAxis:
            return "Gamepad Axis " + std::to_string(binding.code) +
                   (binding.axis_direction == AxisDirection::kNegative ? " -" : " +");
    }
    return "none";
}
