#pragma once
#include <stddef.h>
#include <stdarg.h>

int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
