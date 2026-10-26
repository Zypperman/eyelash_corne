/* zmk/app/include/zmk/hid.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <dt-bindings/zmk/modifiers.h>
typedef uint8_t zmk_mod_flags_t;
zmk_mod_flags_t zmk_hid_get_explicit_mods(void);
