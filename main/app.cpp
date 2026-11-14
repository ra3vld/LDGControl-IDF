// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#include "mqtt_protocol.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <new>
namespace app {
ldg::Controller *controller = nullptr;
SemaphoreHandle_t controlMutex;
ldg::Id bootId{};
std::atomic<bool> updating{false}, networkUp{false}, mqttConnected{false}, mqttAllocated{false},
    mqttStopping{false}, tciAllocated{false};
std::atomic<uint32_t> mqttReconnect{0};
std::atomic<int> managedToSave{-1};
TaskHandle_t observedTasks[6]{};
std::atomic<uint32_t> ownerMaxGapUs{0}, ownerMaxWorkUs{0};
static Snapshot remoteState{};
static uint32_t remoteAt = 0;
static bool remoteValid = false, uartReady = false;
static ldg::Operations operationJournal;
static Config applied;
static ldg::SimulatedSwitch simulated;
static QueueHandle_t uartEvents;
uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }
static bool uartWrite(void *, uint8_t b) {
    return uartReady && uart_tx_chars(UART_NUM_2, reinterpret_cast<const char *>(&b), 1) == 1;
}
uint8_t commandCode(const char *name) {
    if (!name)
        return 0;
    static constexpr const char *names[] = {
        "toggle", "memory_tune", "full_tune", "bypass",     "auto",
        "manual", "antenna_1",   "antenna_2", "controlled", "external_antenna"};
    static constexpr uint8_t values[] = {'A', 'T', 'F', 'P', 'C', 'M', 0x81, 0x82, 0x83, 0x85};
    for (unsigned i = 0; i < 10; ++i)
        if (!strcmp(name, names[i]))
            return values[i];
    return 0;
}
const char *commandName(uint8_t cmd) {
    switch (cmd) {
    case 'A':
        return "toggle";
    case 'T':
        return "memory_tune";
    case 'F':
        return "full_tune";
    case 'P':
        return "bypass";
    case 'C':
        return "auto";
    case 'M':
        return "manual";
    case 0x81:
        return "antenna_1";
    case 0x82:
        return "antenna_2";
    case 0x83:
        return "controlled";
    case 0x85:
        return "external_antenna";
    default:
        return "unknown";
    }
}
Snapshot snapshot() {
    Snapshot s;
    uint32_t now = nowMs();
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return s;
    if (applied.remoteEnabled) {
        s = remoteState;
        s.remote = true;
        s.tuner.remote = true;
        const uint32_t age = now - remoteAt;
        if (!remoteValid || age > 3000)
            s.tuner.meterValid = false;
        s.tuner.meterAgeMs =
            s.tuner.meterAgeMs > UINT32_MAX - age ? UINT32_MAX : s.tuner.meterAgeMs + age;
        s.tuner.radio.ageMs =
            s.tuner.radio.ageMs > UINT32_MAX - age ? UINT32_MAX : s.tuner.radio.ageMs + age;
        if (s.tuner.radio.ageMs > ldg::RadioState::LivenessMs) {
            s.tuner.radio.frequencyValid = false;
            s.tuner.radio.connection = ldg::RadioConnection::Disconnected;
        }
    } else {
        s.tuner = controller->state(now);
        s.target = controller->target();
        s.uptime = now;
        s.seq = controller->sequence();
        s.frames = controller->frames();
        s.errors = controller->errors();
        s.busy = controller->busy();
        s.antennaSwitch = controller->switchState();
        s.route = controller->routeReport(now);
        s.scale = applied.meter.watts;
        s.logarithmic = applied.meter.logarithmic;
    }
    xSemaphoreGive(controlMutex);
    return s;
}
ldg::Result submit(const ldg::Request &req, ldg::Operation &op) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return ldg::Result::Busy;
    unsigned index = 0;
    ldg::Result r;
    if (const auto *old = operationJournal.find(req.id)) {
        const auto &q = old->request;
        r = q.target == req.target && q.command == req.command && q.expires == req.expires &&
                    q.port == req.port
                ? ldg::Result::Success
                : ldg::Result::DuplicateConflict;
        if (r == ldg::Result::Success)
            op = *old;
    } else if (updating || usbMaintenance())
        r = ldg::Result::Busy;
    else if (applied.remoteEnabled) {
        if (!remoteValid || nowMs() - remoteAt > 3000)
            r = ldg::Result::Unavailable;
        else {
            const uint32_t remoteNow = remoteState.uptime + (nowMs() - remoteAt);
            r = operationJournal.accept(req, remoteState.target, remoteNow, index, nowMs());
            if (r == ldg::Result::Success)
                op = operationJournal.at(index);
        }
    } else if (!uartReady && req.command != ldg::SelectExternal)
        r = ldg::Result::Unavailable;
    else {
        r = controller->submit(req, nowMs(), index);
        if (r == ldg::Result::Success)
            op = controller->operations().at(index);
    }
    xSemaphoreGive(controlMutex);
    return r;
}
bool operation(const ldg::Id &id, ldg::Operation &out) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return false;
    auto p = controller->operations().find(id);
    if (!p)
        p = operationJournal.find(id);
    if (p)
        out = *p;
    xSemaphoreGive(controlMutex);
    return p != nullptr;
}
bool cancel(const ldg::Id &id) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return false;
    bool ok = controller->cancel(id, nowMs()) || operationJournal.cancel(id, nowMs());
    xSemaphoreGive(controlMutex);
    return ok;
}
void radioSnapshot(const ldg::RadioState &r) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        controller->radio(r);
        xSemaphoreGive(controlMutex);
    }
}
void remoteSnapshot(const Snapshot &s, bool valid, const char *expectedHost) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (!applied.remoteEnabled || strcmp(applied.device.remoteHost, expectedHost)) {
            xSemaphoreGive(controlMutex);
            return;
        }
        remoteState = s;
        remoteValid = valid;
        remoteAt = nowMs();
        xSemaphoreGive(controlMutex);
    }
}
bool takeRemoteOperation(ldg::Operation &out) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return false;
    int i =
        operationJournal.next(remoteState.target, remoteState.uptime + nowMs() - remoteAt, nowMs());
    if (i >= 0) {
        operationJournal.at(i).state = ldg::OperationState::Running;
        out = operationJournal.at(i);
    }
    xSemaphoreGive(controlMutex);
    return i >= 0;
}
void finishRemoteOperation(const ldg::Id &id, ldg::Result r) {
    if (xSemaphoreTake(controlMutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return;
    for (unsigned i = 0; i < ldg::Operations::Capacity; ++i)
        if (operationJournal.at(i).state == ldg::OperationState::Running &&
            operationJournal.at(i).request.id == id)
            operationJournal.finish(i, r, nowMs());
    xSemaphoreGive(controlMutex);
}
bool pauseForUpdate() {
    if (xSemaphoreTake(controlMutex, 0) != pdTRUE)
        return false;
    bool safe = controller->canPause(nowMs());
    if (applied.remoteEnabled && remoteValid && nowMs() - remoteAt < 3000) {
        const auto &s = remoteState.tuner;
        if (remoteState.busy || (s.meterAgeMs < 3000 && s.meter.forward_power_raw > 0) ||
            (s.radio.ownsBand() && (s.radio.transmitting || s.radio.tuning)))
            safe = false;
    }
    if (safe) {
        updating = true;
        controller->pause(true);
    }
    xSemaphoreGive(controlMutex);
    return safe;
}
void stateJson(JsonDocument &out, const Snapshot &s) {
    ldg::writeState(out, s.tuner, false);
    char token[33], boot[33];
    ldg::formatId(s.target, token);
    ldg::formatId(bootId, boot);
    auto names = out["antenna_names"].to<JsonArray>();
    auto config = settings();
    names.add(config.device.ant1Name);
    names.add(config.device.ant2Name);
    out["api_version"] = 2;
    out["boot_id"] = boot;
    out["source_token"] = token;
    out["sequence"] = s.seq;
    out["uptime_ms"] = s.uptime;
    out["busy"] = s.busy;
    out["source"] = s.remote ? "remote" : "local";
    out["connected"] = s.tuner.meterValid;
    out["scale_watts"] = s.scale;
    out["logarithmic"] = s.logarithmic;
    auto route = out["route"].to<JsonObject>();
    route["band"] = ldg::bandName(s.route.band);
    route["ldg"] = s.route.ldg;
    route["external"] = s.route.external;
    route["stage"] = s.route.stage;
    route["partial"] = s.route.partial;
    route["result"] = ldg::resultName(s.route.result);
    auto a = out["external_antenna"].to<JsonObject>();
    a["requested"] = s.antennaSwitch.requested;
    a["confirmed"] = s.antennaSwitch.confirmed;
    a["evidence"] = static_cast<unsigned>(s.antennaSwitch.evidence);
    a["busy"] = s.antennaSwitch.busy;
    a["result"] = ldg::resultName(s.antennaSwitch.result);
}
static void owner(void *) {
    uint8_t bytes[128];
    TickType_t wake = xTaskGetTickCount();
    uint32_t configAt = 0;
    uint64_t lastCycle = esp_timer_get_time();
    for (;;) {
        const uint64_t cycle = esp_timer_get_time();
        const uint32_t gap = cycle - lastCycle;
        lastCycle = cycle;
        if (gap > ownerMaxGapUs)
            ownerMaxGapUs = gap;
        Config next;
        const uint32_t now = nowMs();
        bool changed = false;
        if (now - configAt >= 100) {
            configAt = now;
            changed = trySettings(next) && next.revision != applied.revision;
        }
        if (xSemaphoreTake(controlMutex, 0) == pdTRUE) {
            if (changed) {
                const bool sourceChanged =
                    next.remoteEnabled != applied.remoteEnabled ||
                    strcmp(next.device.remoteHost, applied.device.remoteHost);
                applied = next;
                controller->configure(applied.antennas, applied.tuning, applied.routes,
                                      applied.device.meterPsuVoltage);
                controller->switchDriver(applied.simulateSwitch ? &simulated : nullptr);
                if (sourceChanged) {
                    ldg::Id token;
                    esp_fill_random(token.data(), token.size());
                    controller->changeTarget(token);
                    remoteValid = false;
                }
            }
            if (uartReady) {
                uart_event_t event;
                bool damaged = false;
                while (xQueueReceive(uartEvents, &event, 0) == pdTRUE)
                    if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                        event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
                        damaged = true;
                        controller->fault();
                    }
                if (damaged)
                    uart_flush_input(UART_NUM_2);
                else {
                    int count = uart_read_bytes(UART_NUM_2, bytes, sizeof(bytes), 0);
                    if (count > 0)
                        controller->feed(bytes, count, now);
                }
            }
            const float voltage = measuredVoltage.load();
            controller->voltage(voltage > 0 ? voltage : applied.device.meterPsuVoltage);
            controller->pause(updating || applied.remoteEnabled || usbMaintenance());
            controller->tick(now);
            bool managed;
            if (controller->takeManagedChange(managed)) {
                applied.tuning.managed = managed;
                managedToSave = managed ? 1 : 0;
            }
            xSemaphoreGive(controlMutex);
        }
        const uint32_t work = esp_timer_get_time() - cycle;
        if (work > ownerMaxWorkUs)
            ownerMaxWorkUs = work;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    }
}
void initialize() {
    observedTasks[0] = xTaskGetCurrentTaskHandle();
    updateInit();
    settingsInit();
    applied = settings();
    esp_fill_random(bootId.data(), bootId.size());
    controlMutex = xSemaphoreCreateMutex();
    alignas(ldg::Controller) static uint8_t memory[sizeof(ldg::Controller)];
    controller = new (memory) ldg::Controller({nullptr, uartWrite}, bootId, operationJournal);
    controller->configure(applied.antennas, applied.tuning, applied.routes,
                          applied.device.meterPsuVoltage);
    controller->switchDriver(applied.simulateSwitch ? &simulated : nullptr);
    if (applied.uartRx >= 0 && applied.uartTx >= 0) {
        uart_config_t cfg{};
        cfg.baud_rate = 38400;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        uartReady = uart_param_config(UART_NUM_2, &cfg) == ESP_OK &&
                    uart_set_pin(UART_NUM_2, applied.uartTx, applied.uartRx, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE) == ESP_OK &&
                    uart_driver_install(UART_NUM_2, 2048, 0, 8, &uartEvents, 0) == ESP_OK;
    }
    xTaskCreatePinnedToCore(owner, "ldg_owner", 6144, nullptr, 5, &observedTasks[1], 1);
    startNetwork();
    startHttp();
    startSensors();
    startDisplay();
    startUsb();
    ESP_LOGI("boot", "LDGControl-IDF 0.1.0; free internal=%u, largest=%u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
} // namespace app
extern "C" void app_main() {
    app::initialize();
    for (unsigned i = 0;; ++i) {
        app::serviceUsb();
        app::serviceSensors();
        app::displayService();
        if (i >= 1000 && i % 100 == 0)
            app::updateBootCheck();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
