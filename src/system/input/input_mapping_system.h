#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLFW/glfw3.h>

#include "data/input_mapping_data.h"

class GlDisplay;
class MemoryBus;

class InputMappingSystem {
 public:
    struct RuntimeDevice {
        std::string profile_id;
        std::string kind;
        std::string name;
        std::string guid;
        int joystick_id = -1;
        bool connected = false;
    };

    explicit InputMappingSystem(GlDisplay* display);

    void RefreshDevices();
    void SyncProfilesWithDevices(InputConfigData* input);
    void PollAndApply(const InputConfigData& input, MemoryBus* bus);

    const std::vector<RuntimeDevice>& Devices() const { return devices_; }

    std::optional<InputBinding> PollCaptureBinding();
    bool IsActiveProfileConnected(const InputConfigData& input) const;
    std::string ActiveProfileStatus(const InputConfigData& input) const;
    std::string BindingDisplayText(const InputBinding& binding) const;

 private:
    struct JoystickState {
        std::array<unsigned char, GLFW_GAMEPAD_BUTTON_LAST + 1> buttons{};
        std::array<float, GLFW_GAMEPAD_AXIS_LAST + 1> axes{};
        bool valid = false;
    };

    const RuntimeDevice* ResolveActiveDevice(const InputConfigData& input) const;
    bool BindingPressed(const InputBinding& binding, const RuntimeDevice* device) const;
    void SnapshotJoystickStates();

    GlDisplay* display_ = nullptr;
    GLFWwindow* window_ = nullptr;
    std::vector<RuntimeDevice> devices_;
    std::unordered_map<int, JoystickState> prev_joystick_state_;
    std::array<unsigned char, GLFW_KEY_LAST + 1> prev_key_state_{};

    static constexpr float kAxisPressedThreshold = 0.5f;
    static constexpr float kCaptureAxisThreshold = 0.75f;
};
