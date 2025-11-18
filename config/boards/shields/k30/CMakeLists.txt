# Dayu k30 keyboard shield configuration
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(k30)

# Add the BLE status LED source file to the build
zephyr_library_sources(ble_status_led.c)