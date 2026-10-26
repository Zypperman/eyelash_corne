/*
 * One Eyelash Corne half's OLED, running real LVGL and nice!oled code, driven
 * from the page through the exported sim_* functions.
 *
 * The display path is a port of what the firmware runs:
 *  - zephyr/modules/lvgl/lvgl.c and lvgl_display_mono.c (Zephyr v3.5.0+zmk-fixes):
 *    render buffer sizing and the mono set_px/rounder/flush callbacks, for the
 *    SSD1306's capabilities (PIXEL_FORMAT_MONO10, SCREEN_INFO_MONO_VTILED)
 *  - zephyr/drivers/display/ssd1306.c: horizontal-addressing writes into GDDRAM
 *  - zmk/app/src/display/main.c (ZMK v0.3.0): theme, status screen, and
 *    lv_task_handler() every CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS
 * The page reads GDDRAM and applies the panel's devicetree settings
 * (inversion-on, segment-remap, com-invdir) itself.
 */
#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/sim_zmk.h>

#include "sim_kcfg.h"

#define EXPORT(name) __attribute__((export_name(#name)))

#define PANEL_W 128
#define PANEL_H 32

uint32_t sim_uptime_ms;
int sim_kcfg[SIM_KCFG_COUNT] = SIM_KCFG_DEFAULTS;

EXPORT(sim_kcfg_ptr) int *sim_kcfg_ptr(void) { return sim_kcfg; }

/* ---------- ZMK state the widgets read ---------- */

static struct {
    uint8_t battery;
    bool usb;
    bool connected;
} state = {.battery = 80, .usb = false, .connected = true};

static bool initialized;

uint8_t zmk_battery_state_of_charge(void) { return state.battery; }
bool zmk_usb_is_powered(void) { return state.usb; }
bool zmk_split_bt_peripheral_is_connected(void) { return state.connected; }
int zmk_display_is_initialized(void) { return initialized; }

#define MAX_LISTENERS 16
static void (*listeners[MAX_LISTENERS])(void);
static int listener_count;

void sim_register_listener(void (*refresh)(void)) {
    if (listener_count < MAX_LISTENERS) {
        listeners[listener_count++] = refresh;
    }
}

/* Same effect as ZMK raising battery/USB/split events: every widget listener re-reads state. */
EXPORT(sim_set_state) void sim_set_state(int battery, int usb, int connected) {
    state.battery = battery;
    state.usb = usb;
    state.connected = connected;
    if (!initialized) {
        return;
    }
    for (int i = 0; i < listener_count; i++) {
        listeners[i]();
    }
}

/* ---------- zmk/app/src/activity.c, and display/main.c's blank-on-idle ---------- */

enum activity_state { ACTIVITY_ACTIVE, ACTIVITY_IDLE, ACTIVITY_SLEEP };

static enum activity_state activity_state;
static uint32_t activity_last_uptime;
static uint32_t activity_next_check = 1000; /* k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1)) */
static bool display_blanked;

static void set_activity(enum activity_state next) {
    if (activity_state == next) {
        return;
    }
    activity_state = next;
#if IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE)
    /* start_display_updates() / stop_display_updates(): the panel is switched off and
     * lv_task_handler() stops running until the next activity */
    display_blanked = next != ACTIVITY_ACTIVE;
#endif
}

/* A key on this half went down or up (zmk_position_state_changed): note_activity(). */
EXPORT(sim_key) void sim_key(int pressed) {
    if (activity_state == ACTIVITY_SLEEP) {
        return; /* sys_poweroff() happened; the page reboots the half */
    }
    activity_last_uptime = k_uptime_get_32();
    set_activity(ACTIVITY_ACTIVE);
}

static void activity_work_handler(void) {
    int32_t inactive_time = sim_uptime_ms - activity_last_uptime;
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (inactive_time > CONFIG_ZMK_IDLE_SLEEP_TIMEOUT && !state.usb) {
        set_activity(ACTIVITY_SLEEP); /* followed by sys_poweroff() */
        return;
    }
#endif
    if (inactive_time > CONFIG_ZMK_IDLE_TIMEOUT) {
        set_activity(ACTIVITY_IDLE);
    }
}

/* ---------- SSD1306 GDDRAM: 4 pages x 128 columns, bit 0 = top row of the page ---------- */

static uint8_t gddram[PANEL_H / 8 * PANEL_W];
static uint32_t flush_count, flush_rejects;

EXPORT(sim_gddram) uint8_t *sim_gddram(void) { return gddram; }

