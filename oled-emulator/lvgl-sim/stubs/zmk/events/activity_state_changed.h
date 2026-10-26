/* zmk/app/include/zmk/events/activity_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/activity.h>
struct zmk_activity_state_changed {
    enum zmk_activity_state state;
};
ZMK_EVENT_DECLARE(zmk_activity_state_changed);
