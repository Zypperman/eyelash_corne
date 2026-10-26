/* zmk/app/include/zmk/events/hid_indicators_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/hid_indicators_types.h>
struct zmk_hid_indicators_changed {
    zmk_hid_indicators_t indicators;
};
ZMK_EVENT_DECLARE(zmk_hid_indicators_changed);
