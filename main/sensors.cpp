// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "esp_adc/adc_oneshot.h"
namespace app {
std::atomic<int> lightRaw{-1};
std::atomic<float> measuredVoltage{0};
static adc_oneshot_unit_handle_t adc = nullptr;
void startSensors() {
#if !LDG_BOARD_guition
    adc_oneshot_unit_init_cfg_t cfg{};
    cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&cfg, &adc) != ESP_OK)
        return;
    adc_oneshot_chan_cfg_t channel{};
    channel.bitwidth = ADC_BITWIDTH_12;
#if LDG_BOARD_sunton
    channel.atten = ADC_ATTEN_DB_0;
#else
    channel.atten = ADC_ATTEN_DB_12;
#endif
    if (adc_oneshot_config_channel(adc, ADC_CHANNEL_6, &channel) != ESP_OK) {
        adc_oneshot_del_unit(adc);
        adc = nullptr;
    }
#endif
}
void serviceSensors() {
    static uint32_t at = 0;
    const auto now = nowMs();
    if (!adc || now - at < 100)
        return;
    at = now;
    int sum = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int raw;
        if (adc_oneshot_read(adc, ADC_CHANNEL_6, &raw) != ESP_OK) {
            lightRaw = -1;
            measuredVoltage = 0;
            return;
        }
        sum += raw;
    }
#if LDG_BOARD_sunton
    lightRaw = (sum + 2) / 4;
#elif LDG_BOARD_headless
    // Preserve the original 100k/22k divider conversion and fallback range.
    const float voltage = float(sum) * 3.3f * 5.545f / (4 * 4095);
    measuredVoltage = voltage >= 8 && voltage <= 18 ? voltage : 0;
#endif
}
} // namespace app
