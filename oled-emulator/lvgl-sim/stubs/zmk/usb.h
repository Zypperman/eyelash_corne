/* zmk/app/include/zmk/usb.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
enum zmk_usb_conn_state {
    ZMK_USB_CONN_NONE,
    ZMK_USB_CONN_POWERED,
    ZMK_USB_CONN_HID,
};
enum zmk_usb_conn_state zmk_usb_get_conn_state(void);
static inline bool zmk_usb_is_powered(void) { return zmk_usb_get_conn_state() != ZMK_USB_CONN_NONE; }
bool zmk_usb_is_hid_ready(void);
