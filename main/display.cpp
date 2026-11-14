// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime.hpp"
#if !LDG_BOARD_headless
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st7796.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "mqtt_protocol.h"
#include "display_fonts.hpp"
#include "meter_mapping.h"
#include <cmath>
#include <cstdarg>
#if LDG_BOARD_guition
#include "esp_lcd_st7701.h"
#include "esp_lcd_panel_io_additions.h"
#endif
namespace app {
static esp_lcd_panel_handle_t panel;
static lv_display_t *display;
static lv_obj_t *screen, *otaScreen, *forward, *reflected, *swr, *status, *fwdBar, *refBar,
    *otaLabel, *toast, *modeLabel, *frequency, *bandLabel, *radioLabel, *wifiLabel, *sourceLabel,
    *scaleLabel, *ant1Button, *ant2Button, *ant1Name, *ant2Name, *bypassButton, *modeButton;
// Palette and control positions follow Remote-LDGControl's Sunton screen.
static constexpr uint32_t background = 0x0a111c, foreground = 0xeff5ff, muted = 0x8fabc3;
static constexpr uint32_t forwardColor = 0x64cfff, reflectedColor = 0xf0b85c;
static constexpr uint32_t positive = 0x63dfb5, warning = 0xf0b85c, danger = 0xff6b6b;
static constexpr uint32_t border = 0x294156, surface = 0x101e2b, modeSurface = 0x183329;
static i2c_master_dev_handle_t touch;
static i2c_master_bus_handle_t touchBus;
static ldg::BrightnessControl brightness;
static unsigned touchFailures = 0;
static uint32_t touchProbeAt = 0;
static uint32_t updateAt = 0, frameAt = 0, toastAt = 0, lastTouch = 0;
static int width = 480, height = 320;
static bool ready = false, pressed = false;
static int touchX = 0, touchY = 0;
static uint8_t rotation = 0;
static ldg::Id pending{};
static bool commandPending = false;
[[maybe_unused]] static bool flushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
                                       void *context) {
    lv_display_flush_ready(static_cast<lv_display_t *>(context));
    return false;
}
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels) {
#if LDG_BOARD_sunton
    lv_draw_sw_rgb565_swap(pixels, (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
#endif
    if (esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixels) !=
        ESP_OK)
        lv_display_flush_ready(d);
#if LDG_BOARD_guition
    else
        lv_display_flush_ready(d);
#endif
}
static bool readReg(uint16_t reg, uint8_t *bytes, size_t size) {
    uint8_t addr[] = {uint8_t(reg >> 8), uint8_t(reg)};
    return touch && i2c_master_transmit_receive(touch, addr, 2, bytes, size, 15) == ESP_OK;
}
static void probeTouch() {
    if (touch || !touchBus || nowMs() - touchProbeAt < 1000)
        return;
    touchProbeAt = nowMs();
    for (unsigned address : {0x5d, 0x14})
        if (i2c_master_probe(touchBus, address, 20) == ESP_OK) {
            i2c_device_config_t cfg{};
            cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            cfg.device_address = address;
            cfg.scl_speed_hz = 400000;
            if (i2c_master_bus_add_device(touchBus, &cfg, &touch) == ESP_OK)
                touchFailures = 0;
            break;
        }
}
static void touchRead(lv_indev_t *, lv_indev_data_t *data) {
    uint8_t status = 0;
    const uint32_t now = nowMs();
    if (readReg(0x814e, &status, 1)) {
        touchFailures = 0;
        if (status & 0x80) {
            uint8_t record[8]{};
            pressed = false;
            if ((status & 15) == 1 && readReg(0x814f, record, sizeof(record))) {
                int x = record[1] | record[2] << 8, y = record[3] | record[4] << 8;
#if LDG_BOARD_sunton
                if (x < 320 && y < 480) {
                    touchX = y;
                    touchY = 319 - x;
                    pressed = true;
                }
#else
                if (x < 480 && y < 480) {
                    touchX = x;
                    touchY = y;
                    pressed = true;
                }
#endif
            }
            uint8_t clear[] = {0x81, 0x4e, 0};
            if (i2c_master_transmit(touch, clear, 3, 15) != ESP_OK)
                pressed = false;
            lastTouch = now;
        } else if (now - lastTouch > 100)
            pressed = false;
    } else {
        pressed = false;
        if (touch && ++touchFailures >= 3) {
            i2c_master_bus_rm_device(touch);
            touch = nullptr;
        }
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = rotation ? width - 1 - touchX : touchX;
    data->point.y = rotation ? height - 1 - touchY : touchY;
}
static const char *displayAddress() {
    static char address[16] = "--";
    static uint32_t checkedAt = 0;
    if (nowMs() - checkedAt >= 1000) {
        checkedAt = nowMs();
        auto netif = esp_netif_get_handle_from_ifkey(networkUp ? "WIFI_STA_DEF" : "WIFI_AP_DEF");
        esp_netif_ip_info_t ip{};
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr)
            snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
        else
            strcpy(address, "--");
    }
    return address;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       const lv_font_t *font = &ldg_font_14) {
    auto o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    if (font)
        lv_obj_set_style_text_font(o, font, 0);
    return o;
}
static void changedLabel(lv_obj_t *label, const char *format, ...) {
    char value[128];
    va_list args;
    va_start(args, format);
    vsnprintf(value, sizeof(value), format, args);
    va_end(args);
    if (strcmp(value, lv_label_get_text(label)))
        lv_label_set_text(label, value);
}
static void notify(const char *text) {
    lv_label_set_text(toast, text);
    lv_obj_set_hidden(toast, false);
    lv_obj_set_hidden(status, true);
    toastAt = nowMs();
}
static void command(lv_event_t *event) {
    auto code = uint8_t(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    auto s = snapshot();
    ldg::Request req;
    esp_fill_random(req.id.data(), 16);
    req.target = s.target;
    req.command = code == 0xff ? ldg::nextModeCommand(ldg::operatingMode(s.tuner)) : code;
    if (code == 'P' && s.tuner.mode == MODE_BYPASS)
        req.command = s.tuner.tuningManaged ? TUNER_CMD_CONTROLLED_MODE : TUNER_CMD_AUTO_MODE;
    req.expires = s.uptime + 2000;
    ldg::Operation op;
    auto result = submit(req, op);
    if (result == ldg::Result::Success) {
        pending = req.id;
        commandPending = true;
        notify("Command accepted");
    } else
        notify(ldg::resultName(result));
}
static int bodyY(int y) {
    return 42 + (y - 42) * (height - 66) / 254;
}
static int bodyHeight(int h) {
    return h * (height - 66) / 254;
}
static void place(lv_obj_t *object, int x, int y, int w, int h) {
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
}
static lv_obj_t *button(const char *name, int x, int y, int w, int h, uint8_t code,
                        uint32_t fill = surface) {
    auto b = lv_button_create(screen);
    lv_obj_remove_style_all(b);
    place(b, x, bodyY(y), w, bodyHeight(h));
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x365779), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_text_font(b, &ldg_font_14, 0);
    lv_obj_set_style_text_color(b, lv_color_hex(foreground), 0);
    lv_obj_set_scrollable(b, false);
    auto t = lv_label_create(b);
    lv_label_set_text(t, name);
    lv_obj_center(t);
    lv_obj_add_event_cb(b, command, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t(code)));
    return b;
}
static lv_obj_t *antennaName(lv_obj_t *b) {
    lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_TOP_MID, 0, 7);
    auto name = label(b, "", 0, 0, &ldg_font_10);
    lv_obj_set_style_text_color(name, lv_color_hex(muted), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 64);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -7);
    return name;
}
static void divider(int y) {
    auto line = lv_obj_create(screen);
    lv_obj_remove_style_all(line);
    place(line, 14, y, 452, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(border), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_clickable(line, false);
}
static void createUi() {
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(background), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(foreground), 0);
    lv_obj_set_style_text_font(screen, &ldg_font_14, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    divider(37);
    divider(height - 23);
    label(screen, "LDG", 14, 10);
    frequency = label(screen, "--", 64, 10);
    place(frequency, 64, 10, 145, 22);
    lv_label_set_long_mode(frequency, LV_LABEL_LONG_CLIP);
    bandLabel = label(screen, "--", 214, 12, &ldg_font_10);
    place(bandLabel, 214, 12, 55, 14);
    modeLabel = label(screen, "--", 273, 7, &ldg_font_10);
    auto mode = modeLabel;
    place(mode, 273, 7, 76, 26);
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(mode, lv_color_hex(positive), 0);
    lv_obj_set_style_bg_opa(mode, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mode, lv_color_hex(modeSurface), 0);
    lv_obj_set_style_border_width(mode, 1, 0);
    lv_obj_set_style_border_color(mode, lv_color_hex(border), 0);
    lv_obj_set_style_radius(mode, 7, 0);
    lv_obj_set_style_pad_top(mode, 5, 0);
    radioLabel = label(screen, "LDG", 358, 5, &ldg_font_10);
    place(radioLabel, 358, 5, 74, 28);
    wifiLabel = label(screen, "Wi-Fi", 436, 13, &ldg_font_10);
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(muted), 0);
    auto title = label(screen, "SWR", 14, bodyY(53), &ldg_font_10);
    lv_obj_set_style_text_color(title, lv_color_hex(muted), 0);
    swr = label(screen, "--", 90, bodyY(43), &ldg_font_32);
    auto fwdTitle = label(screen, "FWD", 14, bodyY(105), &ldg_font_10);
    lv_obj_set_style_text_color(fwdTitle, lv_color_hex(muted), 0);
    forward = label(screen, "-- W", 14, bodyY(117), &ldg_font_32);
    auto refTitle = label(screen, "REF", 14, bodyY(184), &ldg_font_10);
    lv_obj_set_style_text_color(refTitle, lv_color_hex(muted), 0);
    reflected = label(screen, "-- W", 14, bodyY(196), &ldg_font_32);
    fwdBar = lv_bar_create(screen);
    refBar = lv_bar_create(screen);
    for (auto b : {fwdBar, refBar}) {
        lv_obj_remove_style_all(b);
        lv_bar_set_range(b, 0, 1000);
        lv_obj_set_size(b, 278, 12);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(surface), 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(b, lv_color_hex(b == fwdBar ? forwardColor : reflectedColor), LV_PART_INDICATOR);
        lv_obj_set_style_radius(b, 6, LV_PART_INDICATOR);
    }
    lv_obj_set_pos(fwdBar, 14, bodyY(159));
    lv_obj_set_pos(refBar, 14, bodyY(238));
    scaleLabel = label(screen, "", 14, bodyY(271), &ldg_font_10);
    lv_obj_set_style_text_color(scaleLabel, lv_color_hex(muted), 0);
    button("MEM TUNE", 314, 48, 152, 40, 'T');
    button("FULL TUNE", 314, 96, 152, 40, 'F');
    ant1Button = button("ANT 1", 314, 144, 72, 54, 0x81);
    ant2Button = button("ANT 2", 394, 144, 72, 54, 0x82);
    ant1Name = antennaName(ant1Button);
    ant2Name = antennaName(ant2Button);
    bypassButton = button("BYPASS ON", 314, 206, 152, 36, 'P');
    modeButton = button("SET MANUAL", 314, 250, 152, 36, 0xff, modeSurface);
    lv_obj_set_style_text_color(modeButton, lv_color_hex(positive), 0);
    status = label(screen, "Waiting for LDG", 14, height - 19, &ldg_font_10);
    place(status, 14, height - 19, 304, 14);
    lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
    sourceLabel = label(screen, "--", 330, height - 19, &ldg_font_10);
    place(sourceLabel, 330, height - 19, 136, 14);
    lv_obj_set_style_text_align(sourceLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(sourceLabel, lv_color_hex(muted), 0);
    toast = label(screen, "", 14, height - 19, &ldg_font_10);
    place(toast, 14, height - 19, 304, 14);
    lv_obj_set_style_text_color(toast, lv_color_hex(warning), 0);
    lv_label_set_long_mode(toast, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_hidden(toast, true);
    otaScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(otaScreen, lv_color_hex(background), 0);
    lv_obj_set_style_text_color(otaScreen, lv_color_hex(foreground), 0);
    label(otaScreen, "FIRMWARE UPDATE", 30, 45, &lv_font_montserrat_32);
    otaLabel = label(otaScreen, "Preparing", 30, 120, &lv_font_montserrat_32);
    lv_screen_load(screen);
}
void startDisplay() {
#if LDG_BOARD_guition
    height = 480;
#endif
    lv_init();
    lv_tick_set_cb(nowMs);
    display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    esp_lcd_panel_io_handle_t io = nullptr;
#if LDG_BOARD_sunton
    spi_bus_config_t bus{};
    bus.sclk_io_num = 14;
    bus.mosi_io_num = 13;
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 480 * 12 * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return;
    esp_lcd_panel_io_spi_config_t spi{};
    spi.cs_gpio_num = 15;
    spi.dc_gpio_num = 2;
    spi.spi_mode = 0;
    spi.pclk_hz = 40000000;
    spi.trans_queue_depth = 1;
    spi.lcd_cmd_bits = 8;
    spi.lcd_param_bits = 8;
    spi.on_color_trans_done = flushDone;
    spi.user_ctx = display;
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &spi, &io) != ESP_OK)
        return;
    esp_lcd_panel_dev_config_t cfg{};
    cfg.reset_gpio_num = -1;
    cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    cfg.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7796(io, &cfg, &panel) != ESP_OK)
        return;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, false);
    int backlight = 27, sda = 33, scl = 32;
    gpio_set_direction(GPIO_NUM_25, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_25, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_25, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
#else
    spi_line_config_t lines{};
    lines.cs_io_type = IO_TYPE_GPIO;
    lines.cs_gpio_num = 39;
    lines.scl_io_type = IO_TYPE_GPIO;
    lines.scl_gpio_num = 48;
    lines.sda_io_type = IO_TYPE_GPIO;
    lines.sda_gpio_num = 47;
    esp_lcd_panel_io_3wire_spi_config_t control = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(lines, 0);
    if (esp_lcd_new_panel_io_3wire_spi(&control, &io) != ESP_OK)
        return;
    esp_lcd_rgb_panel_config_t rgb{};
    rgb.clk_src = LCD_CLK_SRC_DEFAULT;
    rgb.data_width = 16;
    rgb.bits_per_pixel = 16;
    rgb.num_fbs = 1;
    rgb.bounce_buffer_size_px = 480 * 10;
    rgb.dma_burst_size = 64;
    rgb.hsync_gpio_num = 16;
    rgb.vsync_gpio_num = 17;
    rgb.de_gpio_num = 18;
    rgb.pclk_gpio_num = 21;
    rgb.disp_gpio_num = -1;
    int pins[] = {4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0};
    memcpy(rgb.data_gpio_nums, pins, sizeof(pins));
    rgb.timings.pclk_hz = 26000000;
    rgb.timings.h_res = 480;
    rgb.timings.v_res = 480;
    rgb.timings.hsync_pulse_width = 10;
    rgb.timings.hsync_back_porch = 10;
    rgb.timings.hsync_front_porch = 20;
    rgb.timings.vsync_pulse_width = 10;
    rgb.timings.vsync_back_porch = 10;
    rgb.timings.vsync_front_porch = 10;
    rgb.timings.flags.pclk_active_neg = true;
    rgb.flags.fb_in_psram = true;
    st7701_vendor_config_t vendor{};
    vendor.rgb_config = &rgb;
    vendor.flags.mirror_by_cmd = true;
    esp_lcd_panel_dev_config_t cfg{};
    cfg.reset_gpio_num = -1;
    cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    cfg.bits_per_pixel = 16;
    cfg.vendor_config = &vendor;
    if (esp_lcd_new_panel_st7701(io, &cfg, &panel) != ESP_OK)
        return;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    int backlight = 38, sda = 19, scl = 45;
#endif
    esp_lcd_panel_disp_on_off(panel, true);
    void *pixels = heap_caps_malloc(480 * 12 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!pixels)
        return;
    lv_display_set_buffers(display, pixels, nullptr, 480 * 12 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_0;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);
    ledc_channel_config_t channel{};
    channel.gpio_num = backlight;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 1023;
    ledc_channel_config(&channel);
    i2c_master_bus_config_t busConfig{};
    busConfig.i2c_port = I2C_NUM_0;
    busConfig.sda_io_num = static_cast<gpio_num_t>(sda);
    busConfig.scl_io_num = static_cast<gpio_num_t>(scl);
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&busConfig, &touchBus) == ESP_OK) {
        touchProbeAt = nowMs() - 1000;
        probeTouch();
    }
    createUi();
    auto input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, touchRead);
    ready = true;
}
void displayUpdateState(const char *, unsigned) {} // State is read by the sole LVGL owner.
void displayService() {
    if (!ready)
        return;
    const uint32_t now = nowMs();
    probeTouch();
    auto update = updateStatus();
    static char stage[24] = "";
    if (update.active || !strcmp(update.stage, "ready")) {
        if (lv_screen_active() != otaScreen) {
            lv_screen_load(otaScreen);
            updateAt = 0;
        }
        if (strcmp(stage, update.stage) || now - updateAt >= 1000) {
            updateAt = now;
            strcpy(stage, update.stage);
            changedLabel(otaLabel, "%s\n%u%%\n%u / %u", update.stage,
                         update.total ? unsigned(uint64_t(update.bytes) * 100 / update.total) : 0,
                         unsigned(update.bytes), unsigned(update.total));
        }
    } else {
        if (lv_screen_active() != screen) {
            lv_screen_load(screen);
            if (update.error[0])
                notify(update.error);
        }
        if (now - frameAt >= 100) {
            frameAt = now;
            auto s = snapshot();
            auto c = settings();
            if (rotation != c.device.displayOrientation) {
                rotation = c.device.displayOrientation;
                esp_lcd_panel_mirror(panel, rotation != 0, rotation != 0);
                lv_obj_invalidate(screen);
            }
            if (s.tuner.meterValid) {
                if (std::isfinite(s.tuner.meter.forward_power_watts))
                    changedLabel(forward, "%.1f W", s.tuner.meter.forward_power_watts);
                else
                    lv_label_set_text(forward, "-- W");
                if (std::isfinite(s.tuner.meter.reflected_power_watts))
                    changedLabel(reflected, "%.1f W", s.tuner.meter.reflected_power_watts);
                else
                    lv_label_set_text(reflected, "-- W");
                if (std::isfinite(s.tuner.meter.swr))
                    changedLabel(swr, "%.2f", s.tuner.meter.swr);
                else
                    lv_label_set_text(swr, "--");
                const double f = ldg::meterFraction(s.tuner.meter.forward_power_watts, c.meter.watts, c.meter.logarithmic);
                const double r = ldg::meterFraction(s.tuner.meter.reflected_power_watts, c.meter.watts, c.meter.logarithmic);
                lv_bar_set_value(fwdBar, int(f * 1000), LV_ANIM_OFF);
                lv_bar_set_value(refBar, int(r * 1000), LV_ANIM_OFF);
            } else {
                lv_label_set_text(forward, "-- W");
                lv_label_set_text(reflected, "-- W");
                lv_label_set_text(swr, "--");
                lv_bar_set_value(fwdBar, 0, LV_ANIM_OFF);
                lv_bar_set_value(refBar, 0, LV_ANIM_OFF);
            }
            const double ratio = s.tuner.meterValid ? s.tuner.meter.swr : NAN;
            lv_obj_set_style_text_color(swr, lv_color_hex(!std::isfinite(ratio) ? muted :
                ratio > 2.0 ? danger : ratio > 1.5 ? warning : foreground), 0);
            const auto mode = ldg::operatingMode(s.tuner);
            const char *modeText = mode == MODE_AUTO ? "AUTO" : mode == MODE_MANUAL ? "MANUAL" :
                mode == MODE_CONTROLLED ? "CONTROLLED" : mode == MODE_BYPASS ? "BYPASS" : "--";
            auto badge = modeLabel;
            changedLabel(badge, "%s", modeText);
            lv_obj_set_style_text_color(badge, lv_color_hex(mode == MODE_AUTO ? positive :
                mode == MODE_CONTROLLED ? forwardColor : mode == MODE_UNKNOWN ? muted : warning), 0);
            changedLabel(lv_obj_get_child(modeButton, 0), "%s", mode == MODE_MANUAL ? "SET AUTO" :
                mode == MODE_AUTO ? "CONTROLLED" : "SET MANUAL");
            changedLabel(lv_obj_get_child(bypassButton, 0), "%s", s.tuner.mode == MODE_BYPASS ? "BYPASS OFF" : "BYPASS ON");
            changedLabel(ant1Name, "%s", c.device.ant1Name);
            changedLabel(ant2Name, "%s", c.device.ant2Name);
            for (auto b : {ant1Button, ant2Button}) {
                const auto antenna = b == ant1Button ? ANT_A : ANT_B;
                const bool selected = s.tuner.antenna == antenna;
                const bool uncertain = s.tuner.antenna == ANT_UNKNOWN && ldg::lastKnownAntenna(s.tuner) == antenna;
                const auto accent = selected ? forwardColor : uncertain ? warning : border;
                lv_obj_set_style_bg_color(b, lv_color_hex(selected ? 0x122f43 : uncertain ? 0x302113 : surface), 0);
                lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
                lv_obj_set_style_text_color(b, lv_color_hex(selected || uncertain ? accent : foreground), 0);
            }
            changedLabel(bandLabel, "%s", ldg::bandName(ldg::lastKnownBand(s.tuner)));
            const auto &radio = s.tuner.radio;
            if (radio.frequencyValid && radio.connection == ldg::RadioConnection::Ready)
                changedLabel(frequency, "%.6f", double(radio.txFrequencyHz) / 1000000);
            else
                changedLabel(frequency, "%s", c.device.wifiSSID[0] ? c.device.wifiSSID : "--");
            changedLabel(radioLabel, "%s\n%s/%s", radio.provider == ldg::RadioProvider::Tci ?
                (radio.transmitting ? "TCI TX" : "TCI RX") : "LDG", ldg::radioConnectionName(radio.connection),
                s.remote ? "NET" : "LDG");
            lv_obj_set_style_text_color(radioLabel, lv_color_hex(radio.connection == ldg::RadioConnection::Ready ? positive : muted), 0);
            lv_obj_set_style_text_color(wifiLabel, lv_color_hex(networkUp ? muted : danger), 0);
            changedLabel(sourceLabel, "%s", displayAddress());
            changedLabel(status, "ANT: %s", ldg::antennaInitName(s.tuner.antennaInit));
            changedLabel(scaleLabel, "%u W%s", unsigned(c.meter.watts), c.meter.logarithmic ? " LOG" : "");
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                          brightness.update(c.brightness, lightRaw.load()) * 1023 / 100);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        }
        if (commandPending) {
            ldg::Operation op;
            if (operation(pending, op) && op.state == ldg::OperationState::Done) {
                notify(ldg::resultName(op.result));
                commandPending = false;
            }
        }
        if (now - toastAt > 8000) {
            lv_obj_set_hidden(toast, true);
            lv_obj_set_hidden(status, false);
        }
    }
    lv_timer_handler();
}
} // namespace app
#else
namespace app {
void startDisplay() {}
void displayService() {}
void displayUpdateState(const char *, unsigned) {}
} // namespace app
#endif
