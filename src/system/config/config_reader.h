#pragma once

#include <string>

#include "data/input_mapping_data.h"

class ConfigReader {
 public:
    explicit ConfigReader(std::string path);

    bool Load(InputConfigData* out) const;
    bool Save(const InputConfigData& cfg) const;

    const std::string& Path() const { return path_; }

 private:
    static DeviceProfile DefaultKeyboardProfile();
    static void EnsureInputDefaults(InputConfigData* input);

    std::string path_;
};
