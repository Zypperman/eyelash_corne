/* zmk/app/include/zmk/events/usb_conn_state_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/usb.h>
struct zmk_usb_conn_state_changed {
    enum zmk_usb_conn_state conn_state;
};
ZMK_EVENT_DECLARE(zmk_usb_conn_state_changed);
