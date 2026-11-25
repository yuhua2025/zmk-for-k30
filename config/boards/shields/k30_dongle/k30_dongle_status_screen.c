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
 * 实机照片测得的完整物理映射（r481/r482 两轮确认）：
 *   物理面板只有 64 列 x 16 行。SSD1306 兼容驱动仍按 128x32 GDRAM 写入：
 *     - 列方向：GDRAM 左半 (c=0..63) 与右半 (c=64..127) 都镜像折叠到同一组
 *       64 条物理列：p = 63 - (c mod 64)。
 *     - 行方向：GDRAM 32 行按 q' = r mod 16 折叠到 16 条物理行
 *       （r 与 r+16 落在同一行；r482 里两行 label 叠加且上下颠倒，
 *       标定图里横线在竖条带下方，都只有这个模型能同时解释）。
 *
 * 因此 overlay 必须保持已验证可点亮的 128x32/mux31 配置不动；
 * flush 回调做整帧补偿：
 *   1. 把 LVGL 每个 area 累积进持久 128x32 影子帧（不清理）；
 *   2. flush_is_last 时取中央 64 列 x 前 16 行窗口，按
 *      (p,q) -> GDRAM (63-p, q) 和 (127-p, q) 双写一致值；
 *      GDRAM 第 16..31 行保持全 0，避免折叠叠影。
 * UI 内容全部限制在 16 行内：单行 16px，电量和输出方式每 2 秒交替。
 */
#if LV_COLOR_DEPTH == 1
#define K30_FRAME_W 128
#define K30_FRAME_H 32
#define K30_PANEL_W 64
#define K30_PANEL_H 16
#define K30_WINDOW_X0 ((K30_FRAME_W - K30_PANEL_W) / 2)

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

static void k30_out_set(int32_t c, int32_t r, bool on)
{
    if (c < 0 || c >= K30_FRAME_W || r < 0 || r >= K30_FRAME_H) {
        return;
    }

    uint8_t *byte = &k30_out[c + (r >> 3) * K30_FRAME_W];
    const uint8_t bit = (uint8_t)BIT(r & 7);

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

    for (int32_t q = 0; q < K30_PANEL_H; q++) {
        for (int32_t p = 0; p < K30_PANEL_W; p++) {
            const bool on = k30_shadow_get(K30_WINDOW_X0 + p, q);

            k30_out_set(K30_PANEL_W - 1 - p, q, on);
            k30_out_set(K30_FRAME_W - 1 - p, q, on);
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

static lv_obj_t *status_label;
static bool show_output;

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

static void update_label(void)
{
    char bat[8];

    get_battery_text(bat, sizeof(bat));
    lv_label_set_text(status_label, show_output ? get_output_text() : bat);
}

static void refresh_work_handler(struct k_work *work)
{
    (void)work;
    show_output = !show_output;
    update_label();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);

/* 每 2 秒交替显示电量 / 输出方式，同时兼作自愈刷新。 */
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
        show_output = false;
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }

    return 0;
}

ZMK_LISTENER(peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

static int endpoint_listener(const zmk_event_t *eh)
{
    (void)eh;
    show_output = true;
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
    k30_install_frame_flush();

    lv_obj_t *screen = lv_obj_create(NULL);

    /* 黑底白字：MONO10 下 white 像素点亮，black 像素熄灭。 */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /*
     * 物理面板只有 16 行：单行 montserrat_16，电量 / 输出方式每 2 秒交替，
     * 内容水平居中于中央 64 列窗口。
     */
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_TRANSP, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(status_label, "--");

    update_label();
    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
