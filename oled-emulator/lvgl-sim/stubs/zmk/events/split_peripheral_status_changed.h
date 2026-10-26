/* zmk/app/include/zmk/events/split_peripheral_status_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
struct zmk_split_peripheral_status_changed {
    bool connected;
};
ZMK_EVENT_DECLARE(zmk_split_peripheral_status_changed);
