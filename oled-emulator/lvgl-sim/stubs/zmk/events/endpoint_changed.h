/* zmk/app/include/zmk/events/endpoint_changed.h (ZMK v0.3.0) */
#pragma once
#include <zmk/event_manager.h>
#include <zmk/endpoints_types.h>
struct zmk_endpoint_changed {
    struct zmk_endpoint_instance endpoint;
};
ZMK_EVENT_DECLARE(zmk_endpoint_changed);
