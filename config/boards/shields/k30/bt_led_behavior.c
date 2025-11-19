
/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/ble.h>
#include <zmk/event_manager.h>

// 定义日志模块
LOG_MODULE_REGISTER(bt_led_behavior, CONFIG_ZMK_LOG_LEVEL);

// 从设备树获取LED引脚信息
#define BT_LED_NODE DT_NODELABEL(bt_status_led)

#if DT_NODE_HAS_STATUS(BT_LED_NODE, okay)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(BT_LED_NODE, gpios);
static struct k_timer blink_timer;
static bool led_state = false;
static bool is_blinking = false;
static uint32_t on_time = 500;  // 默认亮时间
static uint32_t off_time = 500; // 默认灭时间

// 闪烁定时器回调
static void blink_timer_handler(struct k_timer *timer_id) {
    if (!is_blinking) {
        return;
    }
    
    led_state = !led_state;
    gpio_pin_set_dt(&led, led_state);
    
    // 设置下一次定时器的时间
    uint32_t next_time = led_state ? on_time : off_time;
    k_timer_start(&blink_timer, K_MSEC(next_time), K_NO_WAIT);
}

// 打开LED
static void led_on(void) {
    is_blinking = false;
    k_timer_stop(&blink_timer);
    gpio_pin_set_dt(&led, true);
    led_state = true;
    LOG_INF("LED ON");
}

// 关闭LED
static void led_off(void) {
    is_blinking = false;
    k_timer_stop(&blink_timer);
    gpio_pin_set_dt(&led, false);
    led_state = false;
    LOG_INF("LED OFF");
}

// LED闪烁
static void led_blink(uint32_t on_ms, uint32_t off_ms) {
    on_time = on_ms;
    off_time = off_ms;
    is_blinking = true;
    led_state = false;
    k_timer_start(&blink_timer, K_MSEC(on_time), K_NO_WAIT);
    LOG_INF("LED BLINKING: on=%dms, off=%dms", on_ms, off_ms);
}

// 处理蓝牙连接状态
static void update_led_state(void) {
    bool connected = zmk_ble_active_profile_is_connected();
    uint8_t profile_index = zmk_ble_get_active_profile_index();
    
    if (connected) {
        // 连接状态下，根据不同的配置文件显示不同的LED状态
        switch (profile_index) {
            case 0:
                led_on();  // 配置文件1：常亮
                break;
            case 1:
                led_blink(2000, 200);  // 配置文件2：慢闪烁
                break;
            case 2:
                led_blink(1000, 200);  // 配置文件3：中等闪烁
                break;
            default:
                led_on();  // 默认：常亮
                break;
        }
    } else {
        // 未连接状态：快速闪烁
        led_blink(500, 500);
    }
}

// 事件处理函数
static int bt_led_event_listener(const struct zmk_event_t *eh) {
    // 检查是否为蓝牙连接状态变化事件
    if (as_zmk_ble_connected_state_changed(eh)) {
        LOG_INF("Bluetooth connection state changed");
        update_led_state();
        return 0;
    }
    
    // 检查是否为蓝牙活跃配置文件变化事件
    if (as_zmk_ble_active_profile_changed(eh)) {
        const struct zmk_ble_active_profile_changed *ev = 
            as_zmk_ble_active_profile_changed(eh);
        LOG_INF("Active profile changed to index %d", ev->index);
        update_led_state();
        return 0;
    }
    
    // 检查是否为睡眠状态变化事件
    if (as_zmk_sleep_state_changed(eh)) {
        const struct zmk_sleep_state_changed *ev = 
            as_zmk_sleep_state_changed(eh);
        
        if (ev->state) {
            // 进入睡眠模式，关闭LED以节省电量
            led_off();
            LOG_INF("Entering sleep mode, LED OFF");
        } else {
            // 从睡眠模式唤醒，恢复LED状态
            update_led_state();
            LOG_INF("Waking up from sleep, LED state restored");
        }
        return 0;
    }
    
    return 0;
}

// 模块初始化函数
static int bt_led_init(void) {
    LOG_INF("Initializing Bluetooth LED behavior");
    
    // 检查LED设备是否就绪
    if (!device_is_ready(led.port)) {
        LOG_ERR("LED device %s is not ready", led.port->name);
        return -ENODEV;
    }
    
    // 配置GPIO为输出模式
    int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }
    
    // 初始化闪烁定时器
    k_timer_init(&blink_timer, blink_timer_handler, NULL);
    
    // 设置初始LED状态
    update_led_state();
    
    LOG_INF("Bluetooth LED behavior initialized successfully");
    return 0;
}

// 注册事件监听器
ZMK_LISTENER(bt_led, bt_led_event_listener);
ZMK_SUBSCRIPTION(bt_led, zmk_ble_connected_state_changed);
ZMK_SUBSCRIPTION(bt_led, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(bt_led, zmk_sleep_state_changed);

// 系统初始化
SYS_INIT(bt_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else
LOG_ERR("Bluetooth LED node not found in device tree");
#endif /* DT_NODE_HAS_STATUS(BT_LED_NODE, okay) */
