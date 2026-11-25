/*
 * K30 dongle status screen - clean rewrite (2026-11-25)
 *
 * Shows exactly two things, centered on one 16px line:
 *   - BLE connection state to the keyboard (peripheral)
 *   - keyboard battery level
 *
 *   connected, battery known : "BLE 85%"
 *   connected, no data yet   : "BLE --"
 *   connected, always-zero   : "BLE 0?"   (sensor/report issue)
 *   keyboard disconnected    : "BLE OFF"
 *
 * Zero-percent readings are ignored: ZMK clears the central's battery
 * cache when the peripheral disconnects, which surfaces as a fake 0%
 * event. Battery data self-heals via the GATT BAS read on reconnect,
 * the peripheral's periodic report (battery.c patch in build.yml) and
 * the 2s refresh timer below.
 *
 * Orientation: the panel is drawn upright by the ssd1306 driver itself
 * (overlay has NO segment-remap / com-invdir). No flush hacks here.
 */

#include <lvgl.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static lv_obj_t *status_label;

static bool peripheral_connected;
static bool battery_received_any;
static bool have_battery;
static uint8_t battery_level;

static void apply_battery_level(uint8_t level) {
    battery_received_any = true;

    if (level == 0) {
        return; /* fake 0% on peripheral disconnect - ignore */
    }

    battery_level = level;
    have_battery = true;
}

static void refresh_status(void) {
    char text[20];

    if (!peripheral_connected) {
        snprintf(text, sizeof(text), "BLE OFF");
    } else if (!battery_received_any) {
        snprintf(text, sizeof(text), "BLE --");
    } else if (!have_battery) {
        snprintf(text, sizeof(text), "BLE 0?");
    } else {
        snprintf(text, sizeof(text), "BLE %d%%", battery_level);
    }

    lv_label_set_text(status_label, text);
}

static void refresh_work_handler(struct k_work *_work) { refresh_status(); }

K_WORK_DEFINE(refresh_work, refresh_work_handler);

/*
 * Self-healing: event-driven refreshes can be missed right after the
 * dongle reboots or when an event races the display coming up. Ticking
 * every 2s re-reads the central's caches at negligible cost.
 */
static void refresh_tick_cb(struct k_timer *_timer) { k_work_submit(&refresh_work); }

K_TIMER_DEFINE(refresh_timer, refresh_tick_cb, NULL);

static int battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL) {
        apply_battery_level(ev->state_of_charge);
        k_work_submit(&refresh_work);
    }

    return 0;
}

ZMK_LISTENER(dongle_battery, battery_listener);
ZMK_SUBSCRIPTION(dongle_battery, zmk_peripheral_battery_state_changed);

static int peripheral_status_listener(const zmk_event_t *eh) {
    const struct zmk_split_peripheral_status_changed *ev =
        as_zmk_split_peripheral_status_changed(eh);

    if (ev != NULL) {
        peripheral_connected = ev->connected;
        k_work_submit(&refresh_work);
    }

    return 0;
}

ZMK_LISTENER(dongle_peripheral_status, peripheral_status_listener);
ZMK_SUBSCRIPTION(dongle_peripheral_status, zmk_split_peripheral_status_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    /* Black background + white text ("黑底白字") */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    status_label = lv_label_create(screen);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);

    /* Initial state from current caches, then keep ticking */
    peripheral_connected = zmk_split_bt_peripheral_is_connected();

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t level = 0;
    if (zmk_split_central_get_peripheral_battery_level(0, &level) == 0) {
        apply_battery_level(level);
    }
#endif

    refresh_status();

    k_timer_start(&refresh_timer, K_SECONDS(2), K_SECONDS(2));

    return screen;
}
