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
#include <zmk/events/activity_state_changed.h>
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
static lv_obj_t *output_label;
static lv_obj_t *layer_label;

static uint8_t battery_level = 0;
static bool display_blanked = false;

static struct k_work_delayable blank_work;

static void update_battery(void) {
    char text[16];

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &level) == 0) {
        battery_level = level;
    }
#endif

    snprintf(text, sizeof(text), "BAT:%d%%", battery_level);
    lv_label_set_text(battery_label, text);
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
}

static void update_layer(void) {
    zmk_keymap_layer_index_t layer = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(layer);

    if (name == NULL) {
        name = "???";
    }

    lv_label_set_text(layer_label, name);
}

static void blank_display_work_handler(struct k_work *work) {
    const struct device *display = zmk_display_get_device();

    if (display != NULL && !display_blanked) {
        display_blanking_on(display);
        display_blanked = true;
    }
}

static void unblank_display(void) {
    const struct device *display = zmk_display_get_device();

    if (display != NULL && display_blanked) {
        display_blanking_off(display);
        display_blanked = false;
    }

    /* Reset the blank timer */
    k_work_reschedule(&blank_work, K_SECONDS(DISPLAY_BLANK_TIMEOUT_SECONDS));
}

static void refresh_work_handler(struct k_work *work) {
    update_battery();
    update_output();
    update_layer();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);
K_WORK_DELAYABLE_DEFINE(blank_work, blank_display_work_handler);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        battery_level = ev->state_of_charge;
        if (!display_blanked) {
            k_work_submit(&refresh_work);
        }
    }

    return 0;
}

ZMK_LISTENER(peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

static int layer_listener(const zmk_event_t *eh) {
    k_work_submit(&refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh) {
    k_work_submit(&refresh_work);
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

static int activity_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL) {
        return -ENOTSUP;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        unblank_display();
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        /* Do nothing, let the timer handle it */
        break;
    default:
        break;
    }

    return 0;
}

ZMK_LISTENER(dongle_display_activity, activity_listener);
ZMK_SUBSCRIPTION(dongle_display_activity, zmk_activity_state_changed);

static int key_position_listener(const zmk_event_t *eh) {
    /* Any key press wakes up the display */
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_key, key_position_listener);
ZMK_SUBSCRIPTION(dongle_display_key, zmk_key_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Set black background */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    /* Output label - top left, white text, 16px */
    output_label = lv_label_create(screen);
    lv_obj_align(output_label, LV_ALIGN_TOP_LEFT, 2, 4);
    lv_obj_set_style_text_color(output_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(output_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(output_label, "---");

    /* Battery label - top right, white text, 12px */
    battery_label = lv_label_create(screen);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(battery_label, "BAT:?%");

    /* Layer label - center, large white text */
    layer_label = lv_label_create(screen);
    lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_style_text_color(layer_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(layer_label, "---");

    /* Initial update */
    update_output();
    update_layer();
    update_battery();

    /* Start blank timer */
    k_work_schedule(&blank_work, K_SECONDS(DISPLAY_BLANK_TIMEOUT_SECONDS));

    return screen;
}
