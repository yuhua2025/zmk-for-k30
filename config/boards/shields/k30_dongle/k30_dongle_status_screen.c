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
 * 实机照片测得的物理映射（r481 无变换固件）：
 *   物理面板只有 64 列 x 32 行；GDRAM 左半 (c=0..63) 和右半 (c=64..127)
 *   都镜像折叠到这 64 列上：p = 63 - (c mod 64)；同时整屏上下翻转：q = 31 - r。
 *   证据：居中的 "BLE 0?" 显示成左右两个各自旋转 180 度的半段，
 *   中间的空格正好跨在折叠边界 c=64 上。
 *
 * 因此 overlay 必须保持已验证可点亮的 128x32/mux31 配置不动；
 * 这里在 flush 回调里做整帧补偿：
 *   1. 把 LVGL 的每个局部 area 累积进持久 128x32 影子帧（不清理，
 *      这样局部刷新不会把其余内容抹掉）；
 *   2. 最后一个 area 到齐后，取画面中央 64 列窗口，按
 *      (p,q) -> GDRAM (63-p, 31-q) 和 (127-p, 31-q) 双写（两半物理重叠，
 *      必须写一致，否则残留半段会叠加鬼影），整帧一次 display_write。
 */
#if LV_COLOR_DEPTH == 1
#define K30_FRAME_W 128
#define K30_FRAME_H 32
#define K30_PANEL_W 64
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

    for (int32_t q = 0; q < K30_FRAME_H; q++) {
        for (int32_t p = 0; p < K30_PANEL_W; p++) {
            const bool on = k30_shadow_get(K30_WINDOW_X0 + p, q);

            k30_out_set(K30_PANEL_W - 1 - p, K30_FRAME_H - 1 - q, on);
            k30_out_set(K30_FRAME_W - 1 - p, K30_FRAME_H - 1 - q, on);
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

static lv_obj_t *battery_label;
static lv_obj_t *output_label;

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

static void update_labels(void)
{
    char bat[8];

    get_battery_text(bat, sizeof(bat));
    lv_label_set_text(battery_label, bat);
    lv_label_set_text(output_label, get_output_text());
}

static void refresh_work_handler(struct k_work *work)
{
    (void)work;
    update_labels();
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

    /*
     * 物理面板 64 列宽：主行用 16px 只放电量，副行用 12px 放输出方式，
     * 保证最长内容都落在中央 64 列窗口内。
     */
    battery_label = create_centered_label(screen, &lv_font_montserrat_16, 0, "--");
    output_label = create_centered_label(screen, &lv_font_montserrat_12, 19, "---");

    update_labels();
    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
