/*
 * Just enough libc for LVGL and nice!oled on wasm32 with no OS. Memory
 * functions lower to wasm bulk-memory instructions; snprintf uses LVGL's own
 * printf; malloc hands out LVGL heap memory (LV_MEM_CUSTOM is 0).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <lvgl.h>

void *memcpy(void *dst, const void *src, size_t n) {
    __builtin_memcpy(dst, src, n);
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    __builtin_memmove(dst, src, n);
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    __builtin_memset(dst, c, n);
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}
char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}
char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}
char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strstr(const char *s, const char *needle) {
    size_t n = strlen(needle);
    for (;; s++) {
        if (!strncmp(s, needle, n)) return (char *)s;
        if (!*s) return NULL;
    }
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
int atoi(const char *s) {
    int sign = 1, v = 0;
    if (*s == '-') sign = -1, s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

static uint32_t rand_state = 1;
void srand(unsigned int seed) { rand_state = seed ? seed : 1; }
int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void *malloc(size_t n) { return lv_mem_alloc(n); }
void *calloc(size_t count, size_t n) {
    void *p = lv_mem_alloc(count * n);
    if (p) memset(p, 0, count * n);
    return p;
}
void *realloc(void *p, size_t n) { return lv_mem_realloc(p, n); }
void free(void *p) { lv_mem_free(p); }

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return lv_vsnprintf(buf, n, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = lv_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = lv_vsnprintf(buf, 1 << 16, fmt, ap);
    va_end(ap);
    return r;
}
