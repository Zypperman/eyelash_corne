/* zmk/app/include/zmk/split/central.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <zmk/ble.h>
#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT
int zmk_split_central_get_peripheral_battery_level(uint8_t source, uint8_t *level);
