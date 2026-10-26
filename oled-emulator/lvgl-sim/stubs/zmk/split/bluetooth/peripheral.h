/* zmk/app/include/zmk/split/bluetooth/peripheral.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
bool zmk_split_bt_peripheral_is_connected(void);
bool zmk_split_bt_peripheral_is_bonded(void);
