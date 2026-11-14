// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"
#include "config_store.h"
#include "connection_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>
#include <memory>
#include <new>
namespace app {
static Config current;
static SemaphoreHandle_t mutex;
struct Storage {
    nvs_handle_t handle = 0;
    size_t getBytesLength(const char *k) {
        size_t n = 0;
        nvs_get_blob(handle, k, nullptr, &n);
        return n;
    }
    size_t getBytes(const char *k, void *p, size_t n) {
        return nvs_get_blob(handle, k, p, &n) == ESP_OK ? n : 0;
    }
};
Config defaultSettings() {
    Config value;
    strcpy(value.device.webUsername, "admin");
    strcpy(value.device.webPassword, "change_me_please");
    strcpy(value.device.ant1Name, "ANT 1");
    strcpy(value.device.ant2Name, "ANT 2");
    value.device.meterPsuVoltage = 13.8f;
    value.device.mqttPort = 1883;
#if LDG_BOARD_sunton
    value.uartRx = 35;
    value.uartTx = 22;
#elif LDG_BOARD_headless
    value.uartRx = 16;
    value.uartTx = 17;
#endif
    return value;
}
void settingsInit() {
    mutex = xSemaphoreCreateMutex();
    current = defaultSettings();
    // Never erase NVS to make an initialization error disappear.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("settings", "NVS unavailable: %s", esp_err_to_name(err));
        return;
    }
    Storage s;
    if (nvs_open("ldg-idf", NVS_READONLY, &s.handle) == ESP_OK) {
        Config saved{};
        if (s.getBytesLength("config") == sizeof(saved) &&
            s.getBytes("config", &saved, sizeof(saved)) == sizeof(saved) && saved.schema == 1 &&
            ldg::validDeviceConfig(saved.device) && ldg::validRadioSettings(saved.radio) &&
            ldg::validAntennaSettings(saved.antennas) && ldg::validTuningSettings(saved.tuning) &&
            ldg::validBrightnessSettings(saved.brightness) &&
            ldg::validMeterSettings(saved.meter)) {
            current = saved;
            nvs_close(s.handle);
            return;
        }
        nvs_close(s.handle);
    }
    if (nvs_open("ldg-config", NVS_READONLY, &s.handle) != ESP_OK)
        return;
    if (!ldg::readConfig(s, current.device)) {
        // Before blob-based configuration, Preferences stored individual keys.
        auto str = [&](const char *key, auto &out) {
            size_t n = sizeof(out);
            if (nvs_get_str(s.handle, key, out, &n) != ESP_OK)
                out[0] = 0;
        };
        size_t length = 0;
        if (nvs_get_str(s.handle, "wifiSSID", nullptr, &length) == ESP_OK) {
            str("wifiSSID", current.device.wifiSSID);
            str("wifiPassword", current.device.wifiPassword);
            str("mqttBroker", current.device.mqttBroker);
            str("mqttUsername", current.device.mqttUsername);
            str("mqttPassword", current.device.mqttPassword);
            str("remoteHost", current.device.remoteHost);
            str("staticIP", current.device.staticIP);
            str("staticNetmask", current.device.staticNetmask);
            str("staticGateway", current.device.staticGateway);
            str("staticDNS", current.device.staticDNS);
            auto &d = current.device;
            char u[32]{}, p[64]{};
            str("webUsername", u);
            str("webPassword", p);
            if (u[0])
                strcpy(d.webUsername, u);
            if (p[0])
                strcpy(d.webPassword, p);
            nvs_get_u16(s.handle, "mqttPort", &d.mqttPort);
            uint8_t b = 0;
            nvs_get_u8(s.handle, "useStaticIP", &b);
            d.useStaticIP = b != 0;
            nvs_get_u8(s.handle, "haDiscovery", &b);
            d.mqttDiscoveryEnabled = b != 0;
            size_t n = sizeof(float);
            nvs_get_blob(s.handle, "meterPsuVoltage", &d.meterPsuVoltage, &n);
        }
    }
    // Individual-key migrations may contain a half-written old section.
    // Keep independent valid sections usable instead of inheriting bad values.
    if (!std::isfinite(current.device.meterPsuVoltage) || current.device.meterPsuVoltage < 8 ||
        current.device.meterPsuVoltage > 18)
        current.device.meterPsuVoltage = 13.8;
    if (!current.device.mqttPort)
        current.device.mqttPort = 1883;
    if (current.device.useStaticIP &&
        (!ldg::validIPv4(current.device.staticIP) ||
         !ldg::validIPv4(current.device.staticNetmask) ||
         !ldg::validIPv4(current.device.staticGateway) ||
         (current.device.staticDNS[0] && !ldg::validIPv4(current.device.staticDNS))))
        current.device.useStaticIP = false;
    ldg::readAntennaSettings(s, current.antennas);
    ldg::readTuningSettings(s, current.tuning);
    ldg::readRadioSettings(s, current.radio);
    ldg::readBrightnessSettings(s, current.brightness);
    ldg::readMeterSettings(s, current.meter);
    current.device.displayOrientation = (current.device.displayOrientation & 2) ? 2 : 0;
    current.mqttEnabled = current.device.mqttBroker[0] != 0;
    current.remoteEnabled = current.device.remoteHost[0] != 0;
    nvs_close(s.handle); // Old namespace remains intact, including after new saves.
}
Config settings() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto value = current;
    xSemaphoreGive(mutex);
    return value;
}
bool trySettings(Config &out) {
    if (xSemaphoreTake(mutex, 0) != pdTRUE)
        return false;
    out = current;
    xSemaphoreGive(mutex);
    return true;
}
static bool text(JsonVariantConst v, char *out, size_t n, bool secret = false) {
    if (v.isUnbound())
        return true;
    if (!v.is<const char *>())
        return false;
    const char *s = v.as<const char *>();
    if (strlen(s) >= n || v.as<JsonString>().size() != strlen(s) || !ldg::validConfigText(s))
        return false;
    if (!secret || s[0])
        strcpy(out, s);
    return true;
}
template <class T> static bool number(JsonVariantConst v, T &out, int lo, int hi) {
    if (v.isUnbound())
        return true;
    if (!v.is<int>() || v.as<int>() < lo || v.as<int>() > hi)
        return false;
    out = static_cast<T>(v.as<int>());
    return true;
}
template <class T> static bool boolean(JsonVariantConst v, T &out) {
    if (v.isUnbound())
        return true;
    if (!v.is<bool>())
        return false;
    out = v.as<bool>();
    return true;
}
static int sectionIndex(const char *name) {
    constexpr const char *sections[] = {"network",  "mqtt",    "radio", "remote", "tuning",
                                        "antennas", "display", "meter", "device"};
    for (int i = 0; i < 9; ++i)
        if (!strcmp(name, sections[i]))
            return i;
    return -1;
}
static bool commit(Config &next) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("ldg-idf", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "config", &next, sizeof(next));
        if (err == ESP_OK)
            err = nvs_commit(h);
        if (err == ESP_OK) {
            // Allocate only for the transaction, rather than increasing every
            // HTTP/USB task stack by another complete configuration.
            std::unique_ptr<Config> readback(new(std::nothrow) Config);
            size_t bytes = sizeof(Config);
            err = readback ? nvs_get_blob(h,"config",readback.get(),&bytes) : ESP_FAIL;
            if (err == ESP_OK && (bytes != sizeof(next) || memcmp(readback.get(), &next, sizeof(next))))
                err = ESP_FAIL;
        }
        nvs_close(h);
    }
    if (err == ESP_OK)
        current = next;
    return err == ESP_OK;
}
// Explicit secret deletion also retires the migration backup. The native blob
// is committed first so older credentials cannot be imported after power loss.
static bool purgeLegacy() {
    nvs_handle_t old;
    const auto err=nvs_open("ldg-config",NVS_READWRITE,&old);
    if (err==ESP_ERR_NVS_NOT_FOUND) return true;
    if (err!=ESP_OK) return false;
    const bool ok=nvs_erase_all(old)==ESP_OK && nvs_commit(old)==ESP_OK;
    nvs_close(old);return ok;
}
static bool finishSave(Config& next,bool clearLegacy) {
    return commit(next) && (!clearLegacy || purgeLegacy());
}
bool persistManaged(bool value) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = true;
    if (current.tuning.managed != value) {
        auto next = current;
        next.tuning.managed = value;
        ++next.revision;
        ok = commit(next);
    }
    xSemaphoreGive(mutex);
    return ok;
}
ldg::Result saveSettings(const char *section, JsonVariantConst in, uint32_t expected,
                         bool &restart) {
    const int sectionId = sectionIndex(section);
    if (sectionId < 0 || !in.is<JsonObjectConst>())
        return ldg::Result::Invalid;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (expected != current.sectionRevision[sectionId]) {
        xSemaphoreGive(mutex);
        return ldg::Result::TargetChanged;
    }
    Config next = current;
    auto &d = next.device;
    bool valid = true;
    restart = false;
#define STR(key, field) valid &= text(in[key], field, sizeof(field))
#define SECRET(key, field) valid &= text(in[key], field, sizeof(field), true)
#define BOOL(key, field) valid &= boolean(in[key], field)
#define NUM(key, field, lo, hi) valid &= number(in[key], field, lo, hi)
    if (!strcmp(section, "network")) {
        STR("ssid", d.wifiSSID);
        SECRET("password", d.wifiPassword);
        if (!in["clear_password"].isUnbound()) {
            valid &= in["clear_password"].is<bool>();
            if (in["clear_password"] == true) d.wifiPassword[0] = 0;
        }
        if (!in["ssid"].isUnbound() || !in["password"].isUnbound() || in["clear_password"] == true) {
            const size_t n = strlen(d.wifiPassword);
            valid &= !n || (n >= 8 && n <= 64);
            if (n == 64) for (const char* p = d.wifiPassword; *p; ++p)
                valid &= (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');
        }
        BOOL("static", d.useStaticIP);
        STR("ip", d.staticIP);
        STR("netmask", d.staticNetmask);
        STR("gateway", d.staticGateway);
        STR("dns", d.staticDNS);
        restart = ldg::networkSettingsChanged(current.device, d);
    } else if (!strcmp(section, "mqtt")) {
        BOOL("enabled", next.mqttEnabled);
        STR("host", d.mqttBroker);
        NUM("port", d.mqttPort, 1, 65535);
        STR("username", d.mqttUsername);
        SECRET("password", d.mqttPassword);
        if (!in["clear_password"].isUnbound()) {
            valid &= in["clear_password"].is<bool>();
            if (in["clear_password"] == true) d.mqttPassword[0] = 0;
        }
        for (const char* p = d.mqttBroker; *p; ++p)
            valid &= uint8_t(*p) > 32 && uint8_t(*p) < 127 && !strchr("/#+@", *p);
        BOOL("discovery", d.mqttDiscoveryEnabled);
        if (next.mqttEnabled && !d.mqttBroker[0])
            valid = false;
    } else if (!strcmp(section, "radio")) {
        auto &r = next.radio;
        BOOL("enabled", r.enabled);
        STR("host", r.host);
        NUM("port", r.port, 1, 65535);
        NUM("receiver", r.receiver, 0, 15);
    } else if (!strcmp(section, "remote")) {
        BOOL("enabled", next.remoteEnabled);
        STR("host", d.remoteHost);
        if (next.remoteEnabled && !d.remoteHost[0])
            valid = false;
    } else if (!strcmp(section, "tuning")) {
        BOOL("auto_fallback", next.tuning.autoFallback);
        if (!in["swr_threshold"].isUnbound()) {
            double v = in["swr_threshold"].as<double>();
            if (!in["swr_threshold"].is<double>() || !std::isfinite(v) || v < 1.1 || v > 10 ||
                std::abs(v * 10 - std::round(v * 10)) > 0.0001)
                valid = false;
            else
                next.tuning.swrTenths = uint16_t(std::round(v * 10));
        }
        if (!in["managed"].isUnbound())
            valid = false;
    } else if (!strcmp(section, "antennas")) {
        BOOL("enabled", next.antennas.enabled);
        BOOL("external_enabled", next.routes.enabled);
        BOOL("simulate", next.simulateSwitch);
        auto bands = in["bands"];
        if (!bands.isUnbound()) {
            if (!bands.is<JsonArrayConst>() || bands.size() != 11)
                valid = false;
            else
                for (unsigned i = 1; i < BAND_COUNT; ++i) {
                    auto v = bands[i - 1];
                    if (!v.is<JsonObjectConst>())
                        valid = false;
                    else {
                        valid &= number(v["ldg"], next.antennas.bands[i], 0, 2);
                        valid &= number(v["external"], next.routes.bands[i].external, 0, 16);
                        next.routes.bands[i].ldg = next.antennas.bands[i];
                    }
                }
        }
        STR("ant1_name", d.ant1Name);
        STR("ant2_name", d.ant2Name);
    } else if (!strcmp(section, "display")) {
        BOOL("automatic", next.brightness.automatic);
        NUM("brightness", next.brightness.manual, 1, 100);
        NUM("minimum", next.brightness.minimum, 1, 100);
        NUM("maximum", next.brightness.maximum, 1, 100);
        NUM("dark_adc", next.brightness.darkAdc, 0, 4095);
        NUM("bright_adc", next.brightness.brightAdc, 0, 4095);
        if (!in["rotation"].isUnbound()) {
            int r = in["rotation"] | 0;
            if (!in["rotation"].is<int>() || (r != 0 && r != 180))
                valid = false;
            else
                d.displayOrientation = r == 180 ? 2 : 0;
        }
    } else if (!strcmp(section, "meter")) {
        NUM("watts", next.meter.watts, 1, 1000);
        BOOL("logarithmic", next.meter.logarithmic);
        if (!in["psu_voltage"].isUnbound()) {
            if (!in["psu_voltage"].is<float>())
                valid = false;
            else
                d.meterPsuVoltage = in["psu_voltage"];
        }
    } else if (!strcmp(section, "device")) {
        STR("username", d.webUsername);
        SECRET("password", d.webPassword);
        NUM("uart_rx", next.uartRx, -1, 48);
        NUM("uart_tx", next.uartTx, -1, 48);
        restart = next.uartRx != current.uartRx || next.uartTx != current.uartTx;
#if LDG_BOARD_sunton
        valid &= next.uartRx == 35 && next.uartTx == 22;
#elif LDG_BOARD_guition
        // Exclude LCD, touch, flash/PSRAM and USB pins. No default UART wiring
        // is assumed: the owner chooses two physically exposed spare GPIOs.
        auto spare = [](int p) { return p == 1 || p == 2 || p == 40 || p == 41 || p == 42; };
        valid &= (next.uartRx == -1 && next.uartTx == -1) ||
                 (spare(next.uartRx) && spare(next.uartTx) && next.uartRx != next.uartTx);
#elif LDG_BOARD_headless
        valid &= (next.uartRx == -1 && next.uartTx == -1) ||
                 (GPIO_IS_VALID_GPIO(next.uartRx) && GPIO_IS_VALID_OUTPUT_GPIO(next.uartTx) &&
                  next.uartRx != next.uartTx && !(next.uartRx >= 6 && next.uartRx <= 11) &&
                  !(next.uartTx >= 6 && next.uartTx <= 11));
#endif
    } else
        valid = false;
#undef STR
#undef SECRET
#undef BOOL
#undef NUM
    valid &= ldg::validDeviceConfig(d) && ldg::validRadioSettings(next.radio) &&
             ldg::validAntennaSettings(next.antennas) && ldg::validTuningSettings(next.tuning) &&
             ldg::validBrightnessSettings(next.brightness) && ldg::validMeterSettings(next.meter);
    if (!valid) {
        xSemaphoreGive(mutex);
        return ldg::Result::Invalid;
    }
    const bool clearLegacy = (!strcmp(section,"network") || !strcmp(section,"mqtt")) && in["clear_password"]==true;
    if (!memcmp(&next, &current, sizeof(next))) {
        const bool ok = !clearLegacy || finishSave(next,true);
        xSemaphoreGive(mutex);return ok ? ldg::Result::Success : ldg::Result::Failed;
    }
    ++next.revision;
    ++next.sectionRevision[sectionId];
    bool ok = finishSave(next,clearLegacy);
    xSemaphoreGive(mutex);
    return ok ? ldg::Result::Success : ldg::Result::Failed;
}
ldg::Result saveConnectionSettings(JsonVariantConst patch, bool& restart) {
    static const char* allowed[] = {"wifiSSID","wifiPassword","clearWifiPassword","mqttBroker",
        "mqttPort","mqttUsername","mqttPassword","clearMqttPassword","mqttDiscoveryEnabled",
        "useStaticIP","staticIP","staticNetmask","staticGateway","staticDNS"};
    if (!patch.is<JsonObjectConst>()) return ldg::Result::Invalid;
    for (JsonPairConst pair : patch.as<JsonObjectConst>()) {
        bool known = false;
        for (const char* key : allowed) if (!strcmp(pair.key().c_str(), key)) known = true;
        if (!known) return ldg::Result::Invalid;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    Config next = current;
    if (!ldg::patchConnectionConfig(patch, next.device, true)) {
        xSemaphoreGive(mutex); return ldg::Result::Invalid;
    }
    next.mqttEnabled = next.device.mqttBroker[0] != 0;
    restart = ldg::networkSettingsChanged(current.device, next.device);
    const bool clearLegacy=patch["clearWifiPassword"]==true || patch["clearMqttPassword"]==true;
    if (!memcmp(&next, &current, sizeof(next))) {
        const bool ok=!clearLegacy || finishSave(next,true);
        xSemaphoreGive(mutex);return ok ? ldg::Result::Success : ldg::Result::Failed;
    }
    if (restart) ++next.sectionRevision[0];
    if (ldg::mqttSettingsChanged(current.device, next.device) || next.mqttEnabled != current.mqttEnabled)
        ++next.sectionRevision[1];
    ++next.revision;
    const bool ok = finishSave(next,clearLegacy);
    xSemaphoreGive(mutex);
    return ok ? ldg::Result::Success : ldg::Result::Failed;
}
ldg::Result resetSettings() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto next = defaultSettings();
    next.revision = current.revision + 1;
    for (unsigned i = 0; i < 9; ++i) next.sectionRevision[i] = current.sectionRevision[i] + 1;
    const bool ok=finishSave(next,true);
    xSemaphoreGive(mutex);
    return ok ? ldg::Result::Success : ldg::Result::Failed;
}
void readSettings(const char *section, JsonDocument &out) {
    auto c = settings();
    auto &d = c.device;
    const int index = sectionIndex(section);
    out["revision"] = index >= 0 ? c.sectionRevision[index] : 0;
    if (!strcmp(section, "network")) {
        out["ssid"] = d.wifiSSID;
        out["password_set"] = bool(d.wifiPassword[0]);
        out["static"] = d.useStaticIP;
        out["ip"] = d.staticIP;
        out["netmask"] = d.staticNetmask;
        out["gateway"] = d.staticGateway;
        out["dns"] = d.staticDNS;
    } else if (!strcmp(section, "mqtt")) {
        out["enabled"] = c.mqttEnabled;
        out["host"] = d.mqttBroker;
        out["port"] = d.mqttPort;
        out["username"] = d.mqttUsername;
        out["password_set"] = bool(d.mqttPassword[0]);
        out["discovery"] = d.mqttDiscoveryEnabled;
    } else if (!strcmp(section, "radio")) {
        ldg::writeRadioSettingsJson(out, c.radio);
    } else if (!strcmp(section, "remote")) {
        out["enabled"] = c.remoteEnabled;
        out["host"] = d.remoteHost;
    } else if (!strcmp(section, "tuning")) {
        ldg::writeTuningSettingsJson(out, c.tuning);
    } else if (!strcmp(section, "antennas")) {
        out["enabled"] = bool(c.antennas.enabled);
        out["external_enabled"] = c.routes.enabled;
        out["simulate"] = c.simulateSwitch;
        out["ant1_name"] = d.ant1Name;
        out["ant2_name"] = d.ant2Name;
        auto a = out["bands"].to<JsonArray>();
        for (unsigned i = 1; i < BAND_COUNT; ++i) {
            auto v = a.add<JsonObject>();
            v["ldg"] = c.antennas.bands[i];
            v["external"] = c.routes.bands[i].external;
        }
    } else if (!strcmp(section, "display")) {
        ldg::writeBrightnessJson(out, c.brightness);
        out["brightness"] = c.brightness.manual;
        out["rotation"] = d.displayOrientation ? 180 : 0;
    } else if (!strcmp(section, "meter")) {
        out["watts"] = c.meter.watts;
        out["logarithmic"] = c.meter.logarithmic;
        out["psu_voltage"] = d.meterPsuVoltage;
    } else if (!strcmp(section, "device")) {
        out["username"] = d.webUsername;
        out["password_set"] = true;
        out["uart_rx"] = c.uartRx;
        out["uart_tx"] = c.uartTx;
    } else
        out["error"] = "unknown_section";
}
} // namespace app
