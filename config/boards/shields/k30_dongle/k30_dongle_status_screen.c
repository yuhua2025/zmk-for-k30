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
#include <zmk/event_manager.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#include <zmk/events/battery_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static lv_obj_t *battery_label;
static lv_obj_t *battery_label_b;  /* bold shadow */
static lv_obj_t *output_label;
static lv_obj_t *output_label_b;
static lv_obj_t *layer_label;
static lv_obj_t *layer_label_b;

static uint8_t battery_level = 0;

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

static void refresh_work_handler(struct k_work *work) {
    update_battery();
    update_output();
    update_layer();
}

K_WORK_DEFINE(refresh_work, refresh_work_handler);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        battery_level = ev->state_of_charge;
        k_work_submit(&refresh_work);
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

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Output label - top left, 16px, bold via 1px offset shadow */
    output_label = lv_label_create(screen);
    lv_obj_align(output_label, LV_ALIGN_TOP_LEFT, 2, 7);
    lv_obj_set_style_text_font(output_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(output_label, "---");
    output_label_b = lv_label_create(screen);
    lv_obj_align(output_label_b, LV_ALIGN_TOP_LEFT, 3, 7);
    lv_obj_set_style_text_font(output_label_b, &lv_font_montserrat_16, 0);
    lv_label_set_text(output_label_b, "---");

    /* Battery label - top right, 16px, bold via 1px offset shadow */
    battery_label = lv_label_create(screen);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, -2, 7);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(battery_label, "BAT:?%");
    battery_label_b = lv_label_create(screen);
    lv_obj_align(battery_label_b, LV_ALIGN_TOP_RIGHT, -1, 7);
    lv_obj_set_style_text_font(battery_label_b, &lv_font_montserrat_16, 0);
    lv_label_set_text(battery_label_b, "BAT:?%");

    /* Layer label - bottom center, 16px, bold via 1px offset shadow */
    layer_label = lv_label_create(screen);
    lv_obj_align(layer_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(layer_label, "---");
    layer_label_b = lv_label_create(screen);
    lv_obj_align(layer_label_b, LV_ALIGN_BOTTOM_MID, 1, -2);
    lv_obj_set_style_text_font(layer_label_b, &lv_font_montserrat_16, 0);
    lv_label_set_text(layer_label_b, "---");

    /* Initial update */
    update_output();
    update_layer();
    update_battery();

    return screen;
}
