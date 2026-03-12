#include "system/config/config_reader.h"

#include <cstdlib>

#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <GLFW/glfw3.h>

namespace {

QString ToQString(const std::string& value) {
    return QString::fromStdString(value);
}

std::string ToStdString(const QString& value) {
    return value.toStdString();
}

InputBinding ParseBindingToken(const std::string& token) {
    InputBinding binding{};
    if (token.empty() || token == "none") return binding;

    if (token.rfind("key:", 0) == 0) {
        const int code = std::atoi(token.c_str() + 4);
        if (code >= 0) {
            binding.kind = BindingKind::kKeyboardKey;
            binding.code = code;
        }
        return binding;
    }

    if (token.rfind("gbtn:", 0) == 0) {
        const int code = std::atoi(token.c_str() + 5);
        if (code >= 0) {
            binding.kind = BindingKind::kGamepadButton;
            binding.code = code;
        }
        return binding;
    }

    if (token.rfind("gaxis:", 0) == 0) {
        const std::string body = token.substr(6);
        const size_t split = body.find(':');
        if (split == std::string::npos) return binding;

        const int axis = std::atoi(body.substr(0, split).c_str());
        const std::string direction = body.substr(split + 1);
        if (axis < 0) return binding;
        if (direction != "pos" && direction != "neg") return binding;

        binding.kind = BindingKind::kGamepadAxis;
        binding.code = axis;
        binding.axis_direction =
            (direction == "neg") ? AxisDirection::kNegative : AxisDirection::kPositive;
    }

    return binding;
}

std::string BindingToken(const InputBinding& binding) {
    switch (binding.kind) {
        case BindingKind::kNone:
            return "none";
        case BindingKind::kKeyboardKey:
            return "key:" + std::to_string(binding.code);
        case BindingKind::kGamepadButton:
            return "gbtn:" + std::to_string(binding.code);
        case BindingKind::kGamepadAxis:
            return std::string("gaxis:") + std::to_string(binding.code) +
                   (binding.axis_direction == AxisDirection::kNegative ? ":neg" : ":pos");
    }
    return "none";
}

bool AreAllBindingsNone(const DeviceProfile& profile) {
    for (const InputBinding& binding : profile.bindings) {
        if (binding.kind != BindingKind::kNone) return false;
    }
    return true;
}

void ApplyDefaultKeyboardBindings(DeviceProfile* profile) {
    if (!profile) return;
    profile->bindings[static_cast<size_t>(GbaButton::kA)] = {BindingKind::kKeyboardKey, GLFW_KEY_Z, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kB)] = {BindingKind::kKeyboardKey, GLFW_KEY_X, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kSelect)] = {BindingKind::kKeyboardKey, GLFW_KEY_RIGHT_SHIFT, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kStart)] = {BindingKind::kKeyboardKey, GLFW_KEY_ENTER, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kRight)] = {BindingKind::kKeyboardKey, GLFW_KEY_RIGHT, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kLeft)] = {BindingKind::kKeyboardKey, GLFW_KEY_LEFT, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kUp)] = {BindingKind::kKeyboardKey, GLFW_KEY_UP, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kDown)] = {BindingKind::kKeyboardKey, GLFW_KEY_DOWN, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kR)] = {BindingKind::kKeyboardKey, GLFW_KEY_S, AxisDirection::kPositive};
    profile->bindings[static_cast<size_t>(GbaButton::kL)] = {BindingKind::kKeyboardKey, GLFW_KEY_A, AxisDirection::kPositive};
    if (profile->kind.empty()) profile->kind = "keyboard";
    if (profile->name.empty()) profile->name = "Keyboard";
}

}  // namespace

ConfigReader::ConfigReader(std::string path) : path_(std::move(path)) {}

DeviceProfile ConfigReader::DefaultKeyboardProfile() {
    DeviceProfile profile{};
    profile.profile_id = "keyboard";
    profile.kind = "keyboard";
    profile.name = "Keyboard";
    ApplyDefaultKeyboardBindings(&profile);
    return profile;
}

