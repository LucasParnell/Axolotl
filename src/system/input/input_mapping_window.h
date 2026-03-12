#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>

#include "data/input_mapping_data.h"

class ConfigReader;
class InputMappingSystem;

class InputMappingWindow : public QWidget {
 public:
    InputMappingWindow(InputConfigData* config,
                       ConfigReader* reader,
                       InputMappingSystem* input_system,
                       QWidget* parent = nullptr);

    void Tick();

 private:
    void RebuildDeviceCombo();
    void RebuildTable();
    void UpdateStatus();
    void UpdateWindowTitle();
    DeviceProfile* ActiveProfile();
    const DeviceProfile* ActiveProfile() const;
    void SaveConfig();
    void ReloadConfig();
    void MarkDirty();

    InputConfigData* config_ = nullptr;
    ConfigReader* reader_ = nullptr;
    InputMappingSystem* input_system_ = nullptr;

    QComboBox* device_combo_ = nullptr;
    QLabel* instructions_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* reload_button_ = nullptr;

    bool is_updating_combo_ = false;
    int capture_row_ = -1;
};
