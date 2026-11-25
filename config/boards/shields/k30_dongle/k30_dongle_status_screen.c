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
 * 面板是标准 0.87 寸 128x32 SSD1312 模块。
 * overlay 里 segment-remap（列镜像）+ com-invdir（行镜像）同时开启，
 * 合成效果就是整屏旋转 180 度；早期把实机照片误读成“GDRAM 折叠”是错的。
 *
 * 处理方式：overlay 保持这组已验证可点亮的参数不动，在 flush 回调里
 * 做一次整帧 180 度旋转补偿：
 *   1. 把 LVGL 的每个局部 area 累积进持久 128x32 影子帧（不清理，
 *      局部刷新不会抹掉其余内容）；
 *   2. flush_is_last 时按 out(127-x, 31-y) = shadow(x, y) 重排，
 *      整帧一次 display_write。
 */
#if LV_COLOR_DEPTH == 1
#define K30_FRAME_W 128
#define K30_FRAME_H 32

static const struct device *k30_display_dev;
static uint8_t k30_shadow[K30_FRAME_W * K30_FRAME_H / 8];
static uint8_t k30_out[K30_FRAME_W * K30_FRAME_H / 8];
static bool k30_flush_installed;

static void k30_shadow_set(int32_t x, int32_t y, bool on)
{
    if (x < 0 || x >= K30_FRAME_W || y < 0 || y >= K30_FRAME_H) {
        return;
    }

    uint8_t *byte = &k30_shadow[x + (y >> 3) * K30_FRAME_W];
    const uint8_t bit = (uint8_t)BIT(y & 7);

    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

static bool k30_shadow_get(int32_t x, int32_t y)
{
    if (x < 0 || x >= K30_FRAME_W || y < 0 || y >= K30_FRAME_H) {
        return false;
    }

    return (k30_shadow[x + (y >> 3) * K30_FRAME_W] & BIT(y & 7)) != 0U;
}

static void k30_out_set(int32_t x, int32_t y, bool on)
{
    if (x < 0 || x >= K30_FRAME_W || y < 0 || y >= K30_FRAME_H) {
        return;
    }

    uint8_t *byte = &k30_out[x + (y >> 3) * K30_FRAME_W];
    const uint8_t bit = (uint8_t)BIT(y & 7);

    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

static void k30_frame_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                               lv_color_t *color_p)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    const uint8_t *src = (const uint8_t *)color_p;

    if (w > 0 && h > 0) {
        for (int32_t dy = 0; dy < h; dy++) {
            const int32_t sy = area->y1 + dy;

            for (int32_t dx = 0; dx < w; dx++) {
                const bool on = ((src[dx + (dy >> 3) * w] & BIT(dy & 7)) != 0U);
                k30_shadow_set(area->x1 + dx, sy, on);
            }
        }
    }

    if (!lv_disp_flush_is_last(disp_drv)) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    memset(k30_out, 0, sizeof(k30_out));

    for (int32_t y = 0; y < K30_FRAME_H; y++) {
        for (int32_t x = 0; x < K30_FRAME_W; x++) {
            k30_out_set(K30_FRAME_W - 1 - x, K30_FRAME_H - 1 - y,
                        k30_shadow_get(x, y));
        }
    }

    if (k30_display_dev != NULL) {
        struct display_buffer_descriptor desc = {
            .buf_size = sizeof(k30_out),
            .width = K30_FRAME_W,
            .height = K30_FRAME_H,
            .pitch = K30_FRAME_W,
        };

        (void)display_write(k30_display_dev, 0, 0, &desc, k30_out);
    }

    lv_disp_flush_ready(disp_drv);
}

static void k30_install_frame_flush(void)
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
static void k30_install_frame_flush(void) {}
#endif /* LV_COLOR_DEPTH == 1 */

static lv_obj_t *top_label;
static lv_obj_t *layer_label;

static uint8_t battery_level = 0;
static bool have_battery = false;
static bool battery_received_any = false;

/*
 * ZMK 在外设断开时会把 central 电量缓存清零，并注入一个假的 0% 事件。
 * 真实电量不会是 0%，因此忽略 0 值，继续显示最后一次有效电量。
 *   "--" 从未收到任何电量数据
 *   "0?" 收到过数据但只有 0
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
        snprintf(buf, len, "--");
    } else if (!have_battery) {
        snprintf(buf, len, "0?");
    } else {
        snprintf(buf, len, "%d%%", battery_level);
    }
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

static void update_top_line(void)
{
    char bat[8];
    char text[16];

    get_battery_text(bat, sizeof(bat));
    snprintf(text, sizeof(text), "%s %s", get_output_text(), bat);
    lv_label_set_text(top_label, text);
}

static void update_layer(void)
{
    char text[16];
    const char *name = zmk_keymap_layer_name(zmk_keymap_highest_layer_active());

    if (name == NULL || name[0] == '\0') {
        snprintf(text, sizeof(text), "L%d", zmk_keymap_highest_layer_active());
        name = text;
    }

    lv_label_set_text(layer_label, name);
}

static void refresh_work_handler(struct k_work *work)
{
    (void)work;
    update_top_line();
    update_layer();
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

static lv_obj_t *create_centered_label(lv_obj_t *screen, const lv_font_t *font, int32_t y,
                                       const char *text)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *zmk_display_status_screen(void)
{
    k30_install_frame_flush();

    lv_obj_t *screen = lv_obj_create(NULL);

    /* 黑底白字：MONO10 下 white 像素点亮，black 像素熄灭。 */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /* 128x32：主行 16px 输出+电量，副行 12px 层名，均居中。 */
    top_label = create_centered_label(screen, &lv_font_montserrat_16, 0, "--- --");
    layer_label = create_centered_label(screen, &lv_font_montserrat_12, 19, "---");

    update_top_line();
    update_layer();
    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
