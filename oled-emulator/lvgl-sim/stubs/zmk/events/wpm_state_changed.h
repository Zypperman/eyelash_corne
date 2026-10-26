/* zmk/app/include/zmk/events/wpm_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/wpm.h>
struct zmk_wpm_state_changed {
    int state;
};
ZMK_EVENT_DECLARE(zmk_wpm_state_changed);
