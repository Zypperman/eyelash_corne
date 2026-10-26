/* Stand-in for the bits of Zephyr's kernel API that nice!oled widgets use. */
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
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONTAINER_OF(ptr, type, field) ((type *)(((char *)(ptr)) - offsetof(type, field)))
#define __ASSERT(...) ((void)0)
#define __ASSERT_NO_MSG(...) ((void)0)
#define __aligned(x) __attribute__((aligned(x)))
#define __unused __attribute__((unused))

/* sys/util_macro.h: IS_ENABLED() is 1 only for macros defined to 1 */
#define _XXXX1 _YYYY,
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(_XXXX##config_macro)
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...) val

/* sys/slist.h (the subset the widgets use) */
typedef struct _snode { struct _snode *next; } sys_snode_t;
typedef struct { sys_snode_t *head, *tail; } sys_slist_t;
#define SYS_SLIST_STATIC_INIT(ptr) {NULL, NULL}
static inline void sys_slist_append(sys_slist_t *list, sys_snode_t *node) {
    node->next = NULL;
    if (list->tail) list->tail->next = node; else list->head = node;
    list->tail = node;
}
#define SYS_SLIST_FOR_EACH_CONTAINER(list, cn, n)                                                  \
    for (sys_snode_t *_sn = (list)->head;                                                          \
         _sn && ((cn) = CONTAINER_OF(_sn, __typeof__(*(cn)), n), 1); _sn = _sn->next)

/* time: driven by the simulator's clock */
extern uint32_t sim_uptime_ms;
static inline uint32_t k_uptime_get_32(void) { return sim_uptime_ms; }
static inline int64_t k_uptime_get(void) { return sim_uptime_ms; }

/* sync/work primitives collapse to no-ops: the sim is single-threaded */
struct k_mutex { int unused; };
struct k_work { int unused; };
#define K_MUTEX_DEFINE(name) static struct k_mutex name __unused
#define K_WORK_DEFINE(name, handler) static struct k_work name __unused
#define K_FOREVER 0
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
static inline int k_mutex_lock(struct k_mutex *m, int t) { return 0; }
static inline int k_mutex_unlock(struct k_mutex *m) { return 0; }
