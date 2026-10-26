/* Stand-in for the bits of Zephyr's kernel API that display code uses. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h> /* Zephyr's kernel.h pulls these in transitively; widgets rely on it */
#include <stdlib.h>

#define BIT(n) (1UL << (n))
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define CLAMP(val, low, high) (((val) <= (low)) ? (low) : MIN(val, high))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONTAINER_OF(ptr, type, field) ((type *)(((char *)(ptr)) - offsetof(type, field)))
#define __ASSERT(...) ((void)0)
#define __ASSERT_NO_MSG(...) ((void)0)
#define __aligned(x) __attribute__((aligned(x)))
#define __unused __attribute__((unused))
#define __packed __attribute__((packed))

/* sys/util_macro.h: IS_ENABLED() is 1 only for macros defined to 1 */
#define _XXXX1 _YYYY,
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(_XXXX##config_macro)
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...) val

/* sys/slist.h (the subset display code uses) */
typedef struct _snode {
    struct _snode *next;
} sys_snode_t;
typedef struct {
    sys_snode_t *head, *tail;
} sys_slist_t;
#define SYS_SLIST_STATIC_INIT(ptr) {NULL, NULL}
static inline void sys_slist_init(sys_slist_t *list) { list->head = list->tail = NULL; }
static inline void sys_slist_append(sys_slist_t *list, sys_snode_t *node) {
    node->next = NULL;
    if (list->tail) {
        list->tail->next = node;
    } else {
        list->head = node;
    }
    list->tail = node;
}
#define SYS_SLIST_FOR_EACH_CONTAINER(list, cn, n)                                                  \
    for (sys_snode_t *_sn = (list)->head;                                                          \
         _sn && ((cn) = CONTAINER_OF(_sn, __typeof__(*(cn)), n), 1); _sn = _sn->next)

/* time: driven by the simulator's clock */
extern uint32_t sim_uptime_ms;
static inline uint32_t k_uptime_get_32(void) { return sim_uptime_ms; }
static inline int64_t k_uptime_get(void) { return sim_uptime_ms; }
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
#define K_SECONDS(s) ((s) * 1000)

/* Single-threaded: mutexes are no-ops; work items run on the display work queue
 * (the dedicated queue ZMK's display code submits to) before the next LVGL tick. */
struct k_mutex {
    int unused;
};
#define K_MUTEX_DEFINE(name) struct k_mutex name
static inline int k_mutex_lock(struct k_mutex *m, int timeout) { return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { return 0; }

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work {
    k_work_handler_t handler;
};
struct k_work_q {
    int unused;
};
#define K_WORK_DEFINE(work, work_handler) struct k_work work = {.handler = work_handler}
void sim_work_submit(struct k_work *work);
static inline void k_work_init(struct k_work *work, k_work_handler_t handler) { work->handler = handler; }
static inline int k_work_submit(struct k_work *work) {
    sim_work_submit(work);
    return 0;
}
static inline int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    sim_work_submit(work);
    return 0;
}
