/*
 * Copyright (c) 2025 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/*
 * 标定图结论：
 * 面板实际可见像素是 64x16，但 SSD1306/SSD1312 GDRAM 按 128x32 写入时会
 * 在 X 和 Y 两个方向折叠：物理像素 p/q 会同时收到 GDRAM 的 p 与 127-p、
 * q 与 31-q。所以之前无论做 180° 还是 Y-only 变换，都会变成叠影/错乱。
 *
 * 现在直接按物理 64x16 维护一张 framebuffer，并让 LVGL 的 flush 回调
 * 无条件把这张帧整屏写出去。这样不再依赖 LVGL 的局部 area，也不再做
 * 任何软件旋转：文字位置就是实机位置。
 */
#if LV_COLOR_DEPTH == 1
#define K30_OLED_WIDTH  64
#define K30_OLED_HEIGHT 16
#define K30_OLED_STRIDE K30_OLED_WIDTH

#define K30_GLYPH_W 5
#define K30_GLYPH_H 7
#define K30_CHAR_ADVANCE (K30_GLYPH_W + 1)

static const struct device *k30_display_dev;
static uint8_t k30_frame[K30_OLED_WIDTH * K30_OLED_HEIGHT / 8];
static bool k30_flush_installed;
static lv_obj_t *k30_status_screen;

K_MUTEX_DEFINE(k30_frame_mutex);

