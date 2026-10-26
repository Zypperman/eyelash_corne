/* zmk/app/include/zmk/events/battery_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
struct zmk_battery_state_changed {
    uint8_t state_of_charge;
};
ZMK_EVENT_DECLARE(zmk_battery_state_changed);

struct zmk_peripheral_battery_state_changed {
    uint8_t source;
    uint8_t state_of_charge;
};
ZMK_EVENT_DECLARE(zmk_peripheral_battery_state_changed);
