/* zmk/app/include/zmk/display.h; widget work runs on the simulator's display work queue. */
#pragma once
#include <zmk/event_manager.h>

struct k_work_q *zmk_display_work_q();
bool zmk_display_is_initialized();
void sim_display_work(void (*handler)(struct k_work *));

#define ZMK_DISPLAY_WIDGET_LISTENER(listener, state_type, cb, state_func)                          \
    static state_type __##listener##_state;                                                        \
    static state_type listener##_get_local_state() { return __##listener##_state; };               \
    static void listener##_work_cb(struct k_work *work) { cb(listener##_get_local_state()); };     \
    static void listener##_refresh_state(const zmk_event_t *eh) {                                  \
        __##listener##_state = state_func(eh);                                                     \
    };                                                                                             \
    static void listener##_init() {                                                                \
        listener##_refresh_state(NULL);                                                            \
        listener##_work_cb(NULL);                                                                  \
    }                                                                                              \
    static int listener##_cb(const zmk_event_t *eh) {                                              \
        if (zmk_display_is_initialized()) {                                                        \
            listener##_refresh_state(eh);                                                          \
            sim_display_work(listener##_work_cb);                                                  \
        }                                                                                          \
        return ZMK_EV_EVENT_BUBBLE;                                                                \
    }                                                                                              \
    ZMK_LISTENER(listener, listener##_cb);
