/*
 * Per-layer RGB underglow.
 *
 * Watches layer changes on the split central (the jerry_dongle) and, for the
 * highest active layer listed under the "zmk,layer-rgb" node
 * (config/layer_rgb.dtsi), invokes the stock &rgb_ug behavior to set its
 * color, effect and speed. &rgb_ug is a global behavior, so ZMK forwards each
 * call to both halves and they stay in sync.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/rgb.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(layer_rgb, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_layer_rgb

#define RGB_UG_NAME DEVICE_DT_NAME(DT_NODELABEL(rgb_ug))
#define NO_EFFECT -1

struct layer_rgb_cfg {
    zmk_keymap_layer_id_t layer;
    uint16_t h;
    uint8_t s;
    uint8_t b;
    int8_t effect;
    uint8_t speed; /* 0 = leave as is */
};

#define LAYER_RGB_CFG(node)                                                                        \
    BUILD_ASSERT(DT_PROP_LEN(node, color_hsb) == 3, "color-hsb needs <hue sat brightness>");       \
    BUILD_ASSERT(DT_PROP_BY_IDX(node, color_hsb, 0) < 360, "hue must be 0-359");                   \
    BUILD_ASSERT(DT_PROP_BY_IDX(node, color_hsb, 1) <= 100, "saturation must be 0-100");           \
    BUILD_ASSERT(DT_PROP_BY_IDX(node, color_hsb, 2) <= 100, "brightness must be 0-100");           \
    BUILD_ASSERT(DT_PROP_OR(node, effect, 0) <= 3, "effect must be 0-3");                          \
    BUILD_ASSERT(DT_PROP_OR(node, speed, 1) >= 1 && DT_PROP_OR(node, speed, 1) <= 5,               \
                 "speed must be 1-5");

#define LAYER_RGB_ENTRY(node)                                                                      \
    {                                                                                              \
        .layer = DT_PROP(node, layer),                                                             \
        .h = DT_PROP_BY_IDX(node, color_hsb, 0),                                                   \
        .s = DT_PROP_BY_IDX(node, color_hsb, 1),                                                   \
        .b = DT_PROP_BY_IDX(node, color_hsb, 2),                                                   \
        .effect = DT_PROP_OR(node, effect, NO_EFFECT),                                             \
        .speed = DT_PROP_OR(node, speed, 0),                                                       \
    },

DT_INST_FOREACH_CHILD(0, LAYER_RGB_CFG)

static const struct layer_rgb_cfg configs[] = {DT_INST_FOREACH_CHILD(0, LAYER_RGB_ENTRY)};

static const struct layer_rgb_cfg *applied;

static const struct layer_rgb_cfg *config_for_layer(zmk_keymap_layer_id_t layer) {
    for (size_t i = 0; i < ARRAY_SIZE(configs); i++) {
        if (configs[i].layer == layer) {
            return &configs[i];
        }
    }
    return NULL;
}

/* Highest active layer that has an entry; layers without one fall through. */
static const struct layer_rgb_cfg *active_config(void) {
    for (int idx = ZMK_KEYMAP_LAYERS_LEN - 1; idx >= 0; idx--) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);
        if (id == ZMK_KEYMAP_LAYER_ID_INVAL || !zmk_keymap_layer_active(id)) {
            continue;
        }
        const struct layer_rgb_cfg *cfg = config_for_layer(id);
        if (cfg) {
            return cfg;
        }
    }
    return NULL;
}

static void rgb_ug(uint32_t cmd, uint32_t param) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = RGB_UG_NAME,
        .param1 = cmd,
        .param2 = param,
    };
    struct zmk_behavior_binding_event event = {
        .layer = 0,
        .position = 0,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int err = zmk_behavior_invoke_binding(&binding, event, true);
    if (err < 0) {
        LOG_WRN("rgb_ug command %d failed (%d)", cmd, err);
    }
}

static void apply_work_cb(struct k_work *work) {
    const struct layer_rgb_cfg *cfg = active_config();
    if (!cfg || cfg == applied) {
        return;
    }
    applied = cfg;

    LOG_DBG("Layer %d: hsb %d/%d/%d effect %d speed %d", cfg->layer, cfg->h, cfg->s, cfg->b,
            cfg->effect, cfg->speed);

    if (cfg->effect != NO_EFFECT) {
        rgb_ug(RGB_EFS_CMD, cfg->effect);
    }

    /* &rgb_ug has no "set speed" command: bottom out at 1, then step up. */
    if (cfg->speed) {
        for (int i = 0; i < 4; i++) {
            rgb_ug(RGB_SPD_CMD, 0);
        }
        for (int i = 1; i < cfg->speed; i++) {
            rgb_ug(RGB_SPI_CMD, 0);
        }
    }

    rgb_ug(RGB_COLOR_HSB_CMD, RGB_COLOR_HSB_VAL(cfg->h, cfg->s, cfg->b));
}

static K_WORK_DELAYABLE_DEFINE(apply_work, apply_work_cb);

static int layer_rgb_listener(const zmk_event_t *eh) {
    k_work_reschedule(&apply_work, K_MSEC(CONFIG_ZMK_LAYER_RGB_DEBOUNCE_MS));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_rgb, layer_rgb_listener);
ZMK_SUBSCRIPTION(layer_rgb, zmk_layer_state_changed);

static int layer_rgb_init(void) {
    k_work_schedule(&apply_work, K_MSEC(CONFIG_ZMK_LAYER_RGB_BOOT_DELAY_MS));
    return 0;
}

SYS_INIT(layer_rgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
