// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "image_stream.hpp"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include <cstring>
#include <cstdio>
#include <atomic>
extern const unsigned char release_key_start[] asm("_binary_release_public_pem_start");
extern const unsigned char release_key_end[] asm("_binary_release_public_pem_end");
namespace app {
static StaticSemaphore_t mutexStorage;
static SemaphoreHandle_t mutex = nullptr;
static UpdateStatus status;
static esp_ota_handle_t handle = 0;
static const esp_partition_t *partition = nullptr;
static mbedtls_sha256_context digest;
static uint8_t expectedHash[32];
static std::atomic<bool> cancelled{false};
static uint32_t lastProgress = 0;
static ldg::ImagePrefix<sizeof(esp_image_header_t)> prefix;
void updateInit() {
    if (!mutex) {
        mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
        mbedtls_sha256_init(&digest);
    }
}
static void fail(const char *reason) {
    strlcpy(status.error, reason, sizeof(status.error));
    strcpy(status.stage, "error");
    status.active = false;
    if (handle) {
        esp_ota_abort(handle);
        handle = 0;
    }
    updating = false;
}
UpdateStatus updateStatus() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto result = status;
    xSemaphoreGive(mutex);
    return result;
}
static const char *board() {
#if LDG_BOARD_sunton
    return "sunton";
#elif LDG_BOARD_guition
    return "guition";
#else
    return "headless";
#endif
}
bool updateBegin(JsonVariantConst manifest) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (status.active || updating) {
        xSemaphoreGive(mutex);
        return false;
    }
    status = UpdateStatus{};
    cancelled = false;
    const char *target = manifest["board"], *version = manifest["version"],
               *hash = manifest["sha256"], *signature = manifest["signature"];
    if (manifest["format"] != 1 || !target || strcmp(target, board()) || !version ||
        strlen(version) > 31 || strchr(version, '\n') || !hash || strlen(hash) != 64 ||
        !signature || strlen(signature) > 128 || !manifest["size"].is<uint32_t>()) {
        fail("invalid_manifest");
        xSemaphoreGive(mutex);
        return false;
    }
    status.total = manifest["size"];
    partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || status.total < 256 || status.total > partition->size) {
        fail("image_size");
        xSemaphoreGive(mutex);
        return false;
    }
    for (unsigned i = 0; i < 32; ++i) {
        auto hex = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        int a = hex(hash[2 * i]), b = hex(hash[2 * i + 1]);
        if (a < 0 || b < 0) {
            fail("invalid_hash");
            xSemaphoreGive(mutex);
            return false;
        }
        expectedHash[i] = uint8_t(a * 16 + b);
    }
    char signedText[200];
    int n = snprintf(signedText, sizeof(signedText), "LDGControl-IDF\n1\n%s\n%s\n%u\n%s\n", target,
                     version, unsigned(status.total), hash);
    uint8_t sha[32], sig[96];
    size_t sigSize = 0;
    mbedtls_sha256(reinterpret_cast<uint8_t *>(signedText), n, sha, 0);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool verified = mbedtls_base64_decode(sig, sizeof(sig), &sigSize,
                                          reinterpret_cast<const uint8_t *>(signature),
                                          strlen(signature)) == 0 &&
                    mbedtls_pk_parse_public_key(&key, release_key_start,
                                                release_key_end - release_key_start) == 0 &&
                    mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, sha, sizeof(sha), sig, sigSize) == 0;
    mbedtls_pk_free(&key);
    if (!verified) {
        fail("signature_invalid");
        xSemaphoreGive(mutex);
        return false;
    }
    // The API returns immediately. Cleanup runs in the existing network worker.
    strcpy(status.stage, "waiting");
    status.active = true;
    prefix.reset();
    lastProgress = nowMs();
    xSemaphoreGive(mutex);
    return true;
}
void updateService() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!status.active) {
        xSemaphoreGive(mutex);
        return;
    }
    if (cancelled || nowMs() - lastProgress > 60000) {
        fail(cancelled ? "cancelled" : "update_idle_timeout");
        xSemaphoreGive(mutex);
        return;
    }
    if (!strcmp(status.stage, "waiting")) {
        bool safe = pauseForUpdate();
        if (safe) {
            strcpy(status.stage, "preparing");
            lastProgress = nowMs();
        }
        xSemaphoreGive(mutex);
        return;
    }
    if (strcmp(status.stage, "preparing")) {
        xSemaphoreGive(mutex);
        return;
    }
    if (mqttAllocated || tciAllocated) {
        if (nowMs() - lastProgress > 30000)
            fail("network_cleanup_timeout");
        xSemaphoreGive(mutex);
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 24576) {
        fail("memory_budget");
        xSemaphoreGive(mutex);
        return;
    }
    if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK ||
        mbedtls_sha256_starts(&digest, 0)) {
        fail("ota_begin_failed");
        xSemaphoreGive(mutex);
        return;
    }
    strcpy(status.stage, "receiving");
    lastProgress = nowMs();
    xSemaphoreGive(mutex);
}
bool updateWrite(const uint8_t *bytes, size_t n) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok =
        status.active && handle && !cancelled && n <= 4096 && n <= status.total - status.bytes;
    const uint8_t *remaining = bytes;
    size_t rest = n;
    if (ok && !prefix.complete()) {
        size_t take = prefix.append(remaining, rest);
        remaining += take;
        rest -= take;
        if (prefix.complete()) {
            esp_image_header_t header{};
            memcpy(&header, prefix.data(), sizeof(header));
            ok = header.magic == ESP_IMAGE_HEADER_MAGIC;
#if CONFIG_IDF_TARGET_ESP32S3
            ok = ok && header.chip_id == ESP_CHIP_ID_ESP32S3;
#else
            ok = ok && header.chip_id == ESP_CHIP_ID_ESP32;
#endif
            if (ok)
                ok = esp_ota_write(handle, prefix.data(), sizeof(esp_image_header_t)) == ESP_OK;
        }
    }
    if (ok && rest)
        ok = esp_ota_write(handle, remaining, rest) == ESP_OK;
    if (ok)
        ok = mbedtls_sha256_update(&digest, bytes, n) == 0;
    if (ok) {
        status.bytes += n;
        lastProgress = nowMs();
    } else
        fail(cancelled ? "cancelled" : "image_write_failed");
    xSemaphoreGive(mutex);
    return ok;
}
bool updateFinish() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    uint8_t hash[32];
    bool ok = status.active && handle && !cancelled && status.bytes == status.total &&
              mbedtls_sha256_finish(&digest, hash) == 0 && !memcmp(hash, expectedHash, 32);
    if (ok) {
        strcpy(status.stage, "verifying");
        auto h = handle;
        handle = 0;
        ok = esp_ota_end(h) == ESP_OK && !cancelled &&
             esp_ota_set_boot_partition(partition) == ESP_OK;
    }
    if (ok) {
        strcpy(status.stage, "ready");
        status.active = false;
    } else
        fail("image_verification_failed");
    xSemaphoreGive(mutex);
    return ok;
}
bool updateCancel() {
    cancelled = true;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!strcmp(status.stage, "ready")) {
        xSemaphoreGive(mutex);
        return false;
    }
    if (status.active)
        fail("cancelled");
    xSemaphoreGive(mutex);
    return true;
}
void updateDownload(const char *url) {
    if (strncmp(url, "https://", 8) || strlen(url) > 1024 || !updateStatus().active) {
        updateCancel();
        return;
    }
    // Explicit bounded Range requests, allowing GitHub's HTTPS redirects.
    uint8_t bytes[4096];
    uint32_t offset = 0;
    bool ok = true;
    while (offset < updateStatus().total && !cancelled) {
        const uint32_t remaining = updateStatus().total - offset;
        const unsigned amount = std::min(uint32_t(sizeof(bytes)), remaining);
        char range[64];
        snprintf(range, sizeof(range), "bytes=%u-%u", unsigned(offset),
                 unsigned(offset + amount - 1));
        esp_http_client_config_t cfg{};
        cfg.url = url;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = 15000;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 1024;
        ldg::ContentRange response;
        cfg.user_data = &response;
        cfg.event_handler = [](esp_http_client_event_t *event) -> esp_err_t {
            if (event->event_id == HTTP_EVENT_ON_HEADER &&
                !strcasecmp(event->header_key, "Content-Range")) {
                *static_cast<ldg::ContentRange *>(event->user_data) =
                    ldg::parseContentRange(event->header_value);
            }
            return ESP_OK;
        };
        auto client = esp_http_client_init(&cfg);
        if (!client) {
            ok = false;
            break;
        }
        esp_http_client_set_header(client, "Range", range);
        esp_err_t opened = ESP_FAIL;
        int code = 0;
        for (unsigned redirect = 0; redirect < 5; ++redirect) {
            response.valid = false;
            opened = esp_http_client_open(client, 0);
            if (opened != ESP_OK)
                break;
            esp_http_client_fetch_headers(client);
            code = esp_http_client_get_status_code(client);
            if (code < 300 || code >= 400)
                break;
            if (esp_http_client_set_redirection(client) != ESP_OK)
                break;
            char redirectUrl[1200];
            if (esp_http_client_get_url(client, redirectUrl, sizeof(redirectUrl)) != ESP_OK ||
                strncmp(redirectUrl, "https://", 8)) {
                opened = ESP_FAIL;
                break;
            }
            esp_http_client_close(client);
        }
        if (opened != ESP_OK || code != 206 || !response.valid || response.first != offset ||
            response.last != offset + amount - 1 || response.total != updateStatus().total ||
            esp_http_client_get_content_length(client) != amount) {
            esp_http_client_cleanup(client);
            ok = false;
            break;
        }
        unsigned got = 0;
        while (got < amount && !cancelled) {
            int n =
                esp_http_client_read(client, reinterpret_cast<char *>(bytes + got), amount - got);
            if (n <= 0)
                break;
            got += n;
        }
        esp_http_client_cleanup(client);
        if (got != amount || !updateWrite(bytes, got)) {
            ok = false;
            break;
        }
        offset += got;
    }
    if (ok && !cancelled)
        updateFinish();
    else {
        xSemaphoreTake(mutex, portMAX_DELAY);
        fail(cancelled ? "cancelled" : "download_failed");
        xSemaphoreGive(mutex);
    }
}
void updateBootCheck() {
    static bool checked = false;
    if (checked)
        return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        checked = true;
        return;
    }
    bool healthy = false;
    if (controller && xSemaphoreTake(controlMutex, 0) == pdTRUE) {
        healthy = controller->sequence() > 1000;
        xSemaphoreGive(controlMutex);
    }
    if (healthy && httpReady &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 16384) {
        checked = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
    } else if (nowMs() > 30000) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
} // namespace app
