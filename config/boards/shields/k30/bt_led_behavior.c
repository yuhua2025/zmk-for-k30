
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/ble_connected_state_changed.h>
#include <drivers/led.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>

static const struct device *bt_led_dev = DEVICE_DT_GET(DT_NODELABEL(bt_status_led));

static int on_ble_connected_state_changed(const zmk_event_t *eh) {
    bool connected = zmk_ble_active_profile_is_connected();
    
    if (!device_is_ready(bt_led_dev)) {
        return -ENODEV;
    }
    
    if (connected) {
        // 蓝牙已连接 - LED常亮
        led_on(bt_led_dev);
    } else {
        // 蓝牙未连接 - LED以500ms间隔闪烁
        led_blink(bt_led_dev, 500, 500);
    }
    return 0;
}

ZMK_LISTENER(bt_led_behavior, on_ble_connected_state_changed);
ZMK_SUBSCRIPTION(bt_led_behavior, zmk_ble_connected_state_changed);
ZMK_SUBSCRIPTION(bt_led_behavior, zmk_ble_active_profile_changed);
