/*
 * Stands in for zephyr/modules/lvgl/include/lv_conf.h. Like Zephyr's, it only
 * sets the platform hooks; every rendering option comes from the CONFIG_LV_*
 * symbols in the Kconfig-generated autoconf.h, which lv_conf_internal.h reads.
 *
 * The hooks differ from firmware only where the sim has no OS: LVGL's own heap
 * instead of Zephyr's sys_heap, LVGL's printf instead of the C library's, and
 * a tick driven by the page instead of k_uptime_get_32().
 */
#ifndef SIM_LV_CONF_H
#define SIM_LV_CONF_H

#define LV_MEMCPY_MEMSET_STD 1
#define LV_MEM_CUSTOM        0
#define LV_MEM_SIZE          (4U * 1024U * 1024U)

#define LV_TICK_CUSTOM               1
#define LV_TICK_CUSTOM_INCLUDE       <zephyr/kernel.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (k_uptime_get_32())

#define LV_SPRINTF_CUSTOM 0

#define LV_CONF_SUPPRESS_DEFINE_CHECK 1

#endif /* SIM_LV_CONF_H */
