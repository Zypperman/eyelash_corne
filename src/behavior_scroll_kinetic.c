/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_scroll_kinetic

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h> // CLAMP

#include <zmk/behavior.h>
#include <dt-bindings/zmk/pointing.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// QMK "kinetic speed" style curve: speed jumps to initial-speed the instant
// delay-ms elapses (sized so a single trigger-period-ms tick already emits a
// whole unit -- no accumulation lag on a tap), then ramps linearly to
// base-speed over time-to-base-ms for sustained holding.

struct movement_state_1d {
    float remainder;
    int16_t speed; // sign encodes direction; magnitude is unused (config's
                    // initial-speed/base-speed drive the actual rate)
    int64_t start_time;
};

struct movement_state_2d {
    struct movement_state_1d x;
    struct movement_state_1d y;
};

struct behavior_scroll_kinetic_data {
    struct k_work_delayable tick_work;
    const struct device *dev;

    struct movement_state_2d state;
};

struct behavior_scroll_kinetic_config {
    int16_t x_code;
    int16_t y_code;
    uint16_t delay_ms;
    uint16_t time_to_base_ms;
    uint8_t trigger_period_ms;
    int16_t initial_speed;
    int16_t base_speed;
};

static int64_t elapsed_ms_since(int64_t start, int64_t now) {
    if (start == 0) {
        return 0;
    }
    int64_t ticks = now - start;
    if (ticks < 0) {
        ticks = 0;
    }
    return 1000 * ticks / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
}

static float current_magnitude(const struct behavior_scroll_kinetic_config *config,
                                int64_t ramp_elapsed_ms) {
    if (config->time_to_base_ms == 0 || ramp_elapsed_ms >= config->time_to_base_ms) {
        return config->base_speed;
    }

    float t = (float)ramp_elapsed_ms / config->time_to_base_ms;
    return config->initial_speed + (config->base_speed - config->initial_speed) * t;
}

static void track_remainder(float *move, float *remainder) {
    float new_move = *move + *remainder;
    *remainder = new_move - (int)new_move;
    *move = (int)new_move;
}

static float update_movement_1d(const struct behavior_scroll_kinetic_config *config,
                                 struct movement_state_1d *state, int64_t now) {
    if (state->speed == 0) {
        state->remainder = 0;
        return 0;
    }

    int64_t total_elapsed_ms = elapsed_ms_since(state->start_time, now);
    if (total_elapsed_ms < config->delay_ms) {
        return 0;
    }

    float magnitude = current_magnitude(config, total_elapsed_ms - config->delay_ms);
    float direction = (state->speed > 0) ? 1.0f : -1.0f;
    float move = direction * magnitude * config->trigger_period_ms / 1000.0f;

    track_remainder(&move, &state->remainder);

    return move;
}

struct vector2d {
    float x;
    float y;
};

static struct vector2d update_movement_2d(const struct behavior_scroll_kinetic_config *config,
                                          struct movement_state_2d *state, int64_t now) {
    return (struct vector2d){
        .x = update_movement_1d(config, &state->x, now),
        .y = update_movement_1d(config, &state->y, now),
    };
}

static bool is_non_zero_1d_movement(int16_t speed) { return speed != 0; }

static bool is_non_zero_2d_movement(struct movement_state_2d *state) {
    return is_non_zero_1d_movement(state->x.speed) || is_non_zero_1d_movement(state->y.speed);
}

static bool should_be_working(struct behavior_scroll_kinetic_data *data) {
    return is_non_zero_2d_movement(&data->state);
}

static void tick_work_cb(struct k_work *work) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(work);
    struct behavior_scroll_kinetic_data *data =
        CONTAINER_OF(d_work, struct behavior_scroll_kinetic_data, tick_work);
    const struct device *dev = data->dev;
    const struct behavior_scroll_kinetic_config *cfg = dev->config;

    uint64_t timestamp = k_uptime_ticks();

    struct vector2d move = update_movement_2d(cfg, &data->state, timestamp);

    int ret = 0;
    bool have_x = is_non_zero_1d_movement((int16_t)move.x);
    bool have_y = is_non_zero_1d_movement((int16_t)move.y);
    if (have_x) {
        ret = input_report_rel(dev, cfg->x_code, (int16_t)CLAMP(move.x, INT16_MIN, INT16_MAX),
                               !have_y, K_NO_WAIT);
    }
    if (have_y) {
        ret = input_report_rel(dev, cfg->y_code, (int16_t)CLAMP(move.y, INT16_MIN, INT16_MAX), true,
                               K_NO_WAIT);
    }

    if (should_be_working(data)) {
        k_work_schedule(&data->tick_work, K_MSEC(cfg->trigger_period_ms));
    }
}

