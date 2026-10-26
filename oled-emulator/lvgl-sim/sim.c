/*
 * One simulated display (a keyboard half or the dongle), running real LVGL and the
 * firmware's display module, driven from the page through the exported sim_* functions.
 *
 * The display path is a port of what the firmware runs:
 *  - zephyr/modules/lvgl/lvgl.c and lvgl_display_mono.c (Zephyr v3.5.0+zmk-fixes):
 *    render buffer sizing and the mono set_px/rounder/flush callbacks for the driver's
 *    capabilities: ls0xx (PIXEL_FORMAT_MONO01, SCREEN_INFO_X_ALIGNMENT_WIDTH) or
 *    SSD1306/SH1106 (PIXEL_FORMAT_MONO10, SCREEN_INFO_MONO_VTILED)
 *  - zephyr/drivers/display/ls0xx.c: whole-line writes into the Sharp memory LCD;
 *    ssd1306.c: its default (SSD1306) and SH1106 write paths into controller RAM
 *  - zmk/app/src/display/main.c (ZMK v0.3.0): theme, status screen, the display work
 *    queue, and lv_task_handler() every CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS
 * The page reads the visible panel and applies the devicetree's inversion-on (OLEDs) and
 * the mounting itself. The ZMK side (events, activity, WPM) is in zmk_core.c.
 */
#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/activity.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/wpm.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#include "sim_kcfg.h"
#include "sim_target.h"
#include "zmk_core.h"

#define EXPORT(name) __attribute__((export_name(#name)))

#define PANEL_W SIM_PANEL_W
#define PANEL_H SIM_PANEL_H
#define PAGES (PANEL_H / 8)
#define LINE_BYTES (PANEL_W / 8) /* ls0xx: one memory line */
/* SH1106 RAM is 132 columns; 128-column panels are wired to columns 2..129. */
#define RAM_W (SIM_PANEL_SH1106 ? 132 : PANEL_W)
#define RAM_VISIBLE_OFFSET (SIM_PANEL_SH1106 ? 2 : 0)

uint32_t sim_uptime_ms;
int sim_kcfg[SIM_KCFG_COUNT] = SIM_KCFG_DEFAULTS;

EXPORT(sim_kcfg_ptr) int *sim_kcfg_ptr(void) { return sim_kcfg; }

static bool initialized;
bool zmk_display_is_initialized() { return initialized; }

/* ---------------- page controls: state changes raise the events ZMK would ---------------- */

EXPORT(sim_set_battery) void sim_set_battery(int level) {
    sim_zmk.battery = level;
    if (initialized) raise_zmk_battery_state_changed((struct zmk_battery_state_changed){.state_of_charge = level});
}

EXPORT(sim_set_usb) void sim_set_usb(int conn_state) {
    sim_zmk.usb = conn_state;
    if (initialized) raise_zmk_usb_conn_state_changed((struct zmk_usb_conn_state_changed){.conn_state = conn_state});
}

EXPORT(sim_set_split_connected) void sim_set_split_connected(int connected) {
    sim_zmk.split_connected = connected;
    if (initialized) {
        raise_zmk_split_peripheral_status_changed(
            (struct zmk_split_peripheral_status_changed){.connected = connected});
    }
}

EXPORT(sim_set_peripheral_battery) void sim_set_peripheral_battery(int source, int level) {
    if (source < 0 || source >= SIM_MAX_PERIPHERALS) return;
    sim_zmk.peripheral_battery[source] = level;
    if (initialized) {
        raise_zmk_peripheral_battery_state_changed(
            (struct zmk_peripheral_battery_state_changed){.source = source, .state_of_charge = level});
    }
}

EXPORT(sim_set_output) void sim_set_output(int transport, int profile, int connected, int open) {
    sim_zmk.transport = transport;
    sim_zmk.ble_profile = profile;
    sim_zmk.ble_connected = connected;
    sim_zmk.ble_open = open;
    if (initialized) {
        raise_zmk_ble_active_profile_changed((struct zmk_ble_active_profile_changed){.index = profile});
        raise_zmk_endpoint_changed((struct zmk_endpoint_changed){.endpoint = zmk_endpoints_selected()});
    }
}

