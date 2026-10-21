#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) (((x) / (y)) * (y))
#define roundup2(x, y) (((x) + ((y) - 1)) & (~((y) - 1)))
#define rounddown2(x, y) ((x) & (~((y) - 1)))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
