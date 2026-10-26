/* zmk/app/include/zmk/ble/profile.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#define ZMK_BLE_PROFILE_NAME_MAX 15
struct zmk_ble_profile {
    char name[ZMK_BLE_PROFILE_NAME_MAX];
    uint8_t peer[7];
};
