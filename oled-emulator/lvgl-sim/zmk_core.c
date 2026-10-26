/*
 * The slice of ZMK the display code talks to, for one simulated device (a half or the
 * dongle). State is set from the page through sim_* exports in sim.c; changes reach the
 * widgets as the same events ZMK raises. ZMK's own activity, WPM and display-blanking
 * logic is ported here as listeners on those events (zmk/app/src/activity.c, wpm.c,
 * display/main.c in ZMK v0.3.0).
 */
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/display.h>
#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/wpm.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/split/central.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>

#include "sim_target.h"
#include "zmk_core.h"

ZMK_EVENT_IMPL(zmk_activity_state_changed);
ZMK_EVENT_IMPL(zmk_battery_state_changed);
ZMK_EVENT_IMPL(zmk_peripheral_battery_state_changed);
ZMK_EVENT_IMPL(zmk_ble_active_profile_changed);
ZMK_EVENT_IMPL(zmk_endpoint_changed);
ZMK_EVENT_IMPL(zmk_hid_indicators_changed);
ZMK_EVENT_IMPL(zmk_keycode_state_changed);
ZMK_EVENT_IMPL(zmk_layer_state_changed);
ZMK_EVENT_IMPL(zmk_position_state_changed);
ZMK_EVENT_IMPL(zmk_split_peripheral_status_changed);
ZMK_EVENT_IMPL(zmk_usb_conn_state_changed);
ZMK_EVENT_IMPL(zmk_wpm_state_changed);

struct sim_zmk_state sim_zmk = {
    .battery = 80,
    .usb = ZMK_USB_CONN_NONE,
    .split_connected = true,
    .peripheral_battery = {80, 80, 80, 80},
    .transport = ZMK_TRANSPORT_USB,
    .ble_profile = 0,
    .ble_connected = true,
    .ble_open = false,
};

/* ---------------- event manager ---------------- */

#define MAX_SUBSCRIPTIONS 128
static struct {
    const struct zmk_listener *listener;
    const struct zmk_event_type *type;
} subscriptions[MAX_SUBSCRIPTIONS];
static int subscription_count;

void sim_subscribe(const struct zmk_listener *listener, const struct zmk_event_type *type) {
    if (subscription_count < MAX_SUBSCRIPTIONS) {
        subscriptions[subscription_count].listener = listener;
        subscriptions[subscription_count].type = type;
        subscription_count++;
    }
}

int sim_raise(zmk_event_t *event) {
    for (int i = 0; i < subscription_count; i++) {
        if (subscriptions[i].type != event->event) {
            continue;
        }
        int ret = subscriptions[i].listener->callback(event);
        if (ret == ZMK_EV_EVENT_HANDLED || ret == ZMK_EV_EVENT_CAPTURED) {
            return 0;
        }
    }
    return 0;
}

/* ---------------- display work queue ---------------- */

#define MAX_WORK 32
static void (*work_queue[MAX_WORK])(struct k_work *);
static struct k_work *work_items[MAX_WORK];
static int work_count;

static void queue_work(void (*handler)(struct k_work *), struct k_work *item) {
    for (int i = 0; i < work_count; i++) {
        if (work_queue[i] == handler && work_items[i] == item) {
            return; /* already pending, like k_work_submit */
        }
    }
    if (work_count < MAX_WORK) {
        work_queue[work_count] = handler;
        work_items[work_count] = item;
        work_count++;
    }
}

void sim_display_work(void (*handler)(struct k_work *)) { queue_work(handler, NULL); }
void sim_work_submit(struct k_work *work) { queue_work(work->handler, work); }

void sim_run_display_work(void) {
    while (work_count) {
        void (*handler)(struct k_work *) = work_queue[0];
        struct k_work *item = work_items[0];
        work_count--;
        for (int i = 0; i < work_count; i++) {
            work_queue[i] = work_queue[i + 1];
            work_items[i] = work_items[i + 1];
        }
        handler(item);
    }
}

struct k_work_q *zmk_display_work_q() { return NULL; }

/* ---------------- state getters ---------------- */

uint8_t zmk_battery_state_of_charge(void) { return sim_zmk.battery; }
enum zmk_usb_conn_state zmk_usb_get_conn_state(void) { return sim_zmk.usb; }
bool zmk_usb_is_hid_ready(void) { return sim_zmk.usb == ZMK_USB_CONN_HID; }
bool zmk_split_bt_peripheral_is_connected(void) { return sim_zmk.split_connected; }
bool zmk_split_bt_peripheral_is_bonded(void) { return true; }

int zmk_split_central_get_peripheral_battery_level(uint8_t source, uint8_t *level) {
    if (source >= SIM_MAX_PERIPHERALS) {
        return -EINVAL;
    }
    *level = sim_zmk.peripheral_battery[source];
    return 0;
}

struct zmk_endpoint_instance zmk_endpoints_selected(void) {
    struct zmk_endpoint_instance ep = {.transport = sim_zmk.transport};
    if (ep.transport == ZMK_TRANSPORT_BLE) {
        ep.ble.profile_index = sim_zmk.ble_profile;
    }
    return ep;
}
bool zmk_endpoint_instance_eq(struct zmk_endpoint_instance a, struct zmk_endpoint_instance b) {
    return a.transport == b.transport &&
           (a.transport != ZMK_TRANSPORT_BLE || a.ble.profile_index == b.ble.profile_index);
}

