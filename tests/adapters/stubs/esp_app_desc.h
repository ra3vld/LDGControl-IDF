#pragma once
#include <cstdint>
struct esp_app_desc_t{const char* version;uint8_t app_elf_sha256[32];};
inline const esp_app_desc_t* esp_app_get_description(){static const esp_app_desc_t app{"0.1.0",{0xab}};return &app;}
