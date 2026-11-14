// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "lwip/sockets.h"
#include <cstring>
namespace app {
// One nonblocking datagram per service call; no DNS task, queue, or heap buffer.
void captiveDnsService() {
    static int fd = -1;
    static uint32_t tried = 0;
    if (fd < 0) {
        if (nowMs() - tried < 5000)
            return;
        tried = nowMs();
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
            return;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(53);
        local.sin_addr.s_addr = inet_addr("192.168.4.1");
        if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
            close(fd);
            fd = -1;
            return;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    uint8_t data[272];
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    int n = recvfrom(fd, data, 256, 0, reinterpret_cast<sockaddr *>(&source), &length);
    if (n < 17 || data[2] & 0x80 || data[4] || data[5] != 1 || data[6] || data[7] ||
        (ntohl(source.sin_addr.s_addr) & 0xffffff00) != 0xc0a80400)
        return;
    unsigned end = 12;
    while (end < unsigned(n) && data[end]) {
        unsigned part = data[end];
        if (part > 63 || end + part + 1 >= unsigned(n))
            return;
        end += part + 1;
    }
    if (end + 5 != unsigned(n) || data[end + 1] != 0 || data[end + 2] != 1 || data[end + 3] != 0 ||
        data[end + 4] != 1)
        return;
    data[2] = 0x81;
    data[3] = 0x80;
    data[6] = 0;
    data[7] = 1;
    data[8] = data[9] = data[10] = data[11] = 0;
    const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 168, 4, 1};
    memcpy(data + n, answer, sizeof(answer));
    sendto(fd, data, n + sizeof(answer), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&source),
           length);
}
} // namespace app
