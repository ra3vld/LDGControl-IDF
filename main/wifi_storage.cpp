// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_storage.hpp"
#include "esp_wifi.h"
#include <algorithm>
#include <iterator>
#include <cstring>
namespace app {
void fillWifiStation(wifi_config_t& out,const char* ssid,const char* password) {
    out={};
    // These fixed-width SDK fields permit a full 32-byte SSID / 64-byte PSK.
    // strlcpy would truncate the last valid byte to reserve a terminator.
    memcpy(out.sta.ssid,ssid,std::min(strlen(ssid),sizeof(out.sta.ssid)));
    memcpy(out.sta.password,password,std::min(strlen(password),sizeof(out.sta.password)));
}
WifiStorageStatus prepareWifiStorage() {
    WifiStorageStatus result;
    auto error=esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (error==ESP_OK) {
        wifi_config_t previous{};
        error=esp_wifi_get_config(WIFI_IF_STA,&previous);
        const auto nonzero=[](uint8_t byte){return byte!=0;};
        const bool stored=std::any_of(std::begin(previous.sta.ssid),std::end(previous.sta.ssid),nonzero) ||
            std::any_of(std::begin(previous.sta.password),std::end(previous.sta.password),nonzero);
        if (error==ESP_OK && stored) {
            previous={};
            error=esp_wifi_set_config(WIFI_IF_STA,&previous);
        }
    }
    result.error=error;
    // Application NVS owns the only persistent credentials. A failed RAM
    // transition must prevent applying a private password to FLASH storage.
    error=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    result.ram=error==ESP_OK;
    if (error!=ESP_OK) result.error=error;
    return result;
}
}
