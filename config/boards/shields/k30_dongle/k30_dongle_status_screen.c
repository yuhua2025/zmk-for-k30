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
 * 这块 SSD1312 面板只有 segment-remap + com-invdir 能点亮，该映射会把显存
 * 内容旋转 180 度。实机验证：只做上下翻转时文字仍是左右镜像，因此这里包
 * 一层 LVGL flush 回调，在写入 SSD1306 驱动之前做完整 180 度旋转。
 *
 * 1bpp + VTILED 布局：每字节是同一列上的 8 个像素，180 度旋转既要倒序
 * 8 行块并左右换列，也要按 ry 重新计算字节内的 bit 号。
 */
#if LV_COLOR_DEPTH == 1
#define K30_OLED_WIDTH  128
#define K30_OLED_HEIGHT 32

static void (*k30_flush_orig)(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                              lv_color_t *color_p);
static uint8_t k30_rotate_buf[K30_OLED_WIDTH * K30_OLED_HEIGHT / 8];
static bool k30_msb_first;

static void k30_rotate_180_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                                    lv_color_t *color_p)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    lv_disp_t *disp = lv_disp_get_default();
    const int32_t xres = (disp != NULL) ? lv_disp_get_hor_res(disp) : K30_OLED_WIDTH;
    const int32_t yres = (disp != NULL) ? lv_disp_get_ver_res(disp) : K30_OLED_HEIGHT;

    if (k30_flush_orig == NULL || w <= 0 || h <= 0 ||
        ((uint32_t)w * (uint32_t)h) > (sizeof(k30_rotate_buf) * 8U)) {
        if (k30_flush_orig != NULL) {
            k30_flush_orig(disp_drv, area, color_p);
        }
        return;
    }

    const uint8_t *src = (const uint8_t *)color_p;
    memset(k30_rotate_buf, 0, sizeof(k30_rotate_buf));

    for (int32_t ry = 0; ry < h; ry++) {
        for (int32_t rx = 0; rx < w; rx++) {
            const int32_t sx = w - 1 - rx;
            const int32_t sy = h - 1 - ry;
            const uint8_t src_bit = k30_msb_first ? (uint8_t)(7 - (sy & 7)) : (uint8_t)(sy & 7);
            const uint8_t dst_bit = k30_msb_first ? (uint8_t)(7 - (ry & 7)) : (uint8_t)(ry & 7);

            if ((src[sx + ((sy >> 3) * w)] & (1U << src_bit)) != 0U) {
                k30_rotate_buf[rx + ((ry >> 3) * w)] |= (uint8_t)(1U << dst_bit);
            }
        }
    }

    lv_area_t rotated_area;
    rotated_area.x1 = xres - 1 - area->x2;
    rotated_area.x2 = xres - 1 - area->x1;
    rotated_area.y1 = yres - 1 - area->y2;
    rotated_area.y2 = yres - 1 - area->y1;

    k30_flush_orig(disp_drv, &rotated_area, (lv_color_t *)k30_rotate_buf);
}

static void k30_install_rotation(void)
{
    lv_disp_t *disp = lv_disp_get_default();

    if (disp == NULL || disp->driver == NULL || disp->driver->flush_cb == NULL ||
        k30_flush_orig != NULL) {
        return;
    }

    const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (device_is_ready(display_dev)) {
        struct display_capabilities caps;
        display_get_capabilities(display_dev, &caps);
        k30_msb_first = (caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST) != 0;
    }

    k30_flush_orig = disp->driver->flush_cb;
    disp->driver->flush_cb = k30_rotate_180_flush_cb;
}
#else
static void k30_install_rotation(void) {}
#endif /* LV_COLOR_DEPTH == 1 */

static lv_obj_t *top_label;
static lv_obj_t *layer_label;

static uint8_t battery_level = 0;
static bool have_battery = false;
static bool battery_received_any = false;

/*
 * ZMK 在外设断开时会把 central 电量缓存清零，并注入一个假的 0% 事件。
 * 真实电量不会是 0%，因此忽略 0 值，继续显示最后一次有效电量。
 *   "BAT:--" 从未收到任何电量数据
 *   "BAT:0?" 收到过数据但只有 0
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

/* 128x32 单行放 16px：最长的 "USB BAT:100%" 约 117px，可以居中放下。 */
static void update_top_line(void)
{
    char bat[12];
    char text[24];

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
    update_top_line();
    update_layer();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);

/* 每 2 秒自愈刷新一次，避免事件和屏幕初始化竞争导致显示不同步。 */
static void refresh_tick_cb(struct k_timer *_timer)
{
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
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh)
{
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

static lv_obj_t *create_centered_label(lv_obj_t *screen, const lv_font_t *font, lv_align_t align,
                                       int32_t y, const char *text)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_align(label, align, 0, y);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *zmk_display_status_screen(void)
{
    k30_install_rotation();

    lv_obj_t *screen = lv_obj_create(NULL);

    /* 黑底白字：MONO10 下 white 像素点亮，black 像素熄灭。 */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /* 主行 16px 居中；图层名 12px 居中，两行刚好放进 32px。 */
    top_label = create_centered_label(screen, &lv_font_montserrat_16, LV_ALIGN_TOP_MID, -1,
                                      "--- BAT:--");
    layer_label = create_centered_label(screen, &lv_font_montserrat_12, LV_ALIGN_BOTTOM_MID, 1,
                                        "---");

    update_top_line();
    update_layer();

    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