EXPORT(sim_set_hid_indicators) void sim_set_hid_indicators(int indicators) {
    if (initialized) {
        raise_zmk_hid_indicators_changed((struct zmk_hid_indicators_changed){.indicators = indicators});
    }
}

/* A key went down or up. `source` is the split peripheral it came from on a central
 * (UINT8_MAX for a local key); on a central, `keycode` is what the keymap sent (HID usage). */
EXPORT(sim_key) void sim_key(int source, int position, int pressed, int keycode) {
    if (zmk_activity_get_state() == ZMK_ACTIVITY_SLEEP) {
        return; /* sys_poweroff() happened; the page reboots this device */
    }
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = source, .position = position, .state = pressed, .timestamp = k_uptime_get()});
#if SIM_ROLE_CENTRAL
    if (keycode >= 0xe0 && keycode <= 0xe7) {
        uint8_t bit = BIT(keycode - 0xe0);
        sim_zmk.mods = pressed ? (sim_zmk.mods | bit) : (sim_zmk.mods & ~bit);
    }
    if (keycode) {
        raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
            .usage_page = 0x07, .keycode = keycode, .state = pressed, .timestamp = k_uptime_get()});
    }
#endif
}

/* ---------------- controller RAM ---------------- */

static uint32_t flush_count, flush_rejects;

#if SIM_PANEL_LS0XX

/* The Sharp memory LCD's lines, as ls0xx.c sends them: PANEL_W / 8 bytes each, shifted out
 * LSB first, so bit 0 is the leftmost pixel; 1 is white. */
static uint8_t ram[PANEL_H * LINE_BYTES];
static uint8_t visible[PANEL_H * LINE_BYTES];

static void controller_init(void) {
    memset(ram, 0xff, sizeof(ram)); /* ls0xx_init(): ls0xx_clear(), all white */
}

/* The visible panel, in line format: byte (y * width / 8 + x / 8), bit x % 8. */
EXPORT(sim_panel) uint8_t *sim_panel(void) {
    memcpy(visible, ram, sizeof(ram));
    return visible;
}

static void controller_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *buf,
                             size_t buf_size) {
    /* ls0xx_write(): whole lines only (LOG_ERR + -EINVAL); the lines keep their old contents */
    if (buf == NULL || w != PANEL_W || x != 0 || y + h > PANEL_H || buf_size < (size_t)h * LINE_BYTES) {
        flush_rejects++;
        return;
    }
    /* ls0xx_update_display(): each line is its number, PANEL_W / 8 data bytes, a dummy byte */
    for (uint16_t line = 0; line < h; line++) {
        memcpy(ram + (y + line) * LINE_BYTES, buf + line * LINE_BYTES, LINE_BYTES);
    }
}

#else

static uint8_t ram[PAGES * RAM_W];
static uint8_t visible[PAGES * PANEL_W];

static void controller_init(void) {}

/* The visible panel, in page format: byte (page * width + x), bit 0 = top row of the page. */
EXPORT(sim_panel) uint8_t *sim_panel(void) {
    for (int p = 0; p < PAGES; p++) {
        memcpy(visible + p * PANEL_W, ram + p * RAM_W + RAM_VISIBLE_OFFSET, PANEL_W);
    }
    return visible;
}

static void controller_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *buf,
                             size_t buf_size) {
    size_t buf_len = MIN(buf_size, (size_t)h * w / 8);

    /* ssd1306_write(): the driver rejects these (LOG_ERR + return -1); the region keeps its old contents */
    if (buf_len == 0 || (y & 0x7) != 0) {
        flush_rejects++;
        return;
    }
#if SIM_PANEL_SH1106
    /* ssd1306_write_sh1106(): page addressing, one page at a time, column + segment-offset */
    uint16_t x_offset = x + SIM_SEGMENT_OFFSET;
    const uint8_t *p = buf;
    for (uint16_t n = 0; n < h / 8; n++) {
        uint16_t page = n + y / 8;
        for (uint16_t i = 0; i < w; i++) {
            if (page < PAGES && x_offset + i < RAM_W) {
                ram[page * RAM_W + x_offset + i] = p[i];
            }
        }
        p += w;
        if (p > buf + buf_len) {
            flush_rejects++; /* "Exceeded buffer length" */
            return;
        }
    }
#else
    /* ssd1306_write_default(): SET_COLUMN_ADDRESS x..x+w-1, SET_PAGE_ADDRESS, horizontal addressing */
    uint16_t c0 = x, c1 = x + w - 1, p0 = y / 8, p1 = (y + h) / 8 - 1;
    uint16_t col = c0, page = p0;
    for (size_t i = 0; i < buf_len; i++) {
        if (page < PAGES && col < RAM_W) {
            ram[page * RAM_W + col] = buf[i];
        }
        if (++col > c1) {
            col = c0;
            if (++page > p1) {
                page = p0;
            }
        }
    }
#endif
}

