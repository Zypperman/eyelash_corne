/* zmk/app/include/zmk/endpoints.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <zmk/ble.h>
#include <zmk/endpoints_types.h>
#define ZMK_ENDPOINT_STR_LEN 10
struct zmk_endpoint_instance zmk_endpoints_selected(void);
bool zmk_endpoint_instance_eq(struct zmk_endpoint_instance a, struct zmk_endpoint_instance b);
