/*
 * ZMK APIs the nice!oled widgets call, answered from simulator state that the
 * page controls (battery level, USB power, split link).
 */
#pragma once
#include <zephyr/kernel.h>

typedef struct zmk_event zmk_event_t;
#define ZMK_EV_EVENT_BUBBLE 0

struct zmk_battery_state_changed { uint8_t state_of_charge; };
struct zmk_usb_conn_state_changed { int conn_state; };
struct zmk_split_peripheral_status_changed { bool connected; };
static inline const struct zmk_battery_state_changed *as_zmk_battery_state_changed(const zmk_event_t *eh) { return NULL; }
static inline const struct zmk_usb_conn_state_changed *as_zmk_usb_conn_state_changed(const zmk_event_t *eh) { return NULL; }
static inline const struct zmk_split_peripheral_status_changed *as_zmk_split_peripheral_status_changed(const zmk_event_t *eh) { return NULL; }

enum zmk_transport { ZMK_TRANSPORT_NONE, ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
struct zmk_endpoint_instance {
    enum zmk_transport transport;
    union { struct { uint8_t profile_index; } ble; };
};

uint8_t zmk_battery_state_of_charge(void);
bool zmk_usb_is_powered(void);
bool zmk_split_bt_peripheral_is_connected(void);
int zmk_display_is_initialized(void);

/* Listeners register a refresh hook; the sim calls them all when its state changes,
 * standing in for ZMK's event manager + display work queue. */
void sim_register_listener(void (*refresh)(void));

/* zmk/display.h, minus the mutex and work queue */
#define ZMK_DISPLAY_WIDGET_LISTENER(listener, state_type, cb, state_func)                          \
    static state_type __##listener##_state;                                                        \
    static void listener##_refresh(void) {                                                         \
        __##listener##_state = state_func(NULL);                                                   \
        cb(__##listener##_state);                                                                  \
    }                                                                                              \
    static void listener##_init() {                                                                \
        sim_register_listener(listener##_refresh);                                                 \
        listener##_refresh();                                                                      \
    }
#define ZMK_LISTENER(...)
#define ZMK_SUBSCRIPTION(...)
