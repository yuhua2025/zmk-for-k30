/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/init.h>

// 硬编码GPIO引脚配置，使用nice_nano_v2上的引脚
#define LED_PORT "GPIO_1"
#define LED_PIN 10

// 全局变量声明
static const struct device *led_dev; 

/**
 * 初始化函数
 */
static int led_init(void) {
    int ret = 0;
    
    // 获取GPIO设备
    led_dev = device_get_binding(LED_PORT);
    if (led_dev == NULL) {
        return -1;
    }
    
    // 配置LED引脚为输出
    ret = gpio_pin_configure(led_dev, LED_PIN, GPIO_OUTPUT);
    if (ret != 0) {
        return ret;
    }
    
    // 初始化LED状态为关闭
    (void)gpio_pin_set(led_dev, LED_PIN, 0);
    
    return 0;
}

// 自动初始化
SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);