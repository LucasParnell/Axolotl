#include "system/input/input_mapping_window.h"

#include <optional>

#include <QHeaderView>
#include <QHBoxLayout>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "system/config/config_reader.h"
#include "system/input/input_mapping_system.h"

InputMappingWindow::InputMappingWindow(InputConfigData* config,
                                       ConfigReader* reader,
                                       InputMappingSystem* input_system,
                                       QWidget* parent)
    : QWidget(parent), config_(config), reader_(reader), input_system_(input_system) {
    setMinimumSize(760, 560);

    auto* root = new QVBoxLayout(this);

    auto* actions = new QHBoxLayout();
    save_button_ = new QPushButton("Save", this);
    reload_button_ = new QPushButton("Reload", this);
    actions->addStretch(1);
    actions->addWidget(save_button_);
    actions->addWidget(reload_button_);
    root->addLayout(actions);

    auto* top = new QHBoxLayout();
    device_combo_ = new QComboBox(this);
    top->addWidget(new QLabel("Input Device/Profile:", this));
    top->addWidget(device_combo_);
    root->addLayout(top);

    instructions_label_ = new QLabel(
        "F11 toggles fullscreen. F12 toggles this window. "
        "To bind a key, click Bind and then press the key in the emulator window "
        "or press a gamepad control.",
        this);
    instructions_label_->setWordWrap(true);
    root->addWidget(instructions_label_);

    status_label_ = new QLabel(this);
    root->addWidget(status_label_);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({"GBA Button", "Binding", "Bind", "Clear"});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    root->addWidget(table_);

    connect(save_button_, &QPushButton::clicked, this, [this]() { SaveConfig(); });
    connect(reload_button_, &QPushButton::clicked, this, [this]() { ReloadConfig(); });
    connect(device_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (is_updating_combo_ || !config_ || idx < 0) return;
        config_->active_profile = device_combo_->itemData(idx).toString().toStdString();
        MarkDirty();
        RebuildTable();
        UpdateStatus();
    });

    RebuildDeviceCombo();
    RebuildTable();
    UpdateStatus();
    UpdateWindowTitle();
}

void InputMappingWindow::RebuildDeviceCombo() {
    if (!config_) return;

    is_updating_combo_ = true;
    device_combo_->clear();
    for (const auto& profile : config_->profiles) {
        const QString label =
            QString::fromStdString(profile.name + " [" + profile.profile_id + "]");
        device_combo_->addItem(label, QString::fromStdString(profile.profile_id));
    }

    int idx = device_combo_->findData(QString::fromStdString(config_->active_profile));
    if (idx < 0 && device_combo_->count() > 0) idx = 0;
    if (idx >= 0) {
        device_combo_->setCurrentIndex(idx);
        config_->active_profile = device_combo_->currentData().toString().toStdString();
    }
    is_updating_combo_ = false;
}

void InputMappingWindow::RebuildTable() {
    table_->setRowCount(static_cast<int>(kGbaButtonCount));
    const DeviceProfile* profile = ActiveProfile();

    for (size_t i = 0; i < kGbaButtonCount; ++i) {
        const GbaButton button = static_cast<GbaButton>(i);
        table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(GbaButtonName(button)));

        std::string binding_text = "none";
        if (profile && input_system_) {
            binding_text = input_system_->BindingDisplayText(profile->bindings[i]);
        }
        table_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(QString::fromStdString(binding_text)));

        auto* bind_button = new QPushButton("Bind", table_);
        connect(bind_button, &QPushButton::clicked, this, [this, i]() {
            capture_row_ = static_cast<int>(i);
            UpdateStatus();
        });
        table_->setCellWidget(static_cast<int>(i), 2, bind_button);

        auto* clear_button = new QPushButton("Clear", table_);
        connect(clear_button, &QPushButton::clicked, this, [this, i]() {
            DeviceProfile* profile_mut = ActiveProfile();
            if (!profile_mut) return;
            profile_mut->bindings[i] = InputBinding{};
            MarkDirty();
            RebuildTable();
            UpdateStatus();
        });
        table_->setCellWidget(static_cast<int>(i), 3, clear_button);
    }
}

void InputMappingWindow::UpdateStatus() {
    if (!config_ || !input_system_) return;

    QString status = QString::fromStdString(input_system_->ActiveProfileStatus(*config_));
    if (capture_row_ >= 0) {
        status += QString(" | Capture mode for ") +
                  QString::fromStdString(GbaButtonName(static_cast<GbaButton>(capture_row_))) +
                  QString(" - press a key in the emulator window or a gamepad control");
    }
    status_label_->setText(status);
}

void InputMappingWindow::UpdateWindowTitle() {
    if (!config_) return;
    setWindowTitle(QString("Input Mapper") + (config_->dirty ? "*" : ""));
}

DeviceProfile* InputMappingWindow::ActiveProfile() {
    if (!config_) return nullptr;
    return FindProfile(config_, config_->active_profile);
}

const DeviceProfile* InputMappingWindow::ActiveProfile() const {
    if (!config_) return nullptr;
    return FindProfile(*config_, config_->active_profile);
}

void InputMappingWindow::SaveConfig() {
    if (!config_ || !reader_) return;
    if (reader_->Save(*config_)) {
        config_->dirty = false;
    }
    UpdateWindowTitle();
}

void InputMappingWindow::ReloadConfig() {
    if (!config_ || !reader_) return;
    InputConfigData loaded{};
    if (!reader_->Load(&loaded)) return;

    *config_ = std::move(loaded);
    capture_row_ = -1;
    RebuildDeviceCombo();
    RebuildTable();
    UpdateStatus();
    UpdateWindowTitle();
}

void InputMappingWindow::MarkDirty() {
    if (!config_) return;
    config_->dirty = true;
    UpdateWindowTitle();
}

void InputMappingWindow::Tick() {
    if (!config_ || !input_system_) return;

    input_system_->RefreshDevices();
    input_system_->SyncProfilesWithDevices(config_);

    if (capture_row_ >= 0) {
        const std::optional<InputBinding> captured = input_system_->PollCaptureBinding();
        if (captured.has_value()) {
            DeviceProfile* profile = ActiveProfile();
            if (profile) {
                profile->bindings[static_cast<size_t>(capture_row_)] = *captured;
                MarkDirty();
            }
            capture_row_ = -1;
            RebuildTable();
        }
    }

    const QString selected = device_combo_->currentData().toString();
    const QString active = QString::fromStdString(config_->active_profile);
    if (selected != active || device_combo_->count() != static_cast<int>(config_->profiles.size())) {
        RebuildDeviceCombo();
    }

    UpdateStatus();
}
