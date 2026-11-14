// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "controller.hpp"
#include "device_config.h"
#include "radio_settings.h"
#include "brightness_control.h"
#include "meter_scale.h"
#include <ArduinoJson.h>
namespace app {
struct Config {
    uint32_t schema = 1, revision = 1;
    uint32_t sectionRevision[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    DeviceConfig device{};
    ldg::AntennaSettings antennas{};
    ldg::TuningSettings tuning{};
    ldg::RadioSettings radio{};
    ldg::BrightnessSettings brightness{};
    ldg::MeterSettings meter{};
    ldg::RouteSettings routes{};
    bool mqttEnabled = false, remoteEnabled = false, simulateSwitch = false;
    int8_t uartRx = -1, uartTx = -1;
    uint8_t displayView = 0; // Legacy schema-1 byte; ignored by the numeric display.
};
void settingsInit();
Config settings();
bool trySettings(Config &);
ldg::Result saveSettings(const char *section, JsonVariantConst input, uint32_t expected,
                         bool &restart);
bool persistManaged(bool value);
void readSettings(const char *section, JsonDocument &out);
} // namespace app
