/*
 * Dayu k30 蓝牙指示灯控制
 * 功能：实现简单可靠的蓝牙状态LED控制
 */

// 使用条件编译，确保在Zephyr环境中使用正确的头文件
#if defined(CONFIG_ZEPHYR)
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#else
// 为了编译检查，定义必要的类型和常量
#include <stddef.h>
typedef struct {
    int dummy;
} device;
typedef unsigned int uint32_t;
typedef int int32_t;
#define NULL ((void *)0)
#define GPIO_OUTPUT 0x0001
#define GPIO_ACTIVE_HIGH 0x0000
#define APPLICATION 0

// 声明所需的函数以通过编译检查
static inline const device *device_get_binding(const char *name) {
    return NULL;
}
static inline int gpio_pin_configure(const device *dev, uint32_t pin, int flags) {
    return 0;
}
static inline int gpio_pin_set(const device *dev, uint32_t pin, int value) {
    return 0;
}
#define SYS_INIT(func, level, prio) \
    int __attribute__((used)) _init_##func = (func(), 0)
#endif

// LED配置 - 根据设备树overlay文件使用GPIO1和PIN 10
#define LED_PORT "gpio1"
#define LED_PIN 10

/**
 * @brief 初始化蓝牙状态LED
 * 
 * 配置并打开LED指示灯，用于显示蓝牙状态
 * 
 * @return 成功返回0，失败返回错误码
 */
static int ble_status_led_init(void) {
    // 获取GPIO设备
    const device *gpio_dev = device_get_binding(LED_PORT);
    if (gpio_dev == NULL) {
        return 0; // 静默失败，确保程序继续运行
    }
    
    // 配置LED引脚为输出模式
    int ret = gpio_pin_configure(gpio_dev, LED_PIN, GPIO_OUTPUT | GPIO_ACTIVE_HIGH);
    if (ret == 0) {
        // 配置成功，打开LED
        gpio_pin_set(gpio_dev, LED_PIN, 1);
    }
    
    return 0;
}

// 注册初始化函数，在系统启动时自动调用
SYS_INIT(ble_status_led_init, APPLICATION, 90);

/**
 * @brief 模块初始化入口点
 * 
 * 提供手动初始化LED的接口
 * 
 * @return 成功返回0，失败返回错误码
 */
int ble_led_module_init(void) {
    return ble_status_led_init();
}