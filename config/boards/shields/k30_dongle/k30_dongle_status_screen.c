/*
 * Copyright (c) 2025 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#include <zmk/events/battery_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 主行 "USB  BAT:85%"，副行图层名，均水平居中。
 * 1-bit 屏不能叠 1px 阴影做加粗（会糊成双影），只用单一 label。 */
static lv_obj_t *top_label;
static lv_obj_t *layer_label;

static uint8_t battery_level = 0;
static bool have_battery = false;
static bool battery_received_any = false;

/*
 * ZMK zeroes the central battery cache whenever the peripheral disconnects
 * (our keyboard sleeps after 10 min idle and drops BLE), injecting a fake
 * 0% event. A working battery never reports exactly 0%, so we ignore zero
 * values and keep showing the last known good level instead of a misleading
 * "BAT:0%". The value self-heals via the GATT BAS read on reconnect plus the
 * peripheral's periodic 60s battery report (see build.yml battery.c patch).
 *
 * Debug states:
 *   "BAT:--" never received ANY battery data (link/report path issue)
 *   "BAT:0?" received data but only ever 0 (keyboard battery sensor issue)
 */
static void apply_battery_level(uint8_t level) {
    battery_received_any = true;

    if (level == 0) {
        return;
    }

    battery_level = level;
    have_battery = true;
}

static void get_battery_text(char *buf, size_t len) {
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

static const char *get_output_text(void) {
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

static void update_top_line(void) {
    char bat[12];
    char text[24];

    get_battery_text(bat, sizeof(bat));
    snprintf(text, sizeof(text), "%s  %s", get_output_text(), bat);

    lv_label_set_text(top_label, text);
}

static void update_layer(void) {
    zmk_keymap_layer_index_t layer = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(layer);

    if (name == NULL) {
        name = "???";
    }

    lv_label_set_text(layer_label, name);
}

static void refresh_work_handler(struct k_work *work) {
    update_top_line();
    update_layer();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);

/*
 * Periodic self-healing refresh: every event-driven refresh can be missed
 * (e.g. right after the dongle reboots, or when a battery/layer event races
 * the display coming up). Ticking every 2s keeps the labels in sync with the
 * central's battery cache with negligible cost (one cache read + label text).
 */
static void refresh_tick_cb(struct k_timer *_timer) { k_work_submit_to_queue(zmk_display_work_q(), &refresh_work); }

K_TIMER_DEFINE(refresh_timer, refresh_tick_cb, NULL);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
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

static int layer_listener(const zmk_event_t *eh) {
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh) {
    k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

/* 屏幕唤醒时（activity 从 IDLE 变 ACTIVE）刷新电量 */
static int activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev != NULL && ev->state == ZMK_ACTIVITY_ACTIVE) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    return 0;
}

ZMK_LISTENER(dongle_display_activity, activity_listener);
ZMK_SUBSCRIPTION(dongle_display_activity, zmk_activity_state_changed);

/* 创建居中标签，黑底白字。128x32 屏放不下两行 16px（行高 18），
 * 主信息用 16px、图层名用 12px。 */
static lv_obj_t *create_centered_label(lv_obj_t *screen, lv_align_t align, int32_t x, int32_t y,
                                       const lv_font_t *font, const char *text) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, align, x, y);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Black background + white text = "黑底白字" on SSD1306 (white pixels lit) */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /* 主信息行（输出 + 电量）- 16px，水平居中，占屏幕上半部 */
    top_label = create_centered_label(screen, LV_ALIGN_TOP_MID, 0, -1, &lv_font_montserrat_16,
                                      "---  BAT:--");

    /* 图层名 - 12px，水平居中，贴底 */
    layer_label = create_centered_label(screen, LV_ALIGN_BOTTOM_MID, 0, 1, &lv_font_montserrat_12,
                                        "---");

    /* Initial update */
    update_top_line();
    update_layer();

    /* Start the periodic self-healing refresh */
    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