int zmk_ble_active_profile_index(void) { return sim_zmk.ble_profile; }
bool zmk_ble_active_profile_is_connected(void) { return sim_zmk.ble_connected; }
bool zmk_ble_active_profile_is_open(void) { return sim_zmk.ble_open; }
bool zmk_ble_profile_is_connected(uint8_t index) { return index == sim_zmk.ble_profile && sim_zmk.ble_connected; }
bool zmk_ble_profile_is_open(uint8_t index) { return index == sim_zmk.ble_profile && sim_zmk.ble_open; }
char *zmk_ble_active_profile_name(void) { return ""; }

zmk_mod_flags_t zmk_hid_get_explicit_mods(void) { return sim_zmk.mods; }

/* The keymap: config/eyelash_corne.keymap's layers, with layer 0 always on. */
static const char *layer_names[] = SIM_LAYER_NAMES;
zmk_keymap_layer_id_t zmk_keymap_layer_index_to_id(zmk_keymap_layer_index_t i) { return i; }
zmk_keymap_layer_id_t zmk_keymap_layer_default(void) { return 0; }
zmk_keymap_layers_state_t zmk_keymap_layer_state(void) { return BIT(0) | BIT(sim_zmk.layer); }
bool zmk_keymap_layer_active(zmk_keymap_layer_id_t layer) { return layer == 0 || layer == sim_zmk.layer; }
zmk_keymap_layer_index_t zmk_keymap_highest_layer_active(void) { return sim_zmk.layer; }
const char *zmk_keymap_layer_name(zmk_keymap_layer_id_t layer) {
    return layer < SIM_LAYER_COUNT ? layer_names[layer] : NULL;
}

/* ---------------- zmk/app/src/activity.c ---------------- */

static enum zmk_activity_state activity_state;
static uint32_t activity_last_uptime;

enum zmk_activity_state zmk_activity_get_state(void) { return activity_state; }
uint32_t sim_ms_since_activity(void) { return sim_uptime_ms - activity_last_uptime; }

static void set_activity(enum zmk_activity_state state) {
    if (activity_state == state) {
        return;
    }
    activity_state = state;
    raise_zmk_activity_state_changed((struct zmk_activity_state_changed){.state = state});
}

static int activity_event_listener(const zmk_event_t *eh) {
    activity_last_uptime = k_uptime_get_32();
    set_activity(ZMK_ACTIVITY_ACTIVE);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);

void sim_activity_work(void) {
    int32_t inactive_time = sim_uptime_ms - activity_last_uptime;
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (inactive_time > CONFIG_ZMK_IDLE_SLEEP_TIMEOUT && !zmk_usb_is_powered()) {
        set_activity(ZMK_ACTIVITY_SLEEP); /* followed by sys_poweroff() */
        return;
    }
#endif
    if (inactive_time > CONFIG_ZMK_IDLE_TIMEOUT) {
        set_activity(ZMK_ACTIVITY_IDLE);
    }
}

/* ---------------- zmk/app/src/display/main.c: blank on idle ---------------- */

bool sim_display_blanked;

#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)
static int display_event_handler(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }
    /* start_display_updates() / stop_display_updates(): panel off and the tick timer stopped */
    sim_display_blanked = ev->state != ZMK_ACTIVITY_ACTIVE;
    return 0;
}
ZMK_LISTENER(display, display_event_handler);
ZMK_SUBSCRIPTION(display, zmk_activity_state_changed);
#endif

/* ---------------- zmk/app/src/wpm.c ---------------- */

#if IS_ENABLED(CONFIG_ZMK_WPM)
#define WPM_UPDATE_INTERVAL_SECONDS 1
#define WPM_RESET_INTERVAL_SECONDS 5
#define CHARS_PER_WORD 5.0

static uint8_t wpm_state = -1;
static uint8_t last_wpm_state;
static uint8_t wpm_update_counter;
static uint32_t key_pressed_count;

int zmk_wpm_get_state(void) { return wpm_state; }

static int wpm_event_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev && !ev->state) { /* count only key up events */
        key_pressed_count++;
    }
    return 0;
}
ZMK_LISTENER(wpm, wpm_event_listener);
ZMK_SUBSCRIPTION(wpm, zmk_keycode_state_changed);

void sim_wpm_init(void) {
    wpm_state = 0;
    wpm_update_counter = 0;
}

void sim_wpm_work(void) {
    wpm_update_counter++;
    wpm_state = (key_pressed_count / CHARS_PER_WORD) /
                (wpm_update_counter * WPM_UPDATE_INTERVAL_SECONDS / 60.0);
    if (last_wpm_state != wpm_state) {
        raise_zmk_wpm_state_changed((struct zmk_wpm_state_changed){.state = wpm_state});
        last_wpm_state = wpm_state;
    }
    if (wpm_update_counter >= WPM_RESET_INTERVAL_SECONDS) {
        wpm_update_counter = 0;
        key_pressed_count = 0;
    }
}
#else
int zmk_wpm_get_state(void) { return 0; }
void sim_wpm_init(void) {}
void sim_wpm_work(void) {}
#endif
