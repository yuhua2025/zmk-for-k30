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
#define TICK_PERIOD_MS 500
#define TICK_THREAD_STACK_SIZE 2048
#define TICK_THREAD_PRIORITY 0   /* 高优先级，避免被饿死 */

static lv_obj_t *battery_label;
static lv_obj_t *battery_label_b;
static lv_obj_t *output_label;
static lv_obj_t *output_label_b;
static lv_obj_t *layer_label;
static lv_obj_t *layer_label_b;

static uint8_t battery_level = 0;
static atomic_t last_activity_ms = ATOMIC_INIT(0);
static volatile bool display_blanked = false;
static volatile int tick_count = 0;

/* 独立线程，完全不依赖 ZMK 的 display workqueue。
   如果 display workqueue 卡死了（T:0 不变证明），这个线程仍然能运行。 */
K_THREAD_STACK_DEFINE(tick_thread_stack, TICK_THREAD_STACK_SIZE);
static struct k_thread tick_thread_data;
static bool tick_thread_started = false;

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
    /* 调试：显示 tick_count 判断线程是否在运行 */
    char tick_text[16];
    snprintf(tick_text, sizeof(tick_text), "T:%d", tick_count);
    lv_label_set_text(layer_label, tick_text);
    lv_label_set_text(layer_label_b, tick_text);
}

static void clear_all_labels(void) {
    lv_label_set_text(output_label, "");
    lv_label_set_text(output_label_b, "");
    lv_label_set_text(battery_label, "");
    lv_label_set_text(battery_label_b, "");
    lv_label_set_text(layer_label, "");
    lv_label_set_text(layer_label_b, "");
}

/* 独立线程主函数：
   - k_sleep 不依赖 k_timer 回调
   - 直接调用 lv_label_set_text + lv_task_handler，不依赖 workqueue
   - 如果 ZMK 的 display workqueue 卡死了，这个线程仍然能驱动屏幕更新 */
static void tick_thread_main(void *a, void *b, void *c) {
    while (1) {
        k_sleep(K_MSEC(TICK_PERIOD_MS));

        tick_count++;

        /* 检查休眠 */
        if (!display_blanked) {
            int32_t now_ms = (int32_t)k_uptime_get_32();
            int32_t last_ms = (int32_t)atomic_get(&last_activity_ms);
            int32_t elapsed_ms = now_ms - last_ms;
            if (elapsed_ms < 0) {
                elapsed_ms = INT32_MAX;
            }
            if (elapsed_ms >= DISPLAY_BLANK_TIMEOUT_SECONDS * 1000) {
                LOG_INF("Blanking display after %d ms idle", elapsed_ms);
                clear_all_labels();
                display_blanked = true;
            }
        }

        /* 每 tick 都刷新显示 */
        if (!display_blanked) {
            update_battery();
            update_output();
            update_layer();
        }

        /* 强制驱动 LVGL 渲染管线 —— flush 像素到 SSD1306 */
        lv_task_handler();
    }
}

static void mark_activity(void) {
    atomic_set(&last_activity_ms, (int32_t)k_uptime_get_32());
}

static void unblank_display(void) {
    mark_activity();
    if (display_blanked) {
        display_blanked = false;
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        battery_level = ev->state_of_charge;
        LOG_INF("Peripheral battery event: %d%%", ev->state_of_charge);
    }

    return 0;
}

ZMK_LISTENER(peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

static int layer_listener(const zmk_event_t *eh) {
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh) {
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

static int position_listener(const zmk_event_t *eh) {
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_position, position_listener);
ZMK_SUBSCRIPTION(dongle_display_position, zmk_position_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    atomic_set(&last_activity_ms, (int32_t)k_uptime_get_32());

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

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

    /* 启动独立线程：不依赖 ZMK display workqueue、不依赖 k_timer 回调。
       k_sleep 由系统时钟驱动，只要内核在运行线程就会运行。 */
    if (!tick_thread_started) {
        k_thread_create(&tick_thread_data, tick_thread_stack,
                        K_THREAD_STACK_SIZEOF(tick_thread_stack),
                        tick_thread_main, NULL, NULL, NULL,
                        TICK_THREAD_PRIORITY, 0, K_NO_WAIT);
        tick_thread_started = true;
    }

    update_output();
    update_layer();
    update_battery();

    return screen;
}
