// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "json.hpp"
#include "mqtt_protocol.h"
#include "mqtt_readings.h"
#include "mqtt_health.hpp"
#include "managed_mqtt_ca.h"
#include "mdns.h"
#include "esp_err.h"
#include "tci_protocol.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <ctime>
#include <cstring>
#include <new>
namespace app {
static esp_netif_t *station;
static ldg::MqttHealth health;
static portMUX_TYPE healthMutex=portMUX_INITIALIZER_UNLOCKED;
template<class F> static void updateHealth(F change) {
    portENTER_CRITICAL(&healthMutex); change(health); portEXIT_CRITICAL(&healthMutex);
}
static ldg::MqttHealth healthSnapshot() {
    portENTER_CRITICAL(&healthMutex);const auto value=health;portEXIT_CRITICAL(&healthMutex);return value;
}
#if LDG_BOARD_headless
static const char* hostname="ldgcontrol-remote";
#else
static const char* hostname="ldgcontrol-display";
#endif
static std::atomic<bool> mdnsReady{false};
struct ScanEntry {char ssid[33]{};int rssi=0;bool secure=false;};
static ScanEntry scanEntries[12];
static unsigned scanCount=0;
static std::atomic<int> scanStage{0};
static std::atomic<bool> scanDone{false};
static std::atomic<uint32_t> scanRequestedAt{0};
static const char* scanError="";
static portMUX_TYPE scanMutex=portMUX_INITIALIZER_UNLOCKED;
bool requestWifiScan() {
    int old=scanStage.load();
    if (updating || old==1 || old==2) return false;
    scanRequestedAt=nowMs(); // Publish the deadline before the worker sees waiting.
    return scanStage.compare_exchange_strong(old,1);
}
void wifiScanJson(JsonDocument& out) {
    static const char* names[]={"idle","waiting","scanning","complete","error"};
    out["stage"]=names[scanStage.load()];
    portENTER_CRITICAL(&scanMutex);
    ScanEntry entries[12];const auto count=scanCount;const auto error=scanError;
    memcpy(entries,scanEntries,sizeof(entries));
    portEXIT_CRITICAL(&scanMutex);
    out["error"]=error;auto list=out["networks"].to<JsonArray>();
    for (unsigned i=0;i<count;++i) {auto n=list.add<JsonObject>();n["ssid"]=entries[i].ssid;n["rssi"]=entries[i].rssi;n["secure"]=entries[i].secure;}
}
static void serviceScan() {
    if ((scanStage==1 || scanStage==2) && nowMs()-scanRequestedAt.load()>30000) {
        if (scanStage==2) {esp_wifi_scan_stop();esp_wifi_clear_ap_list();}
        portENTER_CRITICAL(&scanMutex);scanError="scan_timeout";scanCount=0;portEXIT_CRITICAL(&scanMutex);
        scanStage=4;
    }
    if (scanStage==1 && !updating && !usbMaintenance() && strcmp(healthSnapshot().state,"connecting")) {
        wifi_scan_config_t config{};config.show_hidden=false;
        config.scan_time.active.min=40;config.scan_time.active.max=120;
        scanDone=false;
        const auto error=esp_wifi_scan_start(&config,false);
        portENTER_CRITICAL(&scanMutex);scanError=error==ESP_OK ? "" : esp_err_to_name(error);portEXIT_CRITICAL(&scanMutex);
        scanStage=error==ESP_OK ? 2 : 4;
    }
    if (scanStage==2 && scanDone.exchange(false)) {
        wifi_ap_record_t records[12];uint16_t n=12;
        const auto result=esp_wifi_scan_get_ap_records(&n,records);
        portENTER_CRITICAL(&scanMutex);
        scanCount=result==ESP_OK?n:0;scanError=result==ESP_OK?"":esp_err_to_name(result);
        for (unsigned i=0;i<scanCount;++i) {memcpy(scanEntries[i].ssid,records[i].ssid,32);scanEntries[i].ssid[32]=0;scanEntries[i].rssi=records[i].rssi;scanEntries[i].secure=records[i].authmode!=WIFI_AUTH_OPEN;}
        portEXIT_CRITICAL(&scanMutex);
        scanStage=result==ESP_OK?3:4;
    }
}
void networkDiagnostics(JsonObject out) {
    const auto h=healthSnapshot();const auto now=nowMs();
    out["state"]=h.state;out["tls"]=h.tls;out["managed"]=h.managed;
    out["error"]=h.error;out["error_active"]=h.errorActive;
    out["attempts"]=h.attempts;out["connect_ms"]=h.connectMs;
    out["retry_ms"]=(!strcmp(h.state,"disconnected") || !strcmp(h.state,"waiting_retry")) && now-h.attemptAt<10000 ? 10000-(now-h.attemptAt):0;
    out["connected_ms"]=!strcmp(h.state,"connected")?now-h.connectedAt:0;
    out["error_age_ms"]=h.errorAt?now-h.errorAt:0;
    out["transport_error"]=h.transport;out["transport_reason"]=esp_err_to_name(h.transport);out["tls_error"]=h.tlsStack;
    out["verify_flags"]=h.verifyFlags;out["socket_errno"]=h.socket;out["broker_code"]=h.broker;
    out["required_free"]=h.requiredFree;out["required_largest"]=h.requiredLargest;
    out["tls_input_bytes"]=CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN;
    out["tls_output_bytes"]=CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN;
    out["trust"]=!h.tls?"none":h.managed?"ISRG Root X2":"certificate_bundle";
    out["published"]=h.published;out["publish_failures"]=h.publishFailures;
    out["hostname"]=hostname;out["mdns"]=mdnsReady.load();
}
static bool stationConfigured = false;
static std::atomic<uint32_t> wifiRetryAt{0};
struct MqttSession {
    esp_mqtt_client_handle_t client = nullptr;
    Config config;
    char uri[100]{}, base[48]{}, statusTopic[64]{}, commandTopic[64]{};
    char input[1025]{};
    size_t inputUsed = 0;
    std::atomic<bool> connected{false}, discovery{false}, reconnectNeeded{false};
    ldg::MqttIdentity identity;
    ldg::MqttStateSchedule schedule;
    struct Reply {
        ldg::Request request{};
        char command[33]{};
        const char *error = nullptr;
        uint8_t state = 0;
        bool extended = false;
    };
    Reply replies[17]; // Last slot is reserved for a queue-full rejection.
    bool inputAccepted = false;
    portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
};
static MqttSession *mqtt = nullptr;
static ldg::MqttReadings heldReadings;
static ldg::Id readingsSource{};
static esp_websocket_client_handle_t tci = nullptr;
static ldg::TciProtocol tciProtocol;
static SemaphoreHandle_t tciMutex;
static ldg::RadioSettings tciSettings;
static bool tciSubscribed = false;
static uint32_t tciConnectedAt = 0;
static void wifiEvent(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) scanDone=true;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START && stationConfigured)
        esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        networkUp = false;
        if (stationConfigured)
            wifiRetryAt = nowMs() + 5000;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        networkUp = true;
}
static void mqttEvent(void *argument, esp_event_base_t, int32_t id, void *eventData) {
    auto &s = *static_cast<MqttSession *>(argument);
    auto e = static_cast<esp_mqtt_event_handle_t>(eventData);
    if (id == MQTT_EVENT_BEFORE_CONNECT) {
        updateHealth([](auto& h){++h.attempts;h.attemptAt=nowMs();h.state="connecting";});return;
    }
    if (id == MQTT_EVENT_CONNECTED) {
        updateHealth([](auto& h){h.connected(nowMs());});
        s.connected = true;
        s.discovery = true;
        mqttConnected = true;
        return;
    }
    if (id == MQTT_EVENT_ERROR && e->error_handle) {
        const auto error=*e->error_handle;
        updateHealth([&](auto& h){
            h.transport=error.esp_tls_last_esp_err;h.tlsStack=error.esp_tls_stack_err;
            h.verifyFlags=error.esp_tls_cert_verify_flags;h.socket=error.esp_transport_sock_errno;
            h.broker=error.error_type==MQTT_ERROR_TYPE_CONNECTION_REFUSED?int(error.connect_return_code):0;
            h.failed(h.broker==4?"credentials":h.broker==5?"authorization":h.broker?"broker_refused":
                h.verifyFlags?"tls_certificate":h.tlsStack?"tls_handshake":"transport",nowMs());
        });
    }
    if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        s.connected=false;mqttConnected=false;
        if (!mqttStopping) s.reconnectNeeded=true;
        if (!mqttStopping) updateHealth([](auto& h){if (!h.errorActive) h.failed("connection_lost",nowMs());});
        return;
    }
    if (id != MQTT_EVENT_DATA || updating || e->total_data_len > 1024)
        return;
    if (e->current_data_offset == 0) {
        s.inputUsed = 0;
        s.inputAccepted = false;
        if (e->topic_len == 20 && !memcmp(e->topic, "homeassistant/status", 20)) {
            if (e->data_len == 6 && !memcmp(e->data, "online", 6))
                s.discovery = true;
            return;
        }
        s.inputAccepted = !e->retain && e->topic_len == int(strlen(s.commandTopic)) &&
                          !memcmp(e->topic, s.commandTopic, e->topic_len);
    }
    if (!s.inputAccepted || e->current_data_offset != int(s.inputUsed) || e->data_len < 0 ||
        s.inputUsed + e->data_len > 1024)
        return;
    memcpy(s.input + s.inputUsed, e->data, e->data_len);
    s.inputUsed += e->data_len;
    if (s.inputUsed != size_t(e->total_data_len))
        return;
    s.input[s.inputUsed] = 0;
    s.inputAccepted = false;
    MqttSession::Reply reply;
    BoundedJson in(4096);
    if (deserializeJson(in.doc, s.input, s.inputUsed, DeserializationOption::NestingLimit(4)))
        reply.error = "invalid_json";
    else {
        const char *name = in.doc["command"] | "";
        if (strlen(name) < sizeof(reply.command))
            strcpy(reply.command, name);
        reply.request.command = commandCode(name);
        reply.request.port = in.doc["port"] | 0;
        if (!reply.request.command)
            reply.error = name[0] ? "unknown_command" : "missing_command";
        reply.extended = !in.doc["request_id"].isUnbound() || !in.doc["source_token"].isUnbound() ||
                         !in.doc["expires_uptime_ms"].isUnbound();
        if (reply.extended) {
            if (!ldg::parseId(in.doc["request_id"], reply.request.id, true) ||
                !ldg::parseId(in.doc["source_token"], reply.request.target) ||
                !in.doc["expires_uptime_ms"].is<uint32_t>())
                reply.error = "invalid_request";
            else
                reply.request.expires = in.doc["expires_uptime_ms"];
        } else {
            esp_fill_random(reply.request.id.data(), 16);
            auto state = snapshot();
            reply.request.target = state.target;
            reply.request.expires = state.uptime + 2000;
        }
    }
    int slot = -1;
    portENTER_CRITICAL(&s.mutex);
    for (unsigned i = 0; i < 16; ++i)
        if (!s.replies[i].state) {
            slot = i;
            break;
        }
    if (slot < 0 && !s.replies[16].state) {
        slot = 16;
        reply.error = "busy";
    }
    if (slot >= 0)
        s.replies[slot].state = 1;
    portEXIT_CRITICAL(&s.mutex);
    if (slot < 0)
        return; // No operation is accepted without a response reservation.
    if (!reply.error) {
        ldg::Operation op;
        auto result = submit(reply.request, op);
        if (result != ldg::Result::Success)
            reply.error = ldg::resultName(result);
    }
    reply.state = 2;
    portENTER_CRITICAL(&s.mutex);
    s.replies[slot] = reply;
    portEXIT_CRITICAL(&s.mutex);
}
static bool publish(MqttSession &s, const char *suffix, JsonDocument &doc, bool retained) {
    char topic[100], bytes[1536];
    snprintf(topic, sizeof(topic), "%s/%s", s.base, suffix);
    if (doc.overflowed() || measureJson(doc) >= sizeof(bytes))
        return false;
    auto n = serializeJson(doc, bytes, sizeof(bytes));
    const bool ok=esp_mqtt_client_publish(s.client, topic, bytes, n, 0, retained)>=0;
    updateHealth([&](auto& h){if (ok) ++h.published;else {++h.publishFailures;h.error="publish";h.errorActive=true;h.errorAt=nowMs();}});
    return ok;
}

