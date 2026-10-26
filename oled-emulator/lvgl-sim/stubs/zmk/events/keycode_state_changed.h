/* zmk/app/include/zmk/events/keycode_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
struct zmk_keycode_state_changed {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t implicit_modifiers;
    uint8_t explicit_modifiers;
    bool state;
    int64_t timestamp;
};
ZMK_EVENT_DECLARE(zmk_keycode_state_changed);
