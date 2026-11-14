#pragma once
#include "nvs.h"
#include <vector>
#include <cassert>
enum wifi_storage_t{WIFI_STORAGE_FLASH,WIFI_STORAGE_RAM};
static constexpr int WIFI_IF_STA=0;
struct wifi_config_t{struct{uint8_t ssid[32]{},password[64]{};}sta;};
namespace fakeWifi {
inline wifi_config_t stored{},runtime{};
inline wifi_storage_t storage=WIFI_STORAGE_FLASH;
inline int getError=0,clearError=0,ramError=0;
inline unsigned writes=0;
inline std::vector<int> calls;
inline void reset(){stored={};runtime={};storage=WIFI_STORAGE_FLASH;getError=clearError=ramError=0;writes=0;calls.clear();}
}
inline int esp_wifi_set_storage(wifi_storage_t storage){fakeWifi::calls.push_back(storage==WIFI_STORAGE_RAM?4:1);if(storage==WIFI_STORAGE_RAM&&fakeWifi::ramError)return fakeWifi::ramError;fakeWifi::storage=storage;return ESP_OK;}
inline int esp_wifi_get_config(int port,wifi_config_t* out){assert(port==WIFI_IF_STA);fakeWifi::calls.push_back(2);if(fakeWifi::getError)return fakeWifi::getError;*out=fakeWifi::runtime;return ESP_OK;}
inline int esp_wifi_set_config(int port,wifi_config_t* in){assert(port==WIFI_IF_STA);fakeWifi::calls.push_back(3);if(fakeWifi::clearError)return fakeWifi::clearError;fakeWifi::runtime=*in;if(fakeWifi::storage==WIFI_STORAGE_FLASH){fakeWifi::stored=*in;++fakeWifi::writes;}return ESP_OK;}