static void set_start_times_for_activity_1d(struct movement_state_1d *state) {
    if (state->speed != 0 && state->start_time == 0) {
        state->start_time = k_uptime_ticks();
    } else if (state->speed == 0) {
        state->start_time = 0;
    }
}
static void set_start_times_for_activity(struct movement_state_2d *state) {
    set_start_times_for_activity_1d(&state->x);
    set_start_times_for_activity_1d(&state->y);
}

static void update_work_scheduling(const struct device *dev) {
    struct behavior_scroll_kinetic_data *data = dev->data;
    const struct behavior_scroll_kinetic_config *cfg = dev->config;

    set_start_times_for_activity(&data->state);

    if (should_be_working(data)) {
        k_work_schedule(&data->tick_work, K_MSEC(cfg->trigger_period_ms));
    } else {
        k_work_cancel_delayable(&data->tick_work);
        data->state.y.remainder = 0;
        data->state.x.remainder = 0;
    }
}

int behavior_scroll_kinetic_adjust_speed(const struct device *dev, int16_t dx, int16_t dy) {
    struct behavior_scroll_kinetic_data *data = dev->data;

    LOG_DBG("Adjusting: %d %d", dx, dy);
    data->state.x.speed += dx;
    data->state.y.speed += dy;

    update_work_scheduling(dev);

    return 0;
}

static int behavior_scroll_kinetic_init(const struct device *dev) {
    struct behavior_scroll_kinetic_data *data = dev->data;

    data->dev = dev;
    k_work_init_delayable(&data->tick_work, tick_work_cb);

    return 0;
};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {

    const struct device *behavior_dev = zmk_behavior_get_binding(binding->behavior_dev);

    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    int16_t x = MOVE_X_DECODE(binding->param1);
    int16_t y = MOVE_Y_DECODE(binding->param1);

    behavior_scroll_kinetic_adjust_speed(behavior_dev, x, y);
    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *behavior_dev = zmk_behavior_get_binding(binding->behavior_dev);

    LOG_DBG("position %d keycode 0x%02X", event.position, binding->param1);

    int16_t x = MOVE_X_DECODE(binding->param1);
    int16_t y = MOVE_Y_DECODE(binding->param1);

    behavior_scroll_kinetic_adjust_speed(behavior_dev, -x, -y);
    return 0;
}

static const struct behavior_driver_api behavior_scroll_kinetic_driver_api = {
    .binding_pressed = on_keymap_binding_pressed, .binding_released = on_keymap_binding_released};

#define SK_INST(n)                                                                                \
    static struct behavior_scroll_kinetic_data behavior_scroll_kinetic_data_##n = {};            \
    static struct behavior_scroll_kinetic_config behavior_scroll_kinetic_config_##n = {           \
        .x_code = DT_INST_PROP(n, x_input_code),                                                  \
        .y_code = DT_INST_PROP(n, y_input_code),                                                  \
        .trigger_period_ms = DT_INST_PROP(n, trigger_period_ms),                                  \
        .delay_ms = DT_INST_PROP_OR(n, delay_ms, 0),                                              \
        .time_to_base_ms = DT_INST_PROP_OR(n, time_to_base_ms, 0),                                \
        .initial_speed = DT_INST_PROP(n, initial_speed),                                          \
        .base_speed = DT_INST_PROP(n, base_speed),                                                \
    };                                                                                            \
    BEHAVIOR_DT_INST_DEFINE(                                                                      \
        n, behavior_scroll_kinetic_init, NULL, &behavior_scroll_kinetic_data_##n,                 \
        &behavior_scroll_kinetic_config_##n, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,    \
        &behavior_scroll_kinetic_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SK_INST)
