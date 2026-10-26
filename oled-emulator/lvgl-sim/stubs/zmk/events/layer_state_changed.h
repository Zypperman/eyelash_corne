/* zmk/app/include/zmk/events/layer_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
struct zmk_layer_state_changed {
    uint8_t layer;
    bool state;
    int64_t timestamp;
};
ZMK_EVENT_DECLARE(zmk_layer_state_changed);
