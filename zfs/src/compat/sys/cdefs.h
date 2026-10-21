#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define __predict_true(x) __builtin_expect(!!(x), 1)
#define __predict_false(x) __builtin_expect(!!(x), 0)
#define __FBSDID(x)
#define __BEGIN_DECLS
#define __END_DECLS
#define __containerof(x, s, m) ((s *)((char *)(x) - offsetof(s, m)))
