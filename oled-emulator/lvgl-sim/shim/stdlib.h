#pragma once
#include <stddef.h>

void *malloc(size_t n);
void *calloc(size_t count, size_t n);
void *realloc(void *p, size_t n);
void free(void *p);
int abs(int v);
long labs(long v);
int rand(void);
void srand(unsigned int seed);
int atoi(const char *s);
#define RAND_MAX 0x7fffffff
