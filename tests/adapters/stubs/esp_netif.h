#pragma once
struct esp_netif_ip_info_t{unsigned ip;};
inline void* esp_netif_get_handle_from_ifkey(const char*){return nullptr;}
inline int esp_netif_get_ip_info(void*,esp_netif_ip_info_t*){return -1;}
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) 0u,0u,0u,0u
