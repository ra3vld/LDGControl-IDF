// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_wifi.h"
namespace app {
struct WifiStorageStatus {bool ram=false;int error=0;};
void fillWifiStation(wifi_config_t&,const char* ssid,const char* password);
WifiStorageStatus prepareWifiStorage(); // After init/mode, before applying credentials.
}
