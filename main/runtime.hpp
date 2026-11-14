// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "settings.hpp"
#include "build_identity.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
namespace app {
uint32_t nowMs();
extern ldg::Controller *controller;
extern SemaphoreHandle_t controlMutex;
extern ldg::Id bootId;
extern std::atomic<bool> updating;
extern std::atomic<bool> networkUp;
extern std::atomic<bool> mqttConnected;
extern std::atomic<bool> mqttAllocated;
extern std::atomic<bool> mqttStopping;
extern std::atomic<bool> tciAllocated;
extern std::atomic<uint32_t> mqttReconnect;
extern std::atomic<int> managedToSave;
extern TaskHandle_t observedTasks[6];
extern std::atomic<uint32_t> ownerMaxGapUs, ownerMaxWorkUs;
struct Snapshot {
    tuner_state_t tuner{};
    ldg::Id target{};
    uint32_t uptime = 0, seq = 0, frames = 0, errors = 0;
    bool busy = false, remote = false;
    uint16_t scale = 1000;
    bool logarithmic = false;
    ldg::SwitchState antennaSwitch{};
    ldg::RouteReport route{};
};
Snapshot snapshot();
ldg::Result submit(const ldg::Request &, ldg::Operation &);
bool operation(const ldg::Id &, ldg::Operation &);
bool cancel(const ldg::Id &);
void radioSnapshot(const ldg::RadioState &);
void remoteSnapshot(const Snapshot &, bool valid, const char *expectedHost);
bool takeRemoteOperation(ldg::Operation &);
void finishRemoteOperation(const ldg::Id &, ldg::Result);
void stateJson(JsonDocument &, const Snapshot &);
uint8_t commandCode(const char *);
const char *commandName(uint8_t);
extern std::atomic<bool> httpReady;
void captiveDnsService();
void startUsb();
void serviceUsb();
bool usbMaintenance();
bool maintenanceSafe();
void holdMaintenance();
void scheduleRestart();
void deviceIdentity(JsonDocument&);
void networkDiagnostics(JsonObject);
void lcdDiagnostics(JsonObject);
bool requestWifiScan();
void wifiScanJson(JsonDocument&);
void startHttp();
void startNetwork();
extern std::atomic<int> lightRaw;
extern std::atomic<float> measuredVoltage;
void startSensors();
void serviceSensors();
void startDisplay();
void displayService();
void displayUpdateState(const char *, unsigned);
struct UpdateStatus {
    char stage[24] = "idle", error[64] = "";
    uint32_t bytes = 0, total = 0;
    bool active = false;
};
bool pauseForUpdate();
void updateInit();
void updateService();
UpdateStatus updateStatus();
bool updateBegin(JsonVariantConst manifest);
bool updateWrite(const uint8_t *, size_t);
bool updateFinish();
bool updateCancel();
void updateDownload(const char *url);
void updateBootCheck();
} // namespace app
