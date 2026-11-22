// SPDX-License-Identifier: GPL-3.0-or-later
// USB API 1 adapter; protocol derived from Remote-LDGControl. See NOTICE.
#include "runtime.hpp"
#ifdef ESP_PLATFORM
#include "shack_server.hpp"
#endif
#include "json.hpp"
#include "usb_provisioning.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include <cstdio>
#include <cstring>
#include <new>
namespace app {
static const char buildMetadata[] = "LDG_METADATA_V1:{\"board\":\"" LDG_BOARD_NAME
    "\",\"commit\":\"" LDG_BUILD_COMMIT "\",\"source_digest\":\"" LDG_SOURCE_DIGEST
    "\",\"version\":\"" LDG_FIRMWARE_VERSION "\"}\n";

static std::atomic<uint32_t> lease{0}, restartAt{0};
static bool consoleReady = false;
bool usbMaintenance() { return lease && int32_t(lease.load() - nowMs()) > 0; }
void holdMaintenance() {lease=nowMs()+5000;}
void scheduleRestart() { restartAt = nowMs() + 700; }
bool maintenanceSafe() {
    if (updating || xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    const bool safe = controller && controller->canPause(nowMs());
    xSemaphoreGive(controlMutex);
    const auto s = snapshot();
    return safe && !s.busy && !(s.tuner.meterAgeMs <= 3000 && s.tuner.meter.forward_power_raw > 0) &&
        !(s.tuner.radio.ageMs<=ldg::RadioState::LivenessMs && (s.tuner.radio.transmitting || s.tuner.radio.tuning));
}
void deviceIdentity(JsonDocument& out) {
    uint8_t mac[6]; char id[13];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    const auto* app = esp_app_get_description();
    char hash[65]; static const char hex[] = "0123456789abcdef";
    for (unsigned i=0;i<32;++i) { hash[2*i]=hex[app->app_elf_sha256[i]>>4];hash[2*i+1]=hex[app->app_elf_sha256[i]&15]; }
    hash[64]=0;
    out["build_metadata"]=buildMetadata; out["device_id"]=id; out["family"]="tuner";
    out["profile"]=LDG_BOARD_NAME; out["version"]=app->version;
    out["commit"]=LDG_BUILD_COMMIT; out["source_digest"]=LDG_SOURCE_DIGEST;
    out["firmware_id"]=hash; out["sdk_version"]=esp_get_idf_version();
    out["role"]=settings().remoteEnabled ? "display_proxy" : "tuner_controller";
    auto caps=out["capabilities"].to<JsonObject>();
    for (const char* name : {"usb_provisioning","wifi","web","mqtt","mqtt_tls","home_assistant_discovery","tci","ota"}) caps[name]=true;
    caps["display"]=strcmp(LDG_BOARD_NAME,"headless")!=0;
}
static void request(const char* op, JsonVariantConst in, JsonDocument& out) {
    lease=nowMs()+5000;
    if (!strcmp(op,"hello")) { deviceIdentity(out); return; }
    if (!strcmp(op,"settings")) {
        // Mutable arrays make ArduinoJson copy strings into the reply. A const
        // array is treated as static storage, but this snapshot ends on return.
        auto c=settings(); auto& d=c.device;
        auto s=out["settings"].to<JsonObject>();
        s["wifiSSID"]=d.wifiSSID; s["hasWifiPassword"]=bool(d.wifiPassword[0]);
        s["mqttBroker"]=d.mqttBroker; s["mqttPort"]=d.mqttPort;
        s["mqttUsername"]=d.mqttUsername; s["hasMqttPassword"]=bool(d.mqttPassword[0]);
        s["mqttTls"]=d.mqttPort==8883; s["mqttDiscoveryEnabled"]=d.mqttDiscoveryEnabled;
        s["useStaticIP"]=d.useStaticIP; s["staticIP"]=d.staticIP;
        s["staticNetmask"]=d.staticNetmask; s["staticGateway"]=d.staticGateway; s["staticDNS"]=d.staticDNS;
        s["configured"]=bool(d.wifiSSID[0]); out["connected"]=networkUp.load();
        out["mqtt_connected"]=mqttConnected.load();
        esp_netif_ip_info_t ip{}; char text[16]{};
        auto net=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (net && esp_netif_get_ip_info(net,&ip)==ESP_OK) snprintf(text,sizeof(text),IPSTR,IP2STR(&ip.ip));
        out["ip"]=text; return;
    }
    if (strcmp(op,"apply") && strcmp(op,"reboot")) { out["status"]=404;out["error"]="unknown USB operation";return; }
    if (!maintenanceSafe()) {out["status"]=409;out["error"]="tuner or transmitter busy";return;}
    if (!strcmp(op,"reboot")) {scheduleRestart();out["restart"]=true;return;}
    bool restart=false; const auto result=saveConnectionSettings(in["settings"],restart);
    if (result!=ldg::Result::Success) {out["status"]=result==ldg::Result::Invalid?400:500;out["error"]="settings save failed";return;}
    out["saved"]=true;out["restart"]=restart;
}
struct Session {
    ldg::UsbProvisioning protocol;
    char input[ldg::UsbProvisioning::Limit+1]{}, output[ldg::UsbProvisioning::Limit+2]{};
    unsigned used=0;
    bool overflow=false;
};
static Session* session=nullptr;
void startUsb() {
    uart_config_t config{};config.baud_rate=115200;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;config.source_clk=UART_SCLK_DEFAULT;
    consoleReady=uart_param_config(UART_NUM_0,&config)==ESP_OK &&
        (uart_is_driver_installed(UART_NUM_0) || uart_driver_install(UART_NUM_0,512,0,0,nullptr,0)==ESP_OK);
}
void serviceUsb() {
    if (restartAt && int32_t(nowMs()-restartAt.load())>=0) {fflush(stdout);esp_restart();}
    if (!consoleReady) return;
    uint8_t bytes[128];const int count=uart_read_bytes(UART_NUM_0,bytes,sizeof(bytes),0);
#ifdef ESP_PLATFORM
    static shack::UsbFrames shackFrames;
    const auto now = nowMs();
    shackFrames.expire(now);
#endif
    for (int i=0;i<count;++i) {
        const char c=char(bytes[i]);
#ifdef ESP_PLATFORM
        if (shackFrames.active() || (c == '@' && (!session || !session->used))) {
            if (shackFrames.feed(c, now)) {
                std::string body;
                if (shack::usb(shackStation(), shackFrames.payload(), body)) {
                    flockfile(stdout);
                    fprintf(stdout, "%s%zu\n", shack::UsbPrefix, body.size());
                    fwrite(body.data(), 1, body.size(), stdout);
                    fflush(stdout);
                    funlockfile(stdout);
                }
            }
            continue;
        }
#endif
        lease=nowMs()+5000;
        if (!session) {
            if (c!='{') continue;
            session=new(std::nothrow) Session;
            if (!session) return;
            char boot[33];ldg::formatId(bootId,boot);boot[16]=0;
            session->protocol.begin(boot,request,esp_random());
        }
        if (c=='\n') {
            auto& s=*session;s.input[s.used]=0;
            JsonBudget budget(8192);
            if (!s.overflow && s.used && s.protocol.handle(s.input,s.output,sizeof(s.output)-1,&budget)) {
                const size_t n=strlen(s.output);s.output[n]='\n';
                flockfile(stdout);fwrite(s.output,1,n+1,stdout);fflush(stdout);funlockfile(stdout);
            }
            memset(s.input,0,sizeof(s.input));memset(s.output,0,sizeof(s.output));s.used=0;s.overflow=false;
        } else if (c!='\r') {
            if (session->used<ldg::UsbProvisioning::Limit) session->input[session->used++]=c;
            else session->overflow=true;
        }
    }
    if (session && !usbMaintenance()) {delete session;session=nullptr;}
}
} // namespace app
