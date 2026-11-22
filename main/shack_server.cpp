// SPDX-License-Identifier: GPL-3.0-or-later
#include "shack_server.hpp"
#include "runtime.hpp"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include <array>
#include <cstdio>
#include <memory>

namespace app {
const char *shackNode() {
    static const auto id = [] {
        std::array<char, 13> result{};
        uint8_t mac[6]{};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(result.data(), result.size(), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return result;
    }();
    return id.data();
}

shack::Station shackStation() {
    static constexpr shack_v1_TunerCapabilities capabilities{
        true, true, true, true, true, true, true, true, 2
    };
    return {shackNode(), "tuner", &capabilities, [](shack_v1_TunerState &out) {
        const auto value = snapshot();
        const auto &tuner = value.tuner;
        out.connected = tuner.meterAgeMs < 3000;
        out.busy = value.busy;
        out.sequence = value.seq;
        switch (tuner.mode == MODE_BYPASS ? MODE_BYPASS : ldg::operatingMode(tuner)) {
        case MODE_MANUAL: out.mode = shack_v1_TunerMode_TUNER_MODE_MANUAL; break;
        case MODE_AUTO: out.mode = shack_v1_TunerMode_TUNER_MODE_AUTO; break;
        case MODE_CONTROLLED: out.mode = shack_v1_TunerMode_TUNER_MODE_CONTROLLED; break;
        case MODE_BYPASS: out.mode = shack_v1_TunerMode_TUNER_MODE_BYPASS; break;
        default: break;
        }
        if (ldg::knownAntenna(tuner.antenna)) {
            out.has_antenna = true;
            out.antenna = uint8_t(tuner.antenna) - 1;
        }
        if (tuner.meterAgeMs != UINT32_MAX) {
            out.has_meter_age_ms = true;
            out.meter_age_ms = tuner.meterAgeMs;
        }
        if (tuner.meterValid) {
            out.has_meter = true;
            out.meter = {true, tuner.meter.forward_power_watts, true,
                         tuner.meter.reflected_power_watts, true, tuner.meter.swr};
        }
        return true;
    }};
}

void serviceShackUdp(esp_netif_t *station, bool enabled) {
    static int fd = -1;
    static uint32_t address = 0;
    static std::unique_ptr<char[]> payload;
    esp_netif_ip_info_t ip{};
    if (!enabled || !station || esp_netif_get_ip_info(station, &ip) != ESP_OK || !ip.netmask.addr)
        ip.ip.addr = 0;
    if (address != ip.ip.addr) {
        if (fd >= 0) close(fd);
        fd = -1;
        address = ip.ip.addr;
        if (!address)
            payload.reset();
        if (address) {
            fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in bound{};
            bound.sin_family = AF_INET;
            bound.sin_addr.s_addr = address;
            bound.sin_port = htons(shack::UdpPort);
            if (fd >= 0 && bind(fd, reinterpret_cast<const sockaddr *>(&bound), sizeof bound) != 0) {
                close(fd);
                fd = -1;
            }
            if (fd >= 0 && !payload)
                payload.reset(new (std::nothrow) char[shack::MaxPacket + 1]);
        }
    }
    if (fd < 0 || !payload)
        return;
    sockaddr_in sender{};
    socklen_t size = sizeof sender;
    const auto n = recvfrom(fd, payload.get(), shack::MaxPacket + 1, MSG_DONTWAIT,
                            reinterpret_cast<sockaddr *>(&sender), &size);
    if (n <= 0 || n > int(shack::MaxPacket) || sender.sin_family != AF_INET ||
        ((sender.sin_addr.s_addr ^ address) & ip.netmask.addr))
        return;
    std::string reply;
    if (shack::udp(shackStation(), {payload.get(), size_t(n)}, reply))
        sendto(fd, reply.data(), reply.size(), MSG_DONTWAIT,
               reinterpret_cast<const sockaddr *>(&sender), size);
}
}
