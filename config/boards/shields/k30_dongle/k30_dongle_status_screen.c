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
#define BLANK_TIMER_PERIOD_MS 1000

static lv_obj_t *battery_label;
static lv_obj_t *battery_label_b;  /* bold shadow */
static lv_obj_t *output_label;
static lv_obj_t *output_label_b;
static lv_obj_t *layer_label;
static lv_obj_t *layer_label_b;

static uint8_t battery_level = 0;
/* 单调时间戳，记录最近一次"活动"时间；监听器在 system workqueue 上下文中只更新它，
   真正的 LVGL 操作全部交给 display workqueue / lv_timer 处理。 */
static atomic_t last_activity_ms = ATOMIC_INIT(0);
static volatile bool display_blanked = false;
static lv_timer_t *blank_timer = NULL;

/* Forward declarations */
static void refresh_work_handler(struct k_work *work);
K_WORK_DEFINE(refresh_work, refresh_work_handler);

static void update_battery(void) {
    char text[16];

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    /* 轮询外设电量缓存。central.c 在 raise zmk_peripheral_battery_state_changed 之前
       就把电量写入缓存数组，所以这里读到的就是最新值。事件监听器只是个冗余的
       "wake up" 触发器，电量真正的来源是这个 API。 */
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

/* 把所有 label 文本清空 —— 这是已知的可行方法（lv_label_set_text 在 update_battery 里
   一直工作正常），用 set_text("") 实现"黑屏"而不是 lv_obj_add_flag(HIDDEN)，因为后者
   在某些 LVGL 配置下不会触发重绘。 */
static void clear_all_labels(void) {
    lv_label_set_text(output_label, "");
    lv_label_set_text(output_label_b, "");
    lv_label_set_text(battery_label, "");
    lv_label_set_text(battery_label_b, "");
    lv_label_set_text(layer_label, "");
    lv_label_set_text(layer_label_b, "");
}

/* lv_timer 回调：在 LVGL 线程里运行，所有 LVGL 调用都安全。
   每 1 秒检查一次"距离上次活动的时间"，>= 15 秒就清空 label。 */
static void blank_timer_cb(lv_timer_t *timer) {
    if (display_blanked) {
        return;
    }
    int32_t now_ms = (int32_t)k_uptime_get_32();
    int32_t last_ms = atomic_get(&last_activity_ms);
    int32_t elapsed_ms = now_ms - last_ms;
    if (elapsed_ms < 0) {
        elapsed_ms = INT32_MAX;  /* 时间戳回绕，强制进入 blank */
    }
    if (elapsed_ms >= DISPLAY_BLANK_TIMEOUT_SECONDS * 1000) {
        LOG_INF("Blanking display after %d ms idle", elapsed_ms);
        clear_all_labels();
        display_blanked = true;
    }
}

/* 监听器调用：只更新时间戳 + 触发 refresh；不做任何 LVGL 操作 */
static void mark_activity(void) {
    atomic_set(&last_activity_ms, (int32_t)k_uptime_get_32());
}

static void unblank_display(void) {
    mark_activity();
    if (display_blanked) {
        display_blanked = false;
        if (zmk_display_is_initialized()) {
            k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
        }
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
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
    unblank_display();
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    return 0;
}

ZMK_LISTENER(dongle_display_layer, layer_listener);
ZMK_SUBSCRIPTION(dongle_display_layer, zmk_layer_state_changed);

static int endpoint_listener(const zmk_event_t *eh) {
    unblank_display();
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
    return 0;
}

ZMK_LISTENER(dongle_display_endpoint, endpoint_listener);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(dongle_display_endpoint, zmk_usb_conn_state_changed);

/* 键盘按键触发点亮屏幕（外设按键通过 split BLE 上报到 central） */
static int position_listener(const zmk_event_t *eh) {
    unblank_display();
    return 0;
}

ZMK_LISTENER(dongle_display_position, position_listener);
ZMK_SUBSCRIPTION(dongle_display_position, zmk_position_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* 初始化活动时间 —— 屏幕将在 15 秒后休眠（如果没有按键） */
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

    /* 启动 LVGL 定时器，每秒检查是否进入休眠。
       lv_timer_create 在 init 完成后才被 lv_task_handler 驱动，正好。 */
    blank_timer = lv_timer_create(blank_timer_cb, BLANK_TIMER_PERIOD_MS, NULL);
    if (blank_timer != NULL) {
        lv_timer_set_repeat_count(blank_timer, -1);  /* 无限循环 */
    }

    /* Initial update */
    update_output();
    update_layer();
    update_battery();

    return screen;
}
