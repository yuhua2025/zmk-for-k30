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
#define TICK_PERIOD_MS 1000   /* lv_timer 每 1 秒触发一次 */
#define REFRESH_EVERY_N_TICKS 2  /* 每 2 个 tick (=2 秒) 强制刷新一次显示 */

static lv_obj_t *battery_label;
static lv_obj_t *battery_label_b;  /* bold shadow */
static lv_obj_t *output_label;
static lv_obj_t *output_label_b;
static lv_obj_t *layer_label;
static lv_obj_t *layer_label_b;

static uint8_t battery_level = 0;
static atomic_t last_activity_ms = ATOMIC_INIT(0);
static volatile bool display_blanked = false;
static lv_timer_t *tick_timer = NULL;
static volatile int tick_count = 0;

/* Forward declarations */
static void refresh_work_handler(struct k_work *work);
K_WORK_DEFINE(refresh_work, refresh_work_handler);

static void update_battery(void) {
    char text[16];

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    /* 每次刷新都重新读 peripheral 电量缓存。
       central.c 在收到 peripheral 的 BAS notification 后会把电量写入缓存数组。 */
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
    /* 调试：显示 tick_count 而不是层名，用来判断 lv_timer 是否在运行。
       如果数字在递增 -> lv_timer 在跑（屏幕刷新机制正常）
       如果数字不变 -> lv_timer 没跑（lv_task_handler 从未被调用） */
    char tick_text[16];
    snprintf(tick_text, sizeof(tick_text), "T:%d", tick_count);
    lv_label_set_text(layer_label, tick_text);
    lv_label_set_text(layer_label_b, tick_text);
}

static void refresh_work_handler(struct k_work *work) {
    update_battery();
    update_output();
    update_layer();
}

static void clear_all_labels(void) {
    lv_label_set_text(output_label, "");
    lv_label_set_text(output_label_b, "");
    lv_label_set_text(battery_label, "");
    lv_label_set_text(battery_label_b, "");
    lv_label_set_text(layer_label, "");
    lv_label_set_text(layer_label_b, "");
}

/* LVGL 定时器回调：在 LVGL 线程里运行，所有 LVGL 调用都安全。
   - 每 1 秒检查是否该进入休眠（15 秒无活动 -> 清空 label）
   - 每 2 秒强制刷新一次显示（从 API 读电量），不依赖事件 listener */
static void tick_timer_cb(lv_timer_t *timer) {
    /* 1. 检查休眠 */
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

    /* 2. 每 N 个 tick 强制刷新一次显示（只有未休眠时） */
    tick_count++;
    if (!display_blanked && (tick_count % REFRESH_EVERY_N_TICKS) == 0) {
        update_battery();
        update_output();
        update_layer();
    }
}

static void mark_activity(void) {
    atomic_set(&last_activity_ms, (int32_t)k_uptime_get_32());
}

static void unblank_display(void) {
    mark_activity();
    if (display_blanked) {
        display_blanked = false;
        /* 唤醒时立即刷新一次，把最新数据填回 label */
        update_battery();
        update_output();
        update_layer();
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

    /* 启动 LVGL 定时器 —— 这是核心修复：
       - 不依赖 k_work_delayable（之前不生效）
       - 不依赖事件 listener 触发 refresh（之前电量不更新）
       - lv_timer 在 LVGL 线程里运行，直接调 update_* 函数 */
    tick_timer = lv_timer_create(tick_timer_cb, TICK_PERIOD_MS, NULL);
    if (tick_timer != NULL) {
        lv_timer_set_repeat_count(tick_timer, -1);  /* 无限循环 */
    }

    /* Initial update */
    update_output();
    update_layer();
    update_battery();

    return screen;
}