#endif /* SIM_PANEL_LS0XX */

/* ---------------- lvgl_display_mono.c ---------------- */

static void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;

    controller_write(area->x1, area->y1, w, h, (const uint8_t *)color_p, (w * h) / 8U);
    flush_count++;
    lv_disp_flush_ready(disp_drv);
}

static void set_px_cb(lv_disp_drv_t *disp_drv, uint8_t *buf, lv_coord_t buf_w, lv_coord_t x,
                      lv_coord_t y, lv_color_t color, lv_opa_t opa) {
#if SIM_PANEL_LS0XX
    /* horizontal tiling, LSB first; PIXEL_FORMAT_MONO01 */
    uint8_t *buf_xy = buf + x / 8 + y * buf_w / 8;
    uint8_t bit = x % 8;

    if (color.full == 0) {
        *buf_xy |= BIT(bit);
    } else {
        *buf_xy &= ~BIT(bit);
    }
#else
    /* SCREEN_INFO_MONO_VTILED, LSB first; PIXEL_FORMAT_MONO10 */
    uint8_t *buf_xy = buf + x + y / 8 * buf_w;
    uint8_t bit = y % 8;

    if (color.full == 0) {
        *buf_xy &= ~BIT(bit);
    } else {
        *buf_xy |= BIT(bit);
    }
#endif
}

static void rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
#if SIM_PANEL_LS0XX
    /* SCREEN_INFO_X_ALIGNMENT_WIDTH */
    area->x1 = 0;
    area->x2 = PANEL_W - 1;
#else
    area->y1 &= ~0x7;
    area->y2 |= 0x7;
#endif
}

/* ---------------- lvgl.c rendering buffers ---------------- */

#define GUARD_SIZE 1024
#define GUARD_BYTE 0xa5

static lv_disp_drv_t disp_drv;

#ifdef CONFIG_LV_Z_BUFFER_ALLOC_STATIC

#define BUFFER_SIZE                                                                                \
    (CONFIG_LV_Z_BITS_PER_PIXEL * ((CONFIG_LV_Z_VDB_SIZE * PANEL_W * PANEL_H) / 100) / 8)
#define NBR_PIXELS_IN_BUFFER (BUFFER_SIZE * 8 / CONFIG_LV_Z_BITS_PER_PIXEL)

static lv_disp_draw_buf_t disp_buf;
/* A guard after the buffer shows whether LVGL ever writes past what the firmware allocates. */
static struct {
    uint8_t buf[BUFFER_SIZE];
    uint8_t guard[GUARD_SIZE];
} vdb __aligned(CONFIG_LV_Z_VDB_ALIGN);

static int allocate_rendering_buffers(lv_disp_drv_t *drv) {
    drv->hor_res = PANEL_W;
    drv->ver_res = PANEL_H;
    memset(vdb.guard, GUARD_BYTE, GUARD_SIZE);
    drv->draw_buf = &disp_buf;
    lv_disp_draw_buf_init(drv->draw_buf, vdb.buf, NULL, NBR_PIXELS_IN_BUFFER);
    return 0;
}

static uint8_t *guard_ptr(void) { return vdb.guard; }

#else

static uint8_t *dyn_guard;

