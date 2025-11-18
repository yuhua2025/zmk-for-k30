/*
 * Dayu k30 蓝牙指示灯控制
 * 功能：实现简单的LED闪烁功能，验证LED硬件工作正常
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

// LED引脚配置 - 使用gpio1的10号引脚，与nrfmicro_13.overlay中的LED0配置一致
#define LED_GPIO_LABEL DT_GPIO_LABEL(DT_ALIAS(led0), gpios)
#define LED_GPIO_PIN DT_GPIO_PIN(DT_ALIAS(led0), gpios)
#define LED_GPIO_FLAGS (GPIO_OUTPUT_ACTIVE | DT_GPIO_FLAGS(DT_ALIAS(led0), gpios))

// 全局变量
static const struct device *led_dev;
static struct k_work_delayable led_blink_work;

// LED闪烁工作函数
static void led_blink_handler(struct k_work *work) {
    static bool led_state = false;
    
    // 切换LED状态
    led_state = !led_state;
    
    // 设置LED引脚状态
    if (led_dev != NULL) {
        gpio_pin_set(led_dev, LED_GPIO_PIN, led_state);
    }
    
    // 再次调度工作，实现周期性闪烁
    k_work_schedule(&led_blink_work, K_MSEC(250));
}

// 初始化函数
static int led_init(void) {
    // 获取GPIO设备
    led_dev = device_get_binding(LED_GPIO_LABEL);
    if (led_dev == NULL) {
        return -1;  // 简化错误处理
    }
    
    // 配置LED引脚
    if (gpio_pin_configure(led_dev, LED_GPIO_PIN, LED_GPIO_FLAGS) != 0) {
        return -1;  // 简化错误处理
    }
    
    // 初始化工作队列
    k_work_init_delayable(&led_blink_work, led_blink_handler);
    
    // 立即开始闪烁
    k_work_schedule(&led_blink_work, K_NO_WAIT);
    
    return 0;
}

// 系统启动时自动初始化
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);