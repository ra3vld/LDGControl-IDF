#pragma once
#include <cstdint>
#include <cstring>
static constexpr int ESP_MAC_WIFI_STA=0;
inline int esp_read_mac(uint8_t* out,int){const uint8_t mac[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};memcpy(out,mac,6);return 0;}
