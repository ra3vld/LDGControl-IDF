// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "json.hpp"
#include "web_assets.hpp"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
namespace app {
std::atomic<bool> httpReady{false};
static httpd_handle_t server;
static QueueHandle_t apiQueue, bulkQueue;
static SemaphoreHandle_t wsWriteMutex;
static std::atomic<unsigned> activeHttp{0};
static portMUX_TYPE wsMutex = portMUX_INITIALIZER_UNLOCKED;
struct Ticket {
    char value[33]{};
    uint32_t at = 0;
    bool used = true;
};
struct Client {
    int fd = -1;
    uint32_t at = 0;
};
static Ticket tickets[4];
static Client clients[2];
static esp_err_t error(httpd_req_t *req, int code, const char *message) {
    char status[40];
    snprintf(status, sizeof(status), "%d %s", code,
             code == 401   ? "Unauthorized"
             : code == 409 ? "Conflict"
             : code == 503 ? "Service Unavailable"
             : code == 404 ? "Not Found"
                           : "Bad Request");
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    return httpd_resp_sendstr(req, body);
}
static bool authenticated(httpd_req_t *req) {
    char auth[256];
    const auto n = httpd_req_get_hdr_value_len(req, "Authorization");
    if (!n || n >= sizeof(auth) ||
        httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK)
        return false;
    if (strncmp(auth, "Basic ", 6))
        return false;
    unsigned char decoded[128];
    size_t length = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &length,
                              reinterpret_cast<unsigned char *>(auth + 6), strlen(auth + 6)))
        return false;
    decoded[length] = 0;
    auto cfg = settings();
    char expected[100];
    int size = snprintf(expected, sizeof(expected), "%s:%s", cfg.device.webUsername,
                        cfg.device.webPassword);
    unsigned different = unsigned(length) ^ unsigned(size);
    for (size_t i = 0; i < length && i < sizeof(expected); ++i)
        different |= decoded[i] ^ expected[i];
    return different == 0;
}
static esp_err_t json(httpd_req_t *req, JsonDocument &doc, const char *status = "200 OK") {
    if (doc.overflowed() || measureJson(doc) > 4096)
        return error(req, 503, "response_budget");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, status);
    struct Writer {
        httpd_req_t *req;
        char data[512];
        size_t used = 0;
        bool ok = true;
        bool flush() {
            if (used && ok) {
                ok = httpd_resp_send_chunk(req, data, used) == ESP_OK;
                used = 0;
            }
            return ok;
        }
        size_t write(uint8_t b) {
            if (!ok)
                return 0;
            data[used++] = b;
            if (used == sizeof(data))
                flush();
            return ok ? 1 : 0;
        }
        size_t write(const uint8_t *bytes, size_t n) {
            size_t i = 0;
            for (; i < n && ok; ++i)
                write(bytes[i]);
            return i;
        }
    } writer{req};
    serializeJson(doc, writer);
    return writer.flush() ? httpd_resp_send_chunk(req, nullptr, 0) : ESP_FAIL;
}
static bool body(httpd_req_t *req, JsonDocument &doc) {
    char type[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncmp(type, "application/json", 16))
        return false;
    if (req->content_len == 0 || req->content_len > 2048)
        return false;
    char data[2049];
    size_t used = 0;
    uint32_t progress = nowMs(), start = progress;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, data + used, req->content_len - used);
        if (n > 0) {
            used += n;
            progress = nowMs();
        } else if (n != HTTPD_SOCK_ERR_TIMEOUT)
            return false;
        if (nowMs() - progress > 4000 || nowMs() - start > 15000)
            return false;
    }
    data[used] = 0;
    return !deserializeJson(doc, static_cast<const char *>(data), used,
                            DeserializationOption::NestingLimit(8));
}
// Called in an HTTP worker: retain the offset on temporary socket pressure.
static int sendResumable(httpd_handle_t, int fd, const char *data, size_t len, int flags) {
    bool ws = false;
    portENTER_CRITICAL(&wsMutex);
    for (auto c : clients)
        if (c.fd == fd)
            ws = true;
    portEXIT_CRITICAL(&wsMutex);
    size_t sent = 0;
    uint32_t start = nowMs(), progress = start;
    while (sent < len) {
        int n = send(fd, data + sent, std::min(size_t(512), len - sent), flags | MSG_DONTWAIT);
        if (n > 0) {
            sent += n;
            progress = nowMs();
        } else if (n == 0)
            return HTTPD_SOCK_ERR_FAIL;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ENOMEM &&
                 errno != ENOBUFS)
            return HTTPD_SOCK_ERR_FAIL;
        if (nowMs() - progress > (ws ? 100u : 4000u) || nowMs() - start > (ws ? 200u : 15000u))
            return HTTPD_SOCK_ERR_TIMEOUT;
        if (sent < len)
            vTaskDelay(1);
    }
    return int(sent);
}
static esp_err_t opened(httpd_handle_t handle, int fd) {
    httpd_sess_set_send_override(handle, fd, sendResumable);
    return ESP_OK;
}
static void closed(httpd_handle_t, int fd) {
    portENTER_CRITICAL(&wsMutex);
    for (auto &c : clients)
        if (c.fd == fd)
            c.fd = -1;
    portEXIT_CRITICAL(&wsMutex);
    close(fd);
}
static esp_err_t handshake(httpd_req_t *req) {
    char query[80], token[40];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "t", token, sizeof(token)) != ESP_OK)
        return ESP_FAIL;
    bool valid = false;
    int slot = -1;
    portENTER_CRITICAL(&wsMutex);
    for (unsigned i = 0; i < 2; ++i)
        if (clients[i].fd < 0)
            slot = int(i);
    if (slot >= 0)
        for (auto &t : tickets)
            if (!t.used && nowMs() - t.at < 10000 && !strcmp(token, t.value)) {
                t.used = true;
                valid = true;
                clients[slot] = {httpd_req_to_sockfd(req), nowMs()};
                break;
            }
    portEXIT_CRITICAL(&wsMutex);
    return valid ? ESP_OK : ESP_FAIL;
}
static esp_err_t websocket(httpd_req_t *req) {
    if (req->method == HTTP_GET)
        return ESP_OK;
    uint8_t data[128];
    httpd_ws_frame_t frame{};
    frame.payload = data;
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > sizeof(data))
        return ESP_FAIL;
    if (httpd_ws_recv_frame(req, &frame, sizeof(data)) != ESP_OK)
        return ESP_FAIL;
    if (frame.type == HTTPD_WS_TYPE_PING || frame.type == HTTPD_WS_TYPE_CLOSE) {
        frame.type = frame.type == HTTPD_WS_TYPE_PING ? HTTPD_WS_TYPE_PONG : HTTPD_WS_TYPE_CLOSE;
        if (xSemaphoreTake(wsWriteMutex, 0) != pdTRUE)
            return ESP_FAIL;
        auto result = httpd_ws_send_frame(req, &frame);
        xSemaphoreGive(wsWriteMutex);
        return result;
    }
    return ESP_OK;
}
static void writeOperation(JsonDocument &out, const ldg::Operation &op) {
    char id[37];
    ldg::formatId(op.request.id, id, true);
    out["request_id"] = id;
    out["state"] = op.state == ldg::OperationState::Queued    ? "queued"
                   : op.state == ldg::OperationState::Running ? "running"
                                                              : "done";
    out["result"] = ldg::resultName(op.result);
    out["command"] = commandName(op.request.command);
}
static void api(httpd_req_t *req) {
    BoundedJson input, output;
    auto &out = output.doc;
    const char *path = req->uri;
    if (req->method == HTTP_POST && !body(req, input.doc)) {
        error(req, 400, "invalid_json");
        return;
    }
    if (!strcmp(path, "/api/v2/state")) {
        stateJson(out, snapshot());
        json(req, out);
    } else if (!strcmp(path, "/api/v2/capabilities")) {
        deviceIdentity(out);
        out["api_version"] = 2;
        out["firmware"] = esp_app_get_description()->version;
#if LDG_BOARD_sunton
        out["board"] = "sunton";
        out["display"] = "480x320";
#elif LDG_BOARD_guition
        out["board"] = "guition";
        out["display"] = "480x480";
#else
        out["board"] = "headless";
        out["display"] = "none";
#endif
        out["ota"] = true;
        out["external_switch"] = "simulator";
        out["ws_clients"] = 2;
        out["operations"] = 16;
        out["release_repository"] = LDG_RELEASE_REPOSITORY;
        json(req, out);
    } else if (!strcmp(path, "/api/v2/ws-token")) {
        ldg::Id id;
        esp_fill_random(id.data(), id.size());
        char token[33];
        ldg::formatId(id, token);
        portENTER_CRITICAL(&wsMutex);
        unsigned i = 0;
        for (unsigned j = 0; j < 4; ++j)
            if (tickets[j].used || nowMs() - tickets[j].at > nowMs() - tickets[i].at)
                i = j;
        strcpy(tickets[i].value, token);
        tickets[i].at = nowMs();
        tickets[i].used = false;
        portEXIT_CRITICAL(&wsMutex);
        out["token"] = token;
        json(req, out);
    } else if (!strcmp(path, "/api/v2/commands") && req->method == HTTP_POST) {
        ldg::Request r;
        auto in = input.doc.as<JsonVariantConst>();
        r.command = commandCode(in["command"]);
        r.port = in["port"] | 0;
        if (!ldg::parseId(in["request_id"], r.id, true) ||
            !ldg::parseId(in["source_token"], r.target) ||
            !in["expires_uptime_ms"].is<uint32_t>() || !r.command) {
            error(req, 400, "invalid_request");
            return;
        }
        r.expires = in["expires_uptime_ms"];
        ldg::Operation op;
        auto result = submit(r, op);
        if (result != ldg::Result::Success) {
            error(req, result == ldg::Result::Busy ? 503 : 409, ldg::resultName(result));
            return;
        }
        writeOperation(out, op);
        json(req, out, "202 Accepted");
    } else if (!strncmp(path, "/api/v2/commands/", 17)) {
        ldg::Id id;
        if (!ldg::parseId(path + 17, id, true)) {
            error(req, 400, "invalid_id");
            return;
        }
        if (req->method == HTTP_DELETE) {
            if (!cancel(id)) {
                error(req, 409, "already_started_or_unknown");
                return;
            }
        }
        ldg::Operation op;
        if (!operation(id, op)) {
            error(req, 404, "unknown_operation");
            return;
        }
        writeOperation(out, op);
        json(req, out);
    } else if (!strncmp(path, "/api/v2/config/", 15)) {
        const char *section = path + 15;
        if (req->method == HTTP_POST) {
            auto in = input.doc.as<JsonVariantConst>();
            if (!in["revision"].is<uint32_t>()) {
                error(req, 400, "revision_required");
                return;
            }
            bool restart = false;
            auto result = saveSettings(section, in["value"], in["revision"], restart);
            if (result != ldg::Result::Success) {
                error(req, result == ldg::Result::TargetChanged ? 409 : 400,
                      result == ldg::Result::TargetChanged ? "config_conflict"
                                                           : ldg::resultName(result));
                return;
            }
            out["restart_required"] = restart;
        }
        readSettings(section, out);
        json(req, out);
    } else if (!strcmp(path, "/api/v2/diagnostics")) {
        auto s = snapshot();
        out["free_internal"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        out["largest_internal"] =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        out["min_internal"] =
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        out["owner_max_gap_us"] = ownerMaxGapUs.load();
        out["owner_max_work_us"] = ownerMaxWorkUs.load();
        auto stacks = out["stack_free_bytes"].to<JsonObject>();
        const char *names[] = {"main_lcd", "owner",     "network",
                               "http_api", "http_bulk", "http_live"};
        for (unsigned i = 0; i < 6; ++i)
            if (observedTasks[i])
                stacks[names[i]] = uxTaskGetStackHighWaterMark(observedTasks[i]);
        out["uart_frames"] = s.frames;
        out["uart_errors"] = s.errors;
        out["mqtt_connected"] = mqttConnected.load();
        out["mqtt_stopping"] = mqttStopping.load();
        out["mqtt_allocated"] = mqttAllocated.load();
        out["settings_save_pending"] = managedToSave.load() >= 0;
        out["light_raw"] = lightRaw.load();
        out["measured_voltage"] = measuredVoltage.load();
        uint8_t mac[6];
        char identity[13], base[32];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(identity, sizeof(identity), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        snprintf(base, sizeof(base), "ldg/tuner/%s", identity);
        out["device_id"] = identity;
        out["mqtt_base_topic"] = base;
        deviceIdentity(out);
        networkDiagnostics(out["mqtt"].to<JsonObject>());
        lcdDiagnostics(out["lcd"].to<JsonObject>());
        out["reset_reason"] = esp_reset_reason();
        out["uptime_ms"] = nowMs();
        json(req, out);
    } else if (!strcmp(path, "/api/v2/mqtt")) {
        networkDiagnostics(out.to<JsonObject>());json(req,out);
    } else if (!strcmp(path, "/api/v2/network/scan")) {
        if (req->method==HTTP_POST && !requestWifiScan()) {error(req,409,"busy");return;}
        wifiScanJson(out);json(req,out,req->method==HTTP_POST?"202 Accepted":"200 OK");
    } else if (!strcmp(path, "/api/v2/reset") && req->method==HTTP_POST) {
        if (input.doc["confirm"]!=true) {error(req,400,"confirmation_required");return;}
        holdMaintenance();
        if (!maintenanceSafe()) {error(req,409,"busy");return;}
        const auto result=resetSettings();
        if (result!=ldg::Result::Success) {error(req,500,"settings_save_failed");return;}
        out["accepted"]=true;json(req,out);scheduleRestart();
    } else if (!strcmp(path, "/api/v2/mqtt/reconnect") && req->method == HTTP_POST) {
        ++mqttReconnect;
        out["accepted"] = true;
        json(req, out, "202 Accepted");
    } else if (!strcmp(path, "/api/v2/restart") && req->method == HTTP_POST) {
        out["accepted"] = true;
        json(req, out);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (!strcmp(path, "/api/v2/update")) {
        auto u = updateStatus();
        out["stage"] = u.stage;
        out["error"] = u.error;
        out["active"] = u.active;
        out["bytes"] = u.bytes;
        out["total"] = u.total;
        json(req, out);
    } else if (!strcmp(path, "/api/v2/update/prepare") && req->method == HTTP_POST) {
        if (!updateBegin(input.doc.as<JsonVariantConst>())) {
            auto status = updateStatus();
            error(req, 409, status.error[0] ? status.error : "busy");
            return;
        }
        out["accepted"] = true;
        json(req, out, "202 Accepted");
    } else if (!strcmp(path, "/api/v2/update/cancel") && req->method == HTTP_POST) {
        if (!updateCancel()) {
            error(req, 409, "already_activated");
            return;
        }
        out["cancelled"] = true;
        json(req, out);
    } else
        error(req, 404, "unknown_endpoint");
}
static void bulk(httpd_req_t *req) {
    if (!strcmp(req->uri, "/api/v2/update/upload") && req->method == HTTP_POST) {
        uint8_t buffer[4096];
        size_t received = 0;
        bool ok = true;
        uint32_t progress = nowMs();
        if (!updateStatus().active || req->content_len != updateStatus().total) {
            error(req, 409, "prepare_required");
            return;
        }
        while (received < req->content_len) {
            int n = httpd_req_recv(req, reinterpret_cast<char *>(buffer),
                                   std::min(sizeof(buffer), req->content_len - received));
            if (n > 0) {
                ok = updateWrite(buffer, n);
                received += n;
                progress = nowMs();
                if (!ok)
                    break;
            } else if (n != HTTPD_SOCK_ERR_TIMEOUT || nowMs() - progress > 10000) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            updateCancel();
            error(req, 400, "upload_interrupted");
            return;
        }
        if (!updateFinish()) {
            error(req, 400, updateStatus().error);
            return;
        }
        httpd_resp_sendstr(req, "{\"result\":\"success\"}");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return;
    }
    if (!strcmp(req->uri, "/api/v2/update/download") && req->method == HTTP_POST) {
        BoundedJson in;
        if (!body(req, in.doc) || !in.doc["url"].is<const char *>()) {
            error(req, 400, "invalid_url");
            return;
        }
        updateDownload(in.doc["url"]);
        if (strcmp(updateStatus().stage, "ready")) {
            error(req, 400, updateStatus().error);
            return;
        }
        httpd_resp_sendstr(req, "{\"result\":\"success\"}");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
        return;
    }
    char assetPath[192];
    strlcpy(assetPath, req->uri, sizeof(assetPath));
    if (auto query = strchr(assetPath, '?'))
        *query = 0;
    const web::Asset *asset = web::find(assetPath);
    if (!asset) {
        error(req, 404, "asset_not_found");
        return;
    }
    httpd_resp_set_type(req, asset->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    httpd_resp_set_hdr(req, "Cache-Control",
                       !strcmp(assetPath, "/") ? "no-cache"
                                               : "public, max-age=31536000, immutable");
    for (size_t i = 0; i < asset->size; i += 512)
        if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(asset->data + i),
                                  std::min(size_t(512), asset->size - i)) != ESP_OK)
            return;
    httpd_resp_send_chunk(req, nullptr, 0);
}
static void worker(void *queue) {
    httpd_req_t *req;
    for (;;)
        if (xQueueReceive(static_cast<QueueHandle_t>(queue), &req, portMAX_DELAY)) {
            if (queue == bulkQueue)
                bulk(req);
            else
                api(req);
            httpd_req_async_handler_complete(req);
            --activeHttp;
        }
}
static esp_err_t dispatch(httpd_req_t *req) {
    if (req->method == HTTP_GET &&
        (!strcmp(req->uri, "/generate_204") || !strcmp(req->uri, "/hotspot-detect.html") ||
         !strcmp(req->uri, "/ncsi.txt") || !strcmp(req->uri, "/connecttest.txt"))) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, nullptr, 0);
    }
    if (!authenticated(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"LDGControl\"");
        return error(req, 401, "authentication_required");
    }
    bool isBulk = strncmp(req->uri, "/api/", 5) || !strcmp(req->uri, "/api/v2/update/upload") ||
                  !strcmp(req->uri, "/api/v2/update/download");
    if (activeHttp.fetch_add(1) >= 4) {
        --activeHttp;
        return error(req, 503, "busy");
    }
    httpd_req_t *copy = nullptr;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        --activeHttp;
        return error(req, 503, "busy");
    }
    auto queue = isBulk ? bulkQueue : apiQueue;
    if (xQueueSend(queue, &copy, 0) != pdTRUE) {
        error(copy, 503, "busy");
        httpd_req_async_handler_complete(copy);
        --activeHttp;
    }
    return ESP_OK;
}
static void live(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(updating ? 1000 : 250));
        if (updating)
            continue;
        BoundedJson document;
        stateJson(document.doc, snapshot());
        char payload[3072];
        if (measureJson(document.doc) >= sizeof(payload) || document.doc.overflowed())
            continue;
        size_t n = serializeJson(document.doc, payload, sizeof(payload));
        Client copy[2];
        portENTER_CRITICAL(&wsMutex);
        memcpy(copy, clients, sizeof(copy));
        portEXIT_CRITICAL(&wsMutex);
        for (auto c : copy)
            if (c.fd >= 0 && httpd_ws_get_fd_info(server, c.fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_frame_t frame{};
                frame.type = HTTPD_WS_TYPE_TEXT;
                frame.payload = reinterpret_cast<uint8_t *>(payload);
                frame.len = n;
                xSemaphoreTake(wsWriteMutex, portMAX_DELAY);
                auto result = httpd_ws_send_frame_async(server, c.fd, &frame);
                xSemaphoreGive(wsWriteMutex);
                if (result != ESP_OK)
                    httpd_sess_trigger_close(server, c.fd);
            }
    }
}
void startHttp() {
    wsWriteMutex = xSemaphoreCreateMutex();
    apiQueue = xQueueCreate(2, sizeof(httpd_req_t *));
    bulkQueue = xQueueCreate(1, sizeof(httpd_req_t *));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 6;
    cfg.max_uri_handlers = 6;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 4096;
    cfg.core_id = 0;
    cfg.recv_wait_timeout = 1;
    cfg.send_wait_timeout = 1;
    cfg.open_fn = opened;
    cfg.close_fn = closed;
    if (httpd_start(&server, &cfg) != ESP_OK)
        return;
    httpd_uri_t ws{};
    ws.uri = "/api/v2/ws";
    ws.method = HTTP_GET;
    ws.handler = websocket;
    ws.is_websocket = true;
    ws.handle_ws_control_frames = true;
    ws.ws_pre_handshake_cb = handshake;
    httpd_register_uri_handler(server, &ws);
    for (auto method : {HTTP_GET, HTTP_POST, HTTP_DELETE}) {
        httpd_uri_t uri{};
        uri.uri = "/*";
        uri.method = method;
        uri.handler = dispatch;
        httpd_register_uri_handler(server, &uri);
    }
    bool ok = xTaskCreatePinnedToCore(worker, "http_api", 8192, apiQueue, 2, &observedTasks[3],
                                      0) == pdPASS;
    ok &= xTaskCreatePinnedToCore(worker, "http_bulk", 10240, bulkQueue, 1, &observedTasks[4], 0) ==
          pdPASS;
    ok &= xTaskCreatePinnedToCore(live, "http_live", 6144, nullptr, 1, &observedTasks[5], 0) ==
          pdPASS;
    httpReady = ok;
}
} // namespace app