static int allocate_rendering_buffers(lv_disp_drv_t *drv) {
    uint16_t buf_nbr_pixels;
    uint32_t buf_size;

    drv->hor_res = PANEL_W;
    drv->ver_res = PANEL_H;
    buf_nbr_pixels = (CONFIG_LV_Z_VDB_SIZE * drv->hor_res * drv->ver_res) / 100;
    if (buf_nbr_pixels < drv->hor_res) {
        buf_nbr_pixels = drv->hor_res;
    }
    buf_size = buf_nbr_pixels / 8;
    buf_size += (buf_nbr_pixels % 8) == 0 ? 0 : 1;

    uint8_t *buf0 = lv_mem_alloc(buf_size + GUARD_SIZE);
    if (buf0 == NULL) {
        return -ENOMEM;
    }
    dyn_guard = buf0 + buf_size;
    memset(dyn_guard, GUARD_BYTE, GUARD_SIZE);
    drv->draw_buf = lv_mem_alloc(sizeof(lv_disp_draw_buf_t));
    lv_disp_draw_buf_init(drv->draw_buf, buf0, NULL, buf_nbr_pixels);
    return 0;
}

static uint8_t *guard_ptr(void) { return dyn_guard; }

#endif

static bool guard_intact(void) {
    uint8_t *g = guard_ptr();
    for (int i = 0; g && i < GUARD_SIZE; i++) {
        if (g[i] != GUARD_BYTE) {
            return false;
        }
    }
    return true;
}

/* ---------------- animation frame sets ---------------- */

#if SIM_ANIMATION_ASSETS
LV_IMG_DECLARE(crystal_01); LV_IMG_DECLARE(crystal_02); LV_IMG_DECLARE(crystal_03);
LV_IMG_DECLARE(crystal_04); LV_IMG_DECLARE(crystal_05); LV_IMG_DECLARE(crystal_06);
LV_IMG_DECLARE(crystal_07); LV_IMG_DECLARE(crystal_08); LV_IMG_DECLARE(crystal_09);
LV_IMG_DECLARE(crystal_10); LV_IMG_DECLARE(crystal_11); LV_IMG_DECLARE(crystal_12);
LV_IMG_DECLARE(crystal_13); LV_IMG_DECLARE(crystal_14); LV_IMG_DECLARE(crystal_15);
LV_IMG_DECLARE(crystal_16);
static const void *set_crystal[] = {
    &crystal_01, &crystal_02, &crystal_03, &crystal_04, &crystal_05, &crystal_06,
    &crystal_07, &crystal_08, &crystal_09, &crystal_10, &crystal_11, &crystal_12,
    &crystal_13, &crystal_14, &crystal_15, &crystal_16};

LV_IMG_DECLARE(cat_0); LV_IMG_DECLARE(cat_1); LV_IMG_DECLARE(cat_2); LV_IMG_DECLARE(cat_3);
LV_IMG_DECLARE(cat_4); LV_IMG_DECLARE(cat_5); LV_IMG_DECLARE(cat_6); LV_IMG_DECLARE(cat_7);
static const void *set_cat[] = {&cat_0, &cat_1, &cat_2, &cat_3, &cat_4, &cat_5, &cat_6, &cat_7};

LV_IMG_DECLARE(head_00); LV_IMG_DECLARE(head_01); LV_IMG_DECLARE(head_02);
LV_IMG_DECLARE(head_03); LV_IMG_DECLARE(head_04); LV_IMG_DECLARE(head_05);
LV_IMG_DECLARE(head_06); LV_IMG_DECLARE(head_07); LV_IMG_DECLARE(head_08);
LV_IMG_DECLARE(head_09); LV_IMG_DECLARE(head_10); LV_IMG_DECLARE(head_11);
LV_IMG_DECLARE(head_12); LV_IMG_DECLARE(head_13); LV_IMG_DECLARE(head_14);
LV_IMG_DECLARE(head_15);
static const void *set_head[] = {&head_00, &head_01, &head_02, &head_03, &head_04, &head_05,
                                 &head_06, &head_07, &head_08, &head_09, &head_10, &head_11,
                                 &head_12, &head_13, &head_14, &head_15};