static const uint8_t k30_glyph_space[K30_GLYPH_W] = {0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k30_glyph_dash[K30_GLYPH_W] = {0x00, 0x00, 0x0E, 0x00, 0x00};
static const uint8_t k30_glyph_colon[K30_GLYPH_W] = {0x00, 0x0C, 0x0C, 0x00, 0x00};
static const uint8_t k30_glyph_percent[K30_GLYPH_W] = {0x23, 0x13, 0x08, 0x64, 0x62};
static const uint8_t k30_glyph_unknown[K30_GLYPH_W] = {0x3E, 0x51, 0x49, 0x45, 0x3E};

static const uint8_t k30_glyph_0[K30_GLYPH_W] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
static const uint8_t k30_glyph_1[K30_GLYPH_W] = {0x00, 0x42, 0x7F, 0x40, 0x00};
static const uint8_t k30_glyph_2[K30_GLYPH_W] = {0x42, 0x61, 0x51, 0x49, 0x46};
static const uint8_t k30_glyph_3[K30_GLYPH_W] = {0x21, 0x41, 0x45, 0x4B, 0x31};
static const uint8_t k30_glyph_4[K30_GLYPH_W] = {0x18, 0x14, 0x12, 0x7F, 0x10};
static const uint8_t k30_glyph_5[K30_GLYPH_W] = {0x27, 0x45, 0x45, 0x45, 0x39};
static const uint8_t k30_glyph_6[K30_GLYPH_W] = {0x3C, 0x4A, 0x49, 0x49, 0x30};
static const uint8_t k30_glyph_7[K30_GLYPH_W] = {0x01, 0x71, 0x09, 0x05, 0x03};
static const uint8_t k30_glyph_8[K30_GLYPH_W] = {0x36, 0x49, 0x49, 0x49, 0x36};
static const uint8_t k30_glyph_9[K30_GLYPH_W] = {0x06, 0x49, 0x49, 0x29, 0x1E};

static const uint8_t k30_glyph_A[K30_GLYPH_W] = {0x7E, 0x11, 0x11, 0x11, 0x7E};
static const uint8_t k30_glyph_B[K30_GLYPH_W] = {0x7F, 0x49, 0x49, 0x49, 0x36};
static const uint8_t k30_glyph_C[K30_GLYPH_W] = {0x3E, 0x41, 0x41, 0x41, 0x22};
static const uint8_t k30_glyph_E[K30_GLYPH_W] = {0x7F, 0x49, 0x49, 0x49, 0x41};
static const uint8_t k30_glyph_F[K30_GLYPH_W] = {0x7F, 0x09, 0x09, 0x09, 0x01};
static const uint8_t k30_glyph_L[K30_GLYPH_W] = {0x40, 0x40, 0x40, 0x40, 0x7F};
static const uint8_t k30_glyph_N[K30_GLYPH_W] = {0x7F, 0x20, 0x18, 0x04, 0x7F};
static const uint8_t k30_glyph_S[K30_GLYPH_W] = {0x46, 0x49, 0x49, 0x49, 0x31};
static const uint8_t k30_glyph_T[K30_GLYPH_W] = {0x01, 0x01, 0x7F, 0x01, 0x01};
static const uint8_t k30_glyph_U[K30_GLYPH_W] = {0x7F, 0x40, 0x40, 0x40, 0x7F};

static char k30_upper(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static const uint8_t *k30_glyph(char c)
{
    switch (k30_upper(c)) {
    case ' ':
        return k30_glyph_space;
    case '-':
        return k30_glyph_dash;
    case ':':
        return k30_glyph_colon;
    case '%':
        return k30_glyph_percent;
    case '0':
        return k30_glyph_0;
    case '1':
        return k30_glyph_1;
    case '2':
        return k30_glyph_2;
    case '3':
        return k30_glyph_3;
    case '4':
        return k30_glyph_4;
    case '5':
        return k30_glyph_5;
    case '6':
        return k30_glyph_6;
    case '7':
        return k30_glyph_7;
    case '8':
        return k30_glyph_8;
    case '9':
        return k30_glyph_9;
    case 'A':
        return k30_glyph_A;
    case 'B':
        return k30_glyph_B;
    case 'C':
        return k30_glyph_C;
    case 'E':
        return k30_glyph_E;
    case 'F':
        return k30_glyph_F;
    case 'L':
        return k30_glyph_L;
    case 'N':
        return k30_glyph_N;
    case 'S':
        return k30_glyph_S;
    case 'T':
        return k30_glyph_T;
    case 'U':
        return k30_glyph_U;
    default:
        return k30_glyph_unknown;
    }
}

static void k30_set_pixel(int32_t x, int32_t y, bool on)
{
    if (x < 0 || x >= K30_OLED_WIDTH || y < 0 || y >= K30_OLED_HEIGHT) {
        return;
    }

    uint8_t *byte = &k30_frame[x + (y >> 3) * K30_OLED_STRIDE];
    const uint8_t bit = (uint8_t)BIT(y & 7);

    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

static void k30_draw_char(char c, int32_t x, int32_t y)
{
    const uint8_t *glyph = k30_glyph(c);

    for (int32_t col = 0; col < K30_GLYPH_W; col++) {
        for (int32_t row = 0; row < K30_GLYPH_H; row++) {
            if ((glyph[col] & BIT(row)) != 0U) {
                k30_set_pixel(x + col, y + row, true);
            }
        }
    }
}

static int32_t k30_text_width(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    return (int32_t)strlen(text) * K30_CHAR_ADVANCE - 1;
}

static void k30_draw_centered(const char *text, int32_t y)
{
    const int32_t width = k30_text_width(text);
    int32_t x = (K30_OLED_WIDTH - width) / 2;

    if (x < 0) {
        x = 0;
    }

    for (int32_t i = 0; text[i] != '\0'; i++) {
        k30_draw_char(text[i], x + i * K30_CHAR_ADVANCE, y);
    }
}

static void k30_frame_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                               lv_color_t *color_p)
{
    (void)area;
    (void)color_p;

    if (k30_display_dev != NULL) {
        struct display_buffer_descriptor desc = {
            .buf_size = sizeof(k30_frame),
            .width = K30_OLED_WIDTH,
            .height = K30_OLED_HEIGHT,
            .pitch = K30_OLED_STRIDE,
        };

        k_mutex_lock(&k30_frame_mutex, K_FOREVER);
        (void)display_write(k30_display_dev, 0, 0, &desc, k30_frame);
        k_mutex_unlock(&k30_frame_mutex);
    }

    lv_disp_flush_ready(disp_drv);
}

static void k30_install_framebuffer(void)
{
    if (k30_flush_installed) {
        return;
    }

    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL || disp->driver == NULL) {
        return;
    }

    k30_display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(k30_display_dev)) {
        return;
    }

    disp->driver->flush_cb = k30_frame_flush_cb;
    k30_flush_installed = true;
}
#else
static void k30_install_framebuffer(void) {}
#endif /* LV_COLOR_DEPTH == 1 */

