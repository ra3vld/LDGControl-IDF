// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "shack_wire.hpp"
#include "esp_netif.h"

namespace app {
const char *shackNode();
shack::Station shackStation();
void serviceShackUdp(esp_netif_t *station, bool enabled);
}
