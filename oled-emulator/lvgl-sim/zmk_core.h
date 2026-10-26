/* State and hooks shared between zmk_core.c (the ZMK side) and sim.c (the page side). */
#pragma once
#include <zephyr/kernel.h>
#include <zmk/endpoints_types.h>
#include <zmk/usb.h>

#define SIM_MAX_PERIPHERALS 4

struct sim_zmk_state {
    uint8_t battery;
    enum zmk_usb_conn_state usb;
    bool split_connected;
    uint8_t peripheral_battery[SIM_MAX_PERIPHERALS];
    enum zmk_transport transport;
    int ble_profile;
    bool ble_connected;
    bool ble_open;
    uint8_t mods;
    uint8_t layer;
};

extern struct sim_zmk_state sim_zmk;
extern bool sim_display_blanked;

void sim_run_display_work(void);
void sim_activity_work(void);
uint32_t sim_ms_since_activity(void);
void sim_wpm_init(void);
void sim_wpm_work(void);