static uint8_t battery_level = 0;
static bool have_battery = false;
static bool battery_received_any = false;

/*
 * ZMK 在外设断开时会把 central 电量缓存清零，并注入一个假的 0% 事件。
 * 真实电量不会是 0%，因此忽略 0 值，继续显示最后一次有效电量。
 */
static void apply_battery_level(uint8_t level)
{
    battery_received_any = true;

    if (level == 0) {
        return;
    }

    battery_level = level;
    have_battery = true;
}

static void get_battery_text(char *buf, size_t len)
{
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &level) == 0) {
        apply_battery_level(level);
    }
#endif

    if (!battery_received_any) {
        snprintf(buf, len, "BAT:--");
    } else if (!have_battery) {
        snprintf(buf, len, "BAT:0?");
    } else {
        snprintf(buf, len, "BAT:%d%%", battery_level);
    }
}

static void get_battery_short(char *buf, size_t len)
{
    char full[12];

    get_battery_text(full, sizeof(full));

    const char *value = strstr(full, "BAT:");
    if (value != NULL) {
        value += 4;
    } else {
        value = full;
    }

    snprintf(buf, len, "%s", value);
}

static const char *get_output_text(void)
{
    struct zmk_endpoint_instance selected = zmk_endpoints_selected();

    switch (selected.transport) {
    case ZMK_TRANSPORT_USB:
        return "USB";
    case ZMK_TRANSPORT_BLE:
        return "BLE";
    default:
        return "---";
    }
}

#if LV_COLOR_DEPTH == 1
static void k30_render_status(void)
{
    char bat[8];
    char top[12];
    char layer[8];

    get_battery_short(bat, sizeof(bat));
    snprintf(top, sizeof(top), "%s %s", get_output_text(), bat);

    const char *name = zmk_keymap_layer_name(zmk_keymap_highest_layer_active());
    if (name == NULL || name[0] == '\0') {
        snprintf(layer, sizeof(layer), "L%d", zmk_keymap_highest_layer_active());
    } else {
        snprintf(layer, sizeof(layer), "%s", name);
    }

    k_mutex_lock(&k30_frame_mutex, K_FOREVER);
    memset(k30_frame, 0, sizeof(k30_frame));
    k30_draw_centered(top, 1);
    k30_draw_centered(layer, 9);
    k_mutex_unlock(&k30_frame_mutex);

    if (k30_status_screen != NULL) {
        lv_obj_invalidate(k30_status_screen);
    }
}
#else
static void k30_render_status(void) {}
#endif /* LV_COLOR_DEPTH == 1 */

static void refresh_work_handler(struct k_work *work)
{
    (void)work;
    k30_render_status();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);

/* 每 2 秒自愈刷新一次，避免事件和屏幕初始化竞争导致显示不同步。 */
static void refresh_tick_cb(struct k_timer *_timer)
{
    (void)_timer;
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
}

K_TIMER_DEFINE(refresh_timer, refresh_tick_cb, NULL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh)
{
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        apply_battery_level(ev->state_of_charge);
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }

    return 0;
}

ZMK_LISTENER(peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

static int layer_listener(const zmk_event_t *eh)
{
    (void)eh;
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh)
{
    (void)eh;
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

static int activity_listener(const zmk_event_t *eh)
{
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev != NULL && ev->state == ZMK_ACTIVITY_ACTIVE) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }

    return 0;
}

ZMK_LISTENER(dongle_display_activity, activity_listener);
ZMK_SUBSCRIPTION(dongle_display_activity, zmk_activity_state_changed);

lv_obj_t *zmk_display_status_screen(void)
{
    k30_install_framebuffer();

    k30_status_screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(k30_status_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(k30_status_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(k30_status_screen, 0, 0);
    lv_obj_set_style_pad_all(k30_status_screen, 0, 0);

    k30_render_status();

    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return k30_status_screen;
}