void ConfigReader::EnsureInputDefaults(InputConfigData* input) {
    if (!input) return;

    if (input->profiles.empty()) {
        input->profiles.push_back(DefaultKeyboardProfile());
    }
    if (!FindProfile(*input, "keyboard")) {
        input->profiles.push_back(DefaultKeyboardProfile());
    }
    if (input->active_profile.empty() || !FindProfile(*input, input->active_profile)) {
        input->active_profile = "keyboard";
    }
    if (!FindProfile(*input, input->active_profile) && !input->profiles.empty()) {
        input->active_profile = input->profiles.front().profile_id;
    }
}

bool ConfigReader::Load(InputConfigData* out) const {
    if (!out) return false;

    InputConfigData cfg{};
    QSettings settings(ToQString(path_), QSettings::IniFormat);

    settings.beginGroup("Input");
    cfg.version = settings.value("version", 1u).toUInt();
    cfg.active_profile = ToStdString(settings.value("active_profile", "keyboard").toString());

    QStringList profile_list;
    const QVariant profiles_value = settings.value("profiles", "");
    if (profiles_value.metaType().id() == QMetaType::QStringList) {
        profile_list = profiles_value.toStringList();
    } else {
        const QString profiles_csv = profiles_value.toString();
        for (const QString& token : profiles_csv.split(',', Qt::SkipEmptyParts)) {
            profile_list.push_back(token.trimmed());
        }
    }
    settings.endGroup();

    cfg.profiles.clear();
    for (const QString& profile_id_q : profile_list) {
        const std::string profile_id = ToStdString(profile_id_q);
        if (profile_id.empty()) continue;

        DeviceProfile profile{};
        profile.profile_id = profile_id;
        settings.beginGroup(ToQString("Profile." + profile_id));
        profile.kind = ToStdString(settings.value("kind", "keyboard").toString());
        profile.name = ToStdString(settings.value("name", ToQString(profile_id)).toString());
        profile.guid = ToStdString(settings.value("guid", "").toString());
        for (size_t i = 0; i < kGbaButtonCount; ++i) {
            const std::string token =
                ToStdString(settings.value(GbaButtonName(static_cast<GbaButton>(i)), "none").toString());
            profile.bindings[i] = ParseBindingToken(token);
        }
        settings.endGroup();

        const bool is_keyboard_profile =
            (profile.kind == "keyboard") || (profile.profile_id == "keyboard");
        if (is_keyboard_profile && AreAllBindingsNone(profile)) {
            ApplyDefaultKeyboardBindings(&profile);
        }

        cfg.profiles.push_back(std::move(profile));
    }

    EnsureInputDefaults(&cfg);
    cfg.dirty = false;
    *out = std::move(cfg);
    return true;
}

bool ConfigReader::Save(const InputConfigData& cfg) const {
    QSettings settings(ToQString(path_), QSettings::IniFormat);

    for (const QString& group : settings.childGroups()) {
        if (group.startsWith("Profile.")) {
            settings.remove(group);
        }
    }

    settings.beginGroup("Input");
    settings.setValue("version", cfg.version);
    settings.setValue("active_profile", ToQString(cfg.active_profile));
    QStringList profiles;
    profiles.reserve(static_cast<qsizetype>(cfg.profiles.size()));
    for (const auto& profile : cfg.profiles) {
        profiles.push_back(ToQString(profile.profile_id));
    }
    settings.setValue("profiles", profiles);
    settings.endGroup();

    for (const auto& profile : cfg.profiles) {
        settings.beginGroup(ToQString("Profile." + profile.profile_id));
        settings.setValue("kind", ToQString(profile.kind));
        settings.setValue("name", ToQString(profile.name));
        settings.setValue("guid", ToQString(profile.guid));
        for (size_t i = 0; i < kGbaButtonCount; ++i) {
            settings.setValue(GbaButtonName(static_cast<GbaButton>(i)),
                              ToQString(BindingToken(profile.bindings[i])));
        }
        settings.endGroup();
    }

    settings.sync();
    return settings.status() == QSettings::NoError;
}