static void ssd1306_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *buf,
                          size_t buf_size) {
    size_t buf_len = MIN(buf_size, (size_t)h * w / 8);

    /* The driver rejects these (LOG_ERR + return -1); the region keeps its old contents. */
    if (buf_len == 0 || (y & 0x7) != 0) {
        flush_rejects++;
        return;
    }

    /* SET_COLUMN_ADDRESS x..x+w-1, SET_PAGE_ADDRESS y/8..(y+h)/8-1, horizontal addressing */
    uint16_t c0 = x, c1 = x + w - 1, p0 = y / 8, p1 = (y + h) / 8 - 1;
    uint16_t col = c0, page = p0;
    for (size_t i = 0; i < buf_len; i++) {
        if (page < PANEL_H / 8 && col < PANEL_W) {
            gddram[page * PANEL_W + col] = buf[i];
        }
        if (++col > c1) {
            col = c0;
            if (++page > p1) {
                page = p0;
            }
        }
    }
}

/* ---------- lvgl_display_mono.c ---------- */

static void flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint16_t w = area->x2 - area->x1 + 1;
    uint16_t h = area->y2 - area->y1 + 1;

    ssd1306_write(area->x1, area->y1, w, h, (const uint8_t *)color_p, (w * h) / 8U);
    flush_count++;
    lv_disp_flush_ready(disp_drv);
}

static void set_px_cb(lv_disp_drv_t *disp_drv, uint8_t *buf, lv_coord_t buf_w, lv_coord_t x,
                      lv_coord_t y, lv_color_t color, lv_opa_t opa) {
    uint8_t *buf_xy = buf + x + y / 8 * buf_w;
    uint8_t bit = y % 8;

    if (color.full == 0) {
        *buf_xy &= ~BIT(bit);
    } else {
        *buf_xy |= BIT(bit);
    }
}

static void rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
    area->y1 &= ~0x7;
    area->y2 |= 0x7;
}

/* ---------- lvgl.c rendering buffers ---------- */

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

/* ---------- animation frame sets ---------- */

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
 * Reserve room for `count` frames of w x h pixels; the page writes the 1-bit
 * pixel rows (MSB first, set bit = lit) into the returned buffer, frame after
 * frame, then calls sim_commit_frames(). The palette follows the build's
 * CONFIG_NICE_OLED_WIDGET_INVERTED, as in nice!oled's own assets.
 */
EXPORT(sim_alloc_frames) uint8_t *sim_alloc_frames(int count, int w, int h) {
    lv_init(); /* frames are staged before sim_init(); LVGL's heap must exist (repeat calls are no-ops) */
    free_custom();
    uint32_t stride = (w + 7) / 8, px = stride * h, size = px + 8;

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
    /* the page stages the pixel rows back to back from the start; commit spreads them out */
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
    case SET_CRYSTAL: *src = set_crystal; return ARRAY_SIZE(set_crystal);
    case SET_CAT: *src = set_cat; return ARRAY_SIZE(set_cat);
    case SET_HEAD: *src = set_head; return ARRAY_SIZE(set_head);
    case SET_SPACEMAN: *src = set_spaceman; return ARRAY_SIZE(set_spaceman);
    case SET_POKEMON: *src = set_pokemon; return ARRAY_SIZE(set_pokemon);
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

/* Swap the frames nice!oled's lv_animimg plays, the way a fork with different assets would. */
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

/* ---------- boot ---------- */

lv_obj_t *zmk_display_status_screen(void);

enum screen_mode { MODE_NICE_OLED, MODE_ART_ONLY };

/*
 * mode MODE_NICE_OLED: nice!oled's status screen as built from your Kconfig,
 *   with `set` (unless SET_KCONFIG) swapped into its animation slot.
 * mode MODE_ART_ONLY: a bare screen holding one lv_animimg at (x, y), what a
 *   minimal custom status screen showing only your art would draw.
 */
EXPORT(sim_init) int sim_init(int mode, int set, int x, int y, int duration_ms) {
    lv_init();

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

    initialized = true;

#if IS_ENABLED(CONFIG_LV_USE_THEME_MONO)
    lv_disp_t *disp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_mono_init(disp, IS_ENABLED(CONFIG_ZMK_DISPLAY_INVERT), LV_FONT_DEFAULT);
    disp->theme = theme;
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

/* ZMK's display timer (lv_task_handler() every CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS, stopped while
 * blanked) and the 1 s activity timer. */
EXPORT(sim_step) void sim_step(uint32_t ms) {
    static uint32_t pending;
    pending += ms;
    while (pending >= CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS) {
        pending -= CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS;
        sim_uptime_ms += CONFIG_ZMK_DISPLAY_TICK_PERIOD_MS;
        while (sim_uptime_ms >= activity_next_check) {
            activity_next_check += 1000;
            activity_work_handler();
        }
        if (!display_blanked && activity_state != ACTIVITY_SLEEP) {
            lv_task_handler();
        }
    }
}

/* 0 flushes, 1 rejected flushes, 2 render buffer overrun (0/1), 3 LVGL heap bytes used,
 * 4 frames in animation, 5 panel off (0/1), 6 activity state, 7 ms since last activity */
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
    case 5: return display_blanked || activity_state == ACTIVITY_SLEEP;
    case 6: return activity_state;
    case 7: return sim_uptime_ms - activity_last_uptime;
    default: return 0;
    }
}

