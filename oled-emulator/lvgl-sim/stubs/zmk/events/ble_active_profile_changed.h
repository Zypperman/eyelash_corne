/* zmk/app/include/zmk/events/ble_active_profile_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/ble/profile.h>
struct zmk_ble_active_profile_changed {
    uint8_t index;
    struct zmk_ble_profile *profile;
};
ZMK_EVENT_DECLARE(zmk_ble_active_profile_changed);
