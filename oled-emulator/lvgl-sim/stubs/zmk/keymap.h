/* zmk/app/include/zmk/keymap.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <zmk/events/position_state_changed.h>
typedef uint8_t zmk_keymap_layer_id_t;
#define ZMK_KEYMAP_LAYER_ID_INVAL UINT8_MAX
typedef uint8_t zmk_keymap_layer_index_t;
typedef uint32_t zmk_keymap_layers_state_t;
zmk_keymap_layer_id_t zmk_keymap_layer_index_to_id(zmk_keymap_layer_index_t layer_index);
zmk_keymap_layer_id_t zmk_keymap_layer_default(void);
zmk_keymap_layers_state_t zmk_keymap_layer_state(void);
bool zmk_keymap_layer_active(zmk_keymap_layer_id_t layer);
zmk_keymap_layer_index_t zmk_keymap_highest_layer_active(void);
const char *zmk_keymap_layer_name(zmk_keymap_layer_id_t layer);
