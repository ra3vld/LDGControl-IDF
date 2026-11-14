#pragma once
static constexpr unsigned portMAX_DELAY = ~0u;
static constexpr int pdTRUE = 1;
using TaskHandle_t=void*;
#define pdMS_TO_TICKS(n) (n)