LV_IMG_DECLARE(spaceman_00); LV_IMG_DECLARE(spaceman_01); LV_IMG_DECLARE(spaceman_02);
LV_IMG_DECLARE(spaceman_03); LV_IMG_DECLARE(spaceman_04); LV_IMG_DECLARE(spaceman_05);
LV_IMG_DECLARE(spaceman_06); LV_IMG_DECLARE(spaceman_07); LV_IMG_DECLARE(spaceman_08);
LV_IMG_DECLARE(spaceman_09); LV_IMG_DECLARE(spaceman_10); LV_IMG_DECLARE(spaceman_11);
LV_IMG_DECLARE(spaceman_12); LV_IMG_DECLARE(spaceman_13); LV_IMG_DECLARE(spaceman_14);
LV_IMG_DECLARE(spaceman_15); LV_IMG_DECLARE(spaceman_16); LV_IMG_DECLARE(spaceman_17);
LV_IMG_DECLARE(spaceman_18); LV_IMG_DECLARE(spaceman_19);
static const void *set_spaceman[] = {
    &spaceman_00, &spaceman_01, &spaceman_02, &spaceman_03, &spaceman_04,
    &spaceman_05, &spaceman_06, &spaceman_07, &spaceman_08, &spaceman_09,
    &spaceman_10, &spaceman_11, &spaceman_12, &spaceman_13, &spaceman_14,
    &spaceman_15, &spaceman_16, &spaceman_17, &spaceman_18, &spaceman_19};

#define P(n) &pokemon_##n
LV_IMG_DECLARE(pokemon_00); LV_IMG_DECLARE(pokemon_01); LV_IMG_DECLARE(pokemon_02);
LV_IMG_DECLARE(pokemon_03); LV_IMG_DECLARE(pokemon_04); LV_IMG_DECLARE(pokemon_05);
LV_IMG_DECLARE(pokemon_06); LV_IMG_DECLARE(pokemon_07); LV_IMG_DECLARE(pokemon_08);
LV_IMG_DECLARE(pokemon_09); LV_IMG_DECLARE(pokemon_10); LV_IMG_DECLARE(pokemon_11);
LV_IMG_DECLARE(pokemon_12); LV_IMG_DECLARE(pokemon_13); LV_IMG_DECLARE(pokemon_14);
LV_IMG_DECLARE(pokemon_15); LV_IMG_DECLARE(pokemon_16); LV_IMG_DECLARE(pokemon_17);
LV_IMG_DECLARE(pokemon_18); LV_IMG_DECLARE(pokemon_19); LV_IMG_DECLARE(pokemon_20);
LV_IMG_DECLARE(pokemon_21); LV_IMG_DECLARE(pokemon_22); LV_IMG_DECLARE(pokemon_23);
LV_IMG_DECLARE(pokemon_24); LV_IMG_DECLARE(pokemon_25); LV_IMG_DECLARE(pokemon_26);
LV_IMG_DECLARE(pokemon_27); LV_IMG_DECLARE(pokemon_28); LV_IMG_DECLARE(pokemon_29);
LV_IMG_DECLARE(pokemon_30); LV_IMG_DECLARE(pokemon_31); LV_IMG_DECLARE(pokemon_32);
LV_IMG_DECLARE(pokemon_33); LV_IMG_DECLARE(pokemon_34); LV_IMG_DECLARE(pokemon_35);
LV_IMG_DECLARE(pokemon_36); LV_IMG_DECLARE(pokemon_37); LV_IMG_DECLARE(pokemon_38);
LV_IMG_DECLARE(pokemon_39); LV_IMG_DECLARE(pokemon_40); LV_IMG_DECLARE(pokemon_41);
LV_IMG_DECLARE(pokemon_42); LV_IMG_DECLARE(pokemon_43); LV_IMG_DECLARE(pokemon_44);
LV_IMG_DECLARE(pokemon_45); LV_IMG_DECLARE(pokemon_46); LV_IMG_DECLARE(pokemon_47);
static const void *set_pokemon[] = {
    P(00), P(01), P(02), P(03), P(04), P(05), P(06), P(07), P(08), P(09), P(10), P(11),
    P(12), P(13), P(14), P(15), P(16), P(17), P(18), P(19), P(20), P(21), P(22), P(23),
    P(24), P(25), P(26), P(27), P(28), P(29), P(30), P(31), P(32), P(33), P(34), P(35),
    P(36), P(37), P(38), P(39), P(40), P(41), P(42), P(43), P(44), P(45), P(46), P(47)};
#undef P
#endif /* SIM_ANIMATION_ASSETS */

