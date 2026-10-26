/* zmk/app/include/zmk/display/widgets/layer_status.h (ZMK v0.3.0): the declarations the display code uses */
#pragma once
#include <zephyr/kernel.h>
#include <lvgl.h>
struct zmk_widget_layer_status {
    sys_snode_t node;
    lv_obj_t *obj;
};
int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget);
