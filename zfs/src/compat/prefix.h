/* Force-included in every engine source file. */

#pragma once

#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stddef.h>
#include <sys/param.h>
#include <sys/endian.h>

#undef __unused
#define __unused __attribute__((unused))

#ifndef __containerof
#define __containerof(x, s, m) ((s *)((char *)(x) - offsetof(s, m)))
#endif

#ifndef __DECONST
#define __DECONST(type, var) ((type)(uintptr_t)(const void *)(var))
#endif