enum frame_set { SET_KCONFIG, SET_CRYSTAL, SET_CAT, SET_HEAD, SET_SPACEMAN, SET_POKEMON, SET_CUSTOM };

/* Your art: frames in nice!oled's LV_IMG_CF_INDEXED_1BIT layout, palette included. */
static lv_img_dsc_t *custom_dsc;
static const void **custom_src;
static uint8_t *custom_data;
static int custom_count;

static void free_custom(void) {
    if (custom_data) lv_mem_free(custom_data);
    if (custom_dsc) lv_mem_free(custom_dsc);
    if (custom_src) lv_mem_free(custom_src);
    custom_data = NULL;
    custom_dsc = NULL;
    custom_src = NULL;
    custom_count = 0;
}

/*
 * Reserve room for `count` frames of w x h pixels; the page writes the 1-bit pixel rows
 * (MSB first, set bit = foreground) for every frame back to back into the returned
 * buffer, then calls sim_commit_frames(). The palette follows
 * CONFIG_NICE_OLED_WIDGET_INVERTED, as in nice!oled's own assets.
 */
EXPORT(sim_alloc_frames) uint8_t *sim_alloc_frames(int count, int w, int h) {
    lv_init(); /* frames are staged before sim_init(); LVGL's heap must exist (repeat calls are no-ops) */
    free_custom();
    uint32_t stride = (w + 7) / 8, size = stride * h + 8;

    custom_data = lv_mem_alloc(size * count);
    custom_dsc = lv_mem_alloc(sizeof(lv_img_dsc_t) * count);
    custom_src = lv_mem_alloc(sizeof(void *) * count);
    if (!custom_data || !custom_dsc || !custom_src) {
        free_custom();
        return NULL;
    }
    custom_count = count;
    for (int i = 0; i < count; i++) {
        custom_dsc[i] = (lv_img_dsc_t){
            .header.cf = LV_IMG_CF_INDEXED_1BIT,
            .header.w = w,
            .header.h = h,
            .data_size = size,
            .data = custom_data + size * i,
        };
        custom_src[i] = &custom_dsc[i];
    }
    return custom_data;
}

/* Move each frame's staged pixel rows behind its own palette. */
EXPORT(sim_commit_frames) void sim_commit_frames(void) {
#if IS_ENABLED(CONFIG_NICE_OLED_WIDGET_INVERTED)
    static const uint8_t palette[8] = {0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff};
#else
    static const uint8_t palette[8] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff};
#endif
    if (!custom_count) {
        return;
    }
    uint32_t px = custom_dsc[0].data_size - 8;
    /* last frame first: every destination lies at or after its source */
    for (int i = custom_count - 1; i >= 0; i--) {
        memmove(custom_data + (px + 8) * i + 8, custom_data + px * i, px);
        memcpy(custom_data + (px + 8) * i, palette, 8);
    }
}

static int frames_for(int set, const void ***src) {
    switch (set) {
#if SIM_ANIMATION_ASSETS
    case SET_CRYSTAL: *src = set_crystal; return ARRAY_SIZE(set_crystal);
    case SET_CAT: *src = set_cat; return ARRAY_SIZE(set_cat);
    case SET_HEAD: *src = set_head; return ARRAY_SIZE(set_head);
    case SET_SPACEMAN: *src = set_spaceman; return ARRAY_SIZE(set_spaceman);
    case SET_POKEMON: *src = set_pokemon; return ARRAY_SIZE(set_pokemon);
#endif
    case SET_CUSTOM: *src = custom_src; return custom_count;
    default: return 0;
    }
}

static lv_obj_t *find_animimg(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_animimg_class)) {
        return obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        lv_obj_t *found = find_animimg(lv_obj_get_child(obj, i));
        if (found) {
            return found;
        }
    }
    return NULL;
}