static void stopMqtt() {
    if (!mqtt)
        return;
    mqttStopping = true;
    mqttConnected = false;
    auto *old = mqtt;
    mqtt = nullptr;
    if (old->connected) esp_mqtt_client_publish(old->client,old->statusTopic,"offline",0,0,true);
    old->connected = false;
    esp_mqtt_client_stop(old->client);
    esp_mqtt_client_destroy(old->client);
    delete old;
    mqttAllocated = false;
    mqttStopping = false;
    updateHealth([](auto& h){h.state="waiting_retry";h.attemptAt=nowMs();});
}
static void startMqtt(const Config &config) {
    const bool tls=config.device.mqttPort==8883, managed=tls && ldg::managedMqttBroker(config.device.mqttBroker);
    const auto gate=ldg::mqttMemoryGate(tls,managed);
    updateHealth([&](auto& h){h.tls=tls;h.managed=managed;h.requiredFree=gate.free;h.requiredLargest=gate.largest;h.attemptAt=nowMs();});
    if (tls && time(nullptr)<1704067200) {
        updateHealth([](auto& h){h.failed("tls_clock",nowMs());});return;
    }
    if (!gate.allowed(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT))) {
        updateHealth([](auto& h){h.failed(h.tls?"tls_memory":"memory",nowMs());});return;
    }
    auto *s = new (std::nothrow) MqttSession;
    if (!s) {updateHealth([](auto& h){h.failed("memory",nowMs());});return;}
    s->config = config;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char address[18];
    snprintf(address, sizeof(address), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    s->identity = ldg::mqttIdentity(address);
    strlcpy(s->base, s->identity.base.c_str(), sizeof(s->base));
    snprintf(s->uri, sizeof(s->uri), "%s://%s:%u",
             config.device.mqttPort == 8883 ? "mqtts" : "mqtt", config.device.mqttBroker,
             config.device.mqttPort);
    snprintf(s->statusTopic, sizeof(s->statusTopic), "%s/status", s->base);
    snprintf(s->commandTopic, sizeof(s->commandTopic), "%s/command", s->base);
    esp_mqtt_client_config_t c{};
    c.broker.address.uri = s->uri;
    if (managed) c.broker.verification.certificate=ldg::managedMqttCa;
    else if (tls) c.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
    c.credentials.client_id = s->identity.node.c_str();
    c.credentials.username = s->config.device.mqttUsername[0]?s->config.device.mqttUsername:nullptr;
    c.credentials.authentication.password = s->config.device.mqttUsername[0]?s->config.device.mqttPassword:nullptr;
    c.session.last_will.topic = s->statusTopic;
    c.session.last_will.msg = "offline";
    c.session.last_will.retain = true;
    c.session.keepalive = 60;
    c.network.timeout_ms = 30000;
    c.network.reconnect_timeout_ms = 10000;
    c.network.disable_auto_reconnect = true; // Reapply clock and RAM guards before every attempt.
    c.task.stack_size = 6144;
    c.task.priority = 1;
    c.buffer.size = 1024;
    c.buffer.out_size = 1024;
    c.outbox.limit = 4096;
    s->client = esp_mqtt_client_init(&c);
    if (!s->client) {
        updateHealth([](auto& h){h.failed("initialization",nowMs());});
        delete s;
        return;
    }
    if (esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, mqttEvent, s)!=ESP_OK) {
        updateHealth([](auto& h){h.failed("initialization",nowMs());});
        esp_mqtt_client_destroy(s->client);delete s;return;
    }
    mqtt = s;
    mqttAllocated = true;
    if (esp_mqtt_client_start(s->client) != ESP_OK) {
        updateHealth([](auto& h){h.failed("initialization",nowMs());});stopMqtt();
    }
}
static void serviceMqtt() {
    auto *s = mqtt;
    if (!s || !s->connected)
        return;
    if (s->discovery.exchange(false)) {
        esp_mqtt_client_subscribe(s->client, s->commandTopic, 0);
        esp_mqtt_client_subscribe(s->client, "homeassistant/status", 0);
        esp_mqtt_client_publish(s->client, s->statusTopic, "online", 0, 0, true);
        for (unsigned i = 0; i < ldg::MQTT_ENTITY_COUNT; ++i) {
            if (!s->config.device.mqttDiscoveryEnabled) {
                esp_mqtt_client_publish(s->client, ldg::discoveryTopic(s->identity, i).c_str(), "",
                                        0, 0, true);
                continue;
            }
            BoundedJson json;
            esp_netif_ip_info_t ip{};
            char url[64];
            if (esp_netif_get_ip_info(station, &ip) == ESP_OK && ip.ip.addr)
                snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&ip.ip));
            else
                strcpy(url, "http://192.168.4.1/");
            ldg::writeDiscovery(json.doc, s->identity, i, "LDGControl-IDF", url);
            char data[1024];
            size_t n = serializeJson(json.doc, data, sizeof(data));
            if (!json.doc.overflowed() && n < sizeof(data))
                esp_mqtt_client_publish(s->client, ldg::discoveryTopic(s->identity, i).c_str(),
                                        data, n, 0, true);
            vTaskDelay(1);
        }
        s->schedule.requestImmediate();
    }
    auto state = snapshot();
    if (readingsSource != state.target) {
        heldReadings = ldg::MqttReadings{};
        readingsSource = state.target;
    }
    heldReadings.observe(state.tuner);
    if (s->schedule.due(nowMs(), state.tuner)) {
        BoundedJson j;
        heldReadings.write(j.doc, state.tuner);
        char token[33];
        ldg::formatId(state.target, token);
        j.doc["api_version"] = 2;
        j.doc["source_token"] = token;
        j.doc["uptime_ms"] = state.uptime;
        j.doc["busy"] = state.busy;
        j.doc["connected"] = state.tuner.meterValid;
        if (publish(*s, "state", j.doc, true))
            s->schedule.published(nowMs(), state.tuner);
    }
    static uint32_t radioAt = 0;
    if (nowMs() - radioAt >= 1000) {
        radioAt = nowMs();
        BoundedJson j;
        ldg::writeRadioJson(j.doc.to<JsonObject>(), state.tuner.radio);
        j.doc.remove("free_heap");
        j.doc.remove("largest_block");
        j.doc.remove("stack_free");
        char token[33];
        ldg::formatId(state.target, token);
        j.doc["source_token"] = token;
        publish(*s, "radio/state", j.doc, true);
    }
    for (unsigned i = 0; i < 17; ++i) {
        MqttSession::Reply reply;
        portENTER_CRITICAL(&s->mutex);
        reply = s->replies[i];
        portEXIT_CRITICAL(&s->mutex);
        if (reply.state != 2)
            continue;
        const char *result = reply.error;
        if (!result) {
            ldg::Operation op;
            if (!operation(reply.request.id, op) || op.state != ldg::OperationState::Done)
                continue;
            result = ldg::resultName(op.result);
        }
        BoundedJson j(2048);
        j.doc["command"] = reply.command;
        j.doc["result"] = result;
        if (reply.extended) {
            char id[37], token[33];
            ldg::formatId(reply.request.id, id, true);
            ldg::formatId(reply.request.target, token);
            j.doc["request_id"] = id;
            j.doc["source_token"] = token;
        }
        if (publish(*s, "command/response", j.doc, false)) {
            portENTER_CRITICAL(&s->mutex);
            s->replies[i].state = 0;
            portEXIT_CRITICAL(&s->mutex);
        }
    }
}
static void tciEvent(void *, esp_event_base_t, int32_t id, void *data) {
    auto *e = static_cast<esp_websocket_event_data_t *>(data);
    xSemaphoreTake(tciMutex, portMAX_DELAY);
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        tciProtocol.connect(tciSettings.receiver, nowMs());
        tciSubscribed = false;
        tciConnectedAt = nowMs();
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        tciProtocol.disconnect(true, "tci_disconnected");
        tciSubscribed = false;
    } else if (id == WEBSOCKET_EVENT_DATA) {
        if (e->op_code == 1 || e->op_code == 0)
            tciProtocol.feed(reinterpret_cast<const uint8_t *>(e->data_ptr), e->data_len, nowMs());
        else if (e->op_code == 9 || e->op_code == 10)
            tciProtocol.heartbeat(nowMs());
    }
    xSemaphoreGive(tciMutex);
}
static void serviceTci(const Config &config) {
    const bool enabled = config.radio.enabled && !config.remoteEnabled && networkUp && !updating;
    if (tci && (!enabled || !ldg::sameRadioSettings(tciSettings, config.radio))) {
        esp_websocket_client_stop(tci);
        esp_websocket_client_destroy(tci);
        tci = nullptr;
        tciProtocol.disconnect(config.radio.enabled);
    }
    if (!tci && enabled) {
        tciSettings = config.radio;
        char uri[128];
        snprintf(uri, sizeof(uri), "ws://%s:%u/", config.radio.host, config.radio.port);
        esp_websocket_client_config_t c{};
        c.uri = uri;
        c.task_stack = 4096;
        c.task_prio = 1;
        c.buffer_size = 512;
        c.network_timeout_ms = 3000;
        c.reconnect_timeout_ms = 2000;
        c.ping_interval_sec = 1;
        c.pingpong_timeout_sec = 3;
        tci = esp_websocket_client_init(&c);
        if (tci) {
            esp_websocket_register_events(tci, WEBSOCKET_EVENT_ANY, tciEvent, nullptr);
            esp_websocket_client_start(tci);
        }
    }
    xSemaphoreTake(tciMutex, portMAX_DELAY);
    auto radio = tciProtocol.snapshot(nowMs());
    bool initialized = tciProtocol.initialized();
    unsigned count = tciProtocol.receiverCount();
    xSemaphoreGive(tciMutex);
    radio.provider = config.radio.enabled ? ldg::RadioProvider::Tci : ldg::RadioProvider::None;
    radioSnapshot(radio);
    tciAllocated = tci != nullptr;
    if (tci && esp_websocket_client_is_connected(tci) && initialized && !tciSubscribed) {
        char query[128];
        unsigned r = config.radio.receiver;
        snprintf(query, sizeof(query),
                 "vfo:%u,0;vfo:%u,1;split_enable:%u;xit_enable:%u;xit_offset:%u;modulation:%u;", r,
                 r, r, r, r, r);
        esp_websocket_client_send_text(tci, query, strlen(query), pdMS_TO_TICKS(500));
        for (unsigned i = 0; i < count; ++i) {
            snprintf(query, sizeof(query), "trx:%u;tune:%u;", i, i);
            esp_websocket_client_send_text(tci, query, strlen(query), pdMS_TO_TICKS(500));
        }
        tciSubscribed = true;
    }
    if (tci && esp_websocket_client_is_connected(tci) && nowMs() - tciConnectedAt > 10000 &&
        radio.connection != ldg::RadioConnection::Ready) {
        esp_websocket_client_stop(tci);
        esp_websocket_client_destroy(tci);
        tci = nullptr;
    }
}
static bool requestRemote(const Config &c, const char *endpoint, const char *body,
                          JsonDocument &out) {
    char url[160];
    snprintf(url, sizeof(url), "http://%s/api/v2/%s", c.device.remoteHost, endpoint);
    esp_http_client_config_t config{};
    config.url = url;
    config.username = c.device.webUsername;
    config.password = c.device.webPassword;
    config.auth_type = HTTP_AUTH_TYPE_BASIC;
    config.timeout_ms = 2000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    auto h = esp_http_client_init(&config);
    if (!h)
        return false;
    size_t size = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        esp_http_client_set_header(h, "Content-Type", "application/json");
    }
    bool ok = esp_http_client_open(h, size) == ESP_OK;
    if (ok && size)
        ok = esp_http_client_write(h, body, size) == int(size);
    if (ok)
        esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    ok = ok && (status == 200 || status == 202);
    char data[3072];
    unsigned used = 0;
    while (ok && used < sizeof(data) - 1) {
        int n = esp_http_client_read(h, data + used, sizeof(data) - 1 - used);
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        used += n;
    }
    data[used] = 0;
    ok = ok && esp_http_client_is_complete_data_received(h) &&
         !deserializeJson(out, static_cast<const char *>(data), used,
                          DeserializationOption::NestingLimit(8));
    esp_http_client_cleanup(h);
    return ok;
}
static void serviceRemote(const Config &c) {
    static uint32_t at = 0, operationAt = 0;
    static ldg::Operation active;
    static bool running = false;
    static Config targetConfig;
    if (updating)
        return;
    if (c.remoteEnabled && networkUp && nowMs() - at >= 500) {
        at = nowMs();
        BoundedJson doc;
        if (requestRemote(c, "state", nullptr, doc.doc) && doc.doc["api_version"] == 2 &&
            !strcmp(doc.doc["source"] | "", "local")) {
            Snapshot s;
            s.tuner = ldg::readWebState(doc.doc.as<JsonVariantConst>());
            s.tuner.radio = ldg::readRadioJson(doc.doc["radio"]);
            s.tuner.meterAgeMs = doc.doc["meter_age_ms"] | UINT32_MAX;
            s.tuner.meterValid = doc.doc["connected"] | false;
            s.uptime = doc.doc["uptime_ms"] | 0u;
            s.seq = doc.doc["sequence"] | 0u;
            s.busy = doc.doc["busy"] | false;
            auto ext = doc.doc["external_antenna"];
            s.antennaSwitch.requested = ext["requested"] | 0;
            s.antennaSwitch.confirmed = ext["confirmed"] | 0;
            s.antennaSwitch.evidence = static_cast<ldg::Confirmation>(ext["evidence"] | 0);
            s.antennaSwitch.busy = ext["busy"] | false;
            const char *stage = doc.doc["route"]["stage"] | "disabled";
            for (const char *known : {"disabled", "waiting_band", "manual", "complete", "ldg",
                                      "external", "switching", "waiting_rf"})
                if (!strcmp(known, stage))
                    s.route.stage = known;
            s.route.ldg = doc.doc["route"]["ldg"] | 0;
            s.route.external = doc.doc["route"]["external"] | 0;
            s.route.partial = doc.doc["route"]["partial"] | false;
            s.scale = doc.doc["scale_watts"] | 1000;
            s.logarithmic = doc.doc["logarithmic"] | false;
            if (ldg::parseId(doc.doc["source_token"], s.target))
                remoteSnapshot(s, true, c.device.remoteHost);
        } else
            remoteSnapshot({}, false, c.device.remoteHost);
    }
    if (!running && c.remoteEnabled && takeRemoteOperation(active)) {
        targetConfig = c;
        char id[37], token[33], body[320];
        ldg::formatId(active.request.id, id, true);
        ldg::formatId(active.request.target, token);
        snprintf(body, sizeof(body),
                 "{\"command\":\"%s\",\"request_id\":\"%s\",\"source_token\":\"%s\",\"expires_"
                 "uptime_ms\":%u,\"port\":%u}",
                 commandName(active.request.command), id, token, unsigned(active.request.expires),
                 unsigned(active.request.port));
        BoundedJson response;
        requestRemote(targetConfig, "commands", body, response.doc);
        // POST may have reached the recipient even if its response was lost.
        // Reconcile by ID against the same immutable recipient, never resend.
        running = true;
        operationAt = nowMs();
    }
    if (running) {
        char id[37], endpoint[64];
        ldg::formatId(active.request.id, id, true);
        snprintf(endpoint, sizeof(endpoint), "commands/%s", id);
        BoundedJson response;
        if (requestRemote(targetConfig, endpoint, nullptr, response.doc) &&
            !strcmp(response.doc["state"] | "", "done")) {
            finishRemoteOperation(active.request.id, !strcmp(response.doc["result"] | "", "success")
                                                         ? ldg::Result::Success
                                                         : ldg::Result::Failed);
            running = false;
        } else if (nowMs() - operationAt > 30000) {
            finishRemoteOperation(active.request.id, ldg::Result::Unknown);
            running = false;
        }
    }
}
static void worker(void *) {
    uint32_t reconnect = 0, lastAttempt = 0, saveAt = 0;
    for (;;) {
        if (stationConfigured && !networkUp && wifiRetryAt &&
            int32_t(nowMs() - wifiRetryAt.load()) >= 0) {
            wifiRetryAt = 0;
            esp_wifi_connect();
        }
        int wanted = managedToSave.load();
        if (wanted >= 0 && nowMs() - saveAt >= 1000) {
            saveAt = nowMs();
            if (persistManaged(wanted != 0))
                managedToSave.compare_exchange_strong(wanted, -1);
        }
        auto c = settings();
        bool changed =
            mqtt && (memcmp(&mqtt->config.device.mqttBroker, &c.device.mqttBroker,
                            sizeof(c.device.mqttBroker)) ||
                     mqtt->config.device.mqttPort != c.device.mqttPort ||
                     strcmp(mqtt->config.device.mqttUsername, c.device.mqttUsername) ||
                     strcmp(mqtt->config.device.mqttPassword, c.device.mqttPassword) ||
                     mqtt->config.device.mqttDiscoveryEnabled != c.device.mqttDiscoveryEnabled);
        if (mqtt && (!c.mqttEnabled || !networkUp || updating || changed || mqtt->reconnectNeeded || reconnect != mqttReconnect)) {
            stopMqtt();
            lastAttempt = nowMs();
        }
        reconnect = mqttReconnect;
        if (!mqtt && c.mqttEnabled && networkUp && !updating && nowMs() - lastAttempt >= 10000) {
            lastAttempt = nowMs();
            startMqtt(c);
        }
        if (!mqtt && c.mqttEnabled && networkUp && !updating)
            updateHealth([](auto& h){h.state="waiting_retry";});
        if (!c.mqttEnabled) updateHealth([](auto& h){h.state="disabled";h.errorActive=false;});
        else if (!networkUp) updateHealth([](auto& h){h.state="waiting_wifi";});
        else if (updating) updateHealth([](auto& h){h.state="paused";});
        serviceScan();
        captiveDnsService();
        serviceTci(c);
        updateService();
        serviceRemote(c);
        if (!updating)
            serviceMqtt();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void startNetwork() {
    esp_netif_init();
    esp_event_loop_create_default();
    station = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK)
        return;
    auto c = settings();
    stationConfigured = c.device.wifiSSID[0];
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap{};
    strcpy(reinterpret_cast<char *>(ap.ap.ssid), "LDGControl-IDF");
    strcpy(reinterpret_cast<char *>(ap.ap.password), "ldgcontrol");
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (stationConfigured) {
        wifi_config_t sta{};
        strlcpy(reinterpret_cast<char *>(sta.sta.ssid), c.device.wifiSSID, sizeof(sta.sta.ssid));
        strlcpy(reinterpret_cast<char *>(sta.sta.password), c.device.wifiPassword,
                sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_netif_set_hostname(station, hostname);
        if (c.device.useStaticIP) {
            esp_netif_ip_info_t ip{};
            if (esp_netif_str_to_ip4(c.device.staticIP, &ip.ip) == ESP_OK &&
                esp_netif_str_to_ip4(c.device.staticNetmask, &ip.netmask) == ESP_OK &&
                esp_netif_str_to_ip4(c.device.staticGateway, &ip.gw) == ESP_OK) {
                esp_netif_dhcpc_stop(station);
                esp_netif_set_ip_info(station, &ip);
                esp_netif_dns_info_t dns{};
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                if (esp_netif_str_to_ip4(c.device.staticDNS, &dns.ip.u_addr.ip4) == ESP_OK)
                    esp_netif_set_dns_info(station, ESP_NETIF_DNS_MAIN, &dns);
            }
        }
    }
    esp_wifi_start();
    if (mdns_init()==ESP_OK) {
        const bool named=mdns_hostname_set(hostname)==ESP_OK;
        mdns_instance_name_set("LDGControl-IDF");
        mdnsReady=named && mdns_service_add(nullptr,"_http","_tcp",80,nullptr,0)==ESP_OK;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_init();
    tciMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(worker, "network", 10240, nullptr, 1, &observedTasks[2], 0);
}
} // namespace app
