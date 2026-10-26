/* zmk/app/include/zmk/activity.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };
enum zmk_activity_state zmk_activity_get_state(void);
