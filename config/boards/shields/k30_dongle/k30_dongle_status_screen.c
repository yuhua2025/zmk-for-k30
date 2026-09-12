/*
 * Copyright (c) 2025 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/display.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#include <zmk/events/battery_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DISPLAY_BLANK_TIMEOUT_SECONDS 15

static lv_obj_t *battery_label;
static lv_obj_t *battery_label_b;  /* bold shadow */
static lv_obj_t *output_label;
static lv_obj_t *output_label_b;
static lv_obj_t *layer_label;
static lv_obj_t *layer_label_b;

static uint8_t battery_level = 0;
static volatile bool display_blanked = false;
static volatile bool pending_unblank = false;

/* Forward declarations for work handlers */
static void refresh_work_handler(struct k_work *work);
static void blank_display_work_handler(struct k_work *work);
static void unblank_display(void);

K_WORK_DEFINE(refresh_work, refresh_work_handler);
K_WORK_DELAYABLE_DEFINE(blank_work, blank_display_work_handler);

static void update_battery(void) {
    char text[16];

    snprintf(text, sizeof(text), "BAT:%d%%", battery_level);
    lv_label_set_text(battery_label, text);
    lv_label_set_text(battery_label_b, text);
}

static void update_output(void) {
    struct zmk_endpoint_instance selected = zmk_endpoints_selected();
    const char *text = "?";

    switch (selected.transport) {
    case ZMK_TRANSPORT_USB:
        text = "USB";
        break;
    case ZMK_TRANSPORT_BLE:
        text = "BLE";
        break;
    default:
        text = "---";
        break;
    }

    lv_label_set_text(output_label, text);
    lv_label_set_text(output_label_b, text);
}

static void update_layer(void) {
    zmk_keymap_layer_index_t layer = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(layer);

    if (name == NULL) {
        name = "???";
    }

    lv_label_set_text(layer_label, name);
    lv_label_set_text(layer_label_b, name);
}

static void set_labels_visible(bool visible) {
    if (visible) {
        lv_obj_clear_flag(output_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(output_label_b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(battery_label_b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(layer_label_b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(output_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(output_label_b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(layer_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(layer_label_b, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_work_handler(struct k_work *work) {
    /* 在 display workqueue 中处理可能的 unblank 请求，避免 LVGL 跨线程操作 */
    if (pending_unblank) {
        pending_unblank = false;
        set_labels_visible(true);
    }
    update_battery();
    update_output();
    update_layer();
}

static void blank_display_work_handler(struct k_work *work) {
    if (!display_blanked) {
        /* 隐藏所有 label，背景已是黑色，所以视觉效果就是黑屏 */
        set_labels_visible(false);
        display_blanked = true;
    }
}

static void unblank_display(void) {
    bool was_blanked = display_blanked;
    /* 标记需要在 display workqueue 中执行 unblank（set_labels_visible 是 LVGL 操作，
       必须在 display 线程中调用）。同时立即清空 display_blanked，让事件监听器知道
       屏幕已醒，可以在 display 线程上安全提交后续 refresh 工作。 */
    display_blanked = false;
    if (was_blanked) {
        pending_unblank = true;
    }
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    k_work_reschedule_for_queue(zmk_display_work_q(), &blank_work,
                                K_SECONDS(DISPLAY_BLANK_TIMEOUT_SECONDS));
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        /* 总是更新电量缓存，保证唤醒时显示最新值；仅在屏幕醒着时刷新 UI */
        battery_level = ev->state_of_charge;
        if (!display_blanked && zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
        }
    }

    return 0;
}

ZMK_LISTENER(peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

static int layer_listener(const zmk_event_t *eh) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

/* 键盘按键触发点亮屏幕 */
static int position_listener(const zmk_event_t *eh) {
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_position, position_listener);
ZMK_SUBSCRIPTION(dongle_display_position, zmk_position_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* 启动屏幕休眠定时器 */
    k_work_schedule_for_queue(zmk_display_work_q(), &blank_work,
                              K_SECONDS(DISPLAY_BLANK_TIMEOUT_SECONDS));

    /* Black background + white text = "黑底白字" on SSD1306 (white pixels lit) */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /* Output label - top left, 16px, bold via 1px offset shadow */
    output_label = lv_label_create(screen);
    lv_obj_align(output_label, LV_ALIGN_TOP_LEFT, 2, 7);
    lv_obj_set_style_text_font(output_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(output_label, lv_color_white(), 0);
    lv_label_set_text(output_label, "---");
    output_label_b = lv_label_create(screen);
    lv_obj_align(output_label_b, LV_ALIGN_TOP_LEFT, 3, 7);
    lv_obj_set_style_text_font(output_label_b, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(output_label_b, lv_color_white(), 0);
    lv_label_set_text(output_label_b, "---");

    /* Battery label - top right, 16px, bold via 1px offset shadow */
    battery_label = lv_label_create(screen);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -2, 7);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_label_set_text(battery_label, "BAT:?%");
    battery_label_b = lv_label_create(screen);
    lv_obj_align(battery_label_b, LV_ALIGN_TOP_RIGHT, -1, 7);
    lv_obj_set_style_text_font(battery_label_b, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(battery_label_b, lv_color_white(), 0);
    lv_label_set_text(battery_label_b, "BAT:?%");

    /* Layer label - bottom center, 16px, bold via 1px offset shadow */
    layer_label = lv_label_create(screen);
    lv_obj_align(layer_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(layer_label, lv_color_white(), 0);
    lv_label_set_text(layer_label, "---");
    layer_label_b = lv_label_create(screen);
    lv_obj_align(layer_label_b, LV_ALIGN_BOTTOM_MID, 1, -2);
    lv_obj_set_style_text_font(layer_label_b, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(layer_label_b, lv_color_white(), 0);
    lv_label_set_text(layer_label_b, "---");

    /* Initial update */
    update_output();
    update_layer();
    update_battery();

    return screen;
}