/* Swap the frames an lv_animimg plays, the way a fork with different assets would. */
static void play_set(lv_obj_t *art, int set, int duration_ms) {
    const void **src;
    int n = frames_for(set, &src);
    if (!art || n <= 0) {
        return;
    }
    if (n > 255) {
        n = 255; /* lv_animimg_set_src() takes a uint8_t count */
    }
    lv_animimg_set_src(art, src, n);
    lv_animimg_set_duration(art, duration_ms);
    lv_animimg_set_repeat_count(art, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(art);
}

/* ---------------- boot ---------------- */

lv_obj_t *zmk_display_status_screen(void);
void __wasm_call_ctors(void);

enum screen_mode { MODE_STATUS_SCREEN, MODE_ART_ONLY };

/*
 * MODE_STATUS_SCREEN: the firmware's status screen as built from its Kconfig, with
 *   `set` (unless SET_KCONFIG) swapped into its lv_animimg.
 * MODE_ART_ONLY: a bare screen holding one lv_animimg at (x, y), what a minimal custom
 *   status screen showing only your art would draw.
 */
EXPORT(sim_init) int sim_init(int mode, int set, int x, int y, int duration_ms) {
    static bool ctors_done;
    if (!ctors_done) {
        __wasm_call_ctors(); /* ZMK_SUBSCRIPTION registrations */
        ctors_done = true;
    }
    lv_init();
    controller_init();

    lv_disp_drv_init(&disp_drv);
#ifdef CONFIG_LV_Z_FULL_REFRESH
    disp_drv.full_refresh = 1;
#endif
    if (allocate_rendering_buffers(&disp_drv) != 0) {
        return -1;
    }
    disp_drv.flush_cb = flush_cb;
    disp_drv.rounder_cb = rounder_cb;
    disp_drv.set_px_cb = set_px_cb;
    if (lv_disp_drv_register(&disp_drv) == NULL) {
        return -2;
    }
    sim_wpm_init();

    /* initialize_display() */
    initialized = true;
#if IS_ENABLED(CONFIG_LV_USE_THEME_MONO)
    lv_disp_t *disp = lv_disp_get_default();
    disp->theme = lv_theme_mono_init(disp, IS_ENABLED(CONFIG_ZMK_DISPLAY_INVERT), LV_FONT_DEFAULT);
#endif
    lv_obj_t *screen;
    if (mode == MODE_ART_ONLY) {
        screen = lv_obj_create(NULL);
        lv_obj_t *art = lv_animimg_create(screen);
        lv_obj_set_pos(art, x, y);
        play_set(art, set, duration_ms);
    } else {
        screen = zmk_display_status_screen();
        if (set != SET_KCONFIG) {
            play_set(find_animimg(screen), set, duration_ms);
        }
    }
    lv_scr_load(screen);
    return 0;
}

/* ZMK's timers: the display tick (work queue + lv_task_handler(), stopped while blanked)
 * every CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS, and the 1 s activity and WPM timers. */
EXPORT(sim_step) void sim_step(uint32_t ms) {
    static uint32_t pending, next_second = 1000;
    pending += ms;
    while (pending >= CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS) {
        pending -= CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS;
        sim_uptime_ms += CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS;
        while (sim_uptime_ms >= next_second) {
            next_second += 1000;
            sim_activity_work();
            sim_wpm_work();
        }
        if (zmk_activity_get_state() == ZMK_ACTIVITY_SLEEP) {
            continue; /* powered off */
        }
        sim_run_display_work();
        if (!sim_display_blanked) {
            lv_task_handler();
        }
    }
}

/* 0 flushes, 1 rejected flushes, 2 render buffer overrun (0/1), 3 LVGL heap bytes used,
 * 4 frames in animation, 5 panel off (0/1), 6 activity state, 7 ms since last activity,
 * 8 WPM */
EXPORT(sim_stat) int sim_stat(int which) {
    switch (which) {
    case 0: return flush_count;
    case 1: return flush_rejects;
    case 2: return !guard_intact();
    case 3: {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        return mon.total_size - mon.free_size;
    }
    case 4: {
        lv_obj_t *art = find_animimg(lv_scr_act());
        return art ? ((lv_animimg_t *)art)->pic_count : 0;
    }
    /* ls0xx has no blanking without disp-en-gpios (-ENOTSUP): blanked, it keeps the last image */
    case 5: return (sim_display_blanked && !SIM_PANEL_LS0XX) || zmk_activity_get_state() == ZMK_ACTIVITY_SLEEP;
    case 6: return zmk_activity_get_state();
    case 7: return sim_ms_since_activity();
    case 8: return zmk_wpm_get_state();
    default: return 0;
    }
}
