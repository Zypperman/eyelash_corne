/* zmk/app/include/zmk/ble.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <zmk/ble/profile.h>
#define ZMK_BLE_IS_CENTRAL (IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
#if ZMK_BLE_IS_CENTRAL
#define ZMK_BLE_PROFILE_COUNT (CONFIG_BT_MAX_PAIRED - CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
#define ZMK_SPLIT_BLE_PERIPHERAL_COUNT CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define ZMK_BLE_PROFILE_COUNT CONFIG_BT_MAX_PAIRED
#endif
int zmk_ble_active_profile_index(void);
bool zmk_ble_profile_is_connected(uint8_t index);
bool zmk_ble_profile_is_open(uint8_t index);
bool zmk_ble_active_profile_is_open(void);
bool zmk_ble_active_profile_is_connected(void);
char *zmk_ble_active_profile_name(void);
