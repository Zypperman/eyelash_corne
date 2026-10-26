/*
 * ZMK's event manager, as the simulator runs it: events are typed structs raised to
 * every subscribed listener in order, synchronously. Listeners and subscriptions
 * register themselves at startup (constructors stand in for ZMK's iterable sections).
 */
#pragma once
#include <zephyr/kernel.h>

struct zmk_event_type {
    const char *name;
};

typedef struct {
    const struct zmk_event_type *event;
    uint8_t last_listener_index;
} zmk_event_t;

struct zmk_listener {
    int (*callback)(const zmk_event_t *eh);
};

#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_EV_EVENT_HANDLED 1
#define ZMK_EV_EVENT_CAPTURED 2

void sim_subscribe(const struct zmk_listener *listener, const struct zmk_event_type *type);
int sim_raise(zmk_event_t *event);

/* As in ZMK, as_*() is out of line and dereferences eh without a NULL check: widgets
 * that call it with NULL at init read whatever sits at address 0, as on the device. */
#define ZMK_EVENT_DECLARE(event_type)                                                              \
    struct event_type##_event {                                                                    \
        zmk_event_t header;                                                                        \
        struct event_type data;                                                                    \
    };                                                                                             \
    extern const struct zmk_event_type zmk_event_##event_type;                                     \
    struct event_type *as_##event_type(const zmk_event_t *eh);                                     \
    int raise_##event_type(struct event_type);

#define ZMK_EVENT_IMPL(event_type)                                                                 \
    const struct zmk_event_type zmk_event_##event_type = {.name = #event_type};                    \
    struct event_type *as_##event_type(const zmk_event_t *eh) {                                    \
        return (eh->event == &zmk_event_##event_type) ? &((struct event_type##_event *)eh)->data   \
                                                      : NULL;                                      \
    }                                                                                              \
    int raise_##event_type(struct event_type data) {                                               \
        struct event_type##_event ev = {.header = {.event = &zmk_event_##event_type}, .data = data}; \
        return sim_raise(&ev.header);                                                              \
    }

#define ZMK_LISTENER(mod, cb) const struct zmk_listener zmk_listener_##mod = {.callback = cb};

#define ZMK_SUBSCRIPTION(mod, ev_type)                                                             \
    __attribute__((constructor)) static void zmk_subscription_##mod##_##ev_type(void) {           \
        sim_subscribe(&zmk_listener_##mod, &zmk_event_##ev_type);                                  \
    }
