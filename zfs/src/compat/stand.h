/* Just enough of FreeBSD's libsa <stand.h> for the ZFS reader to build. */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

#define bcopy(s, d, n) memmove((d), (s), (n))
#define bzero(p, n) memset((p), 0, (n))
#define bcmp(a, b, n) memcmp((a), (b), (n))

#ifndef __packed
#define __packed __attribute__((packed))
#endif

typedef unsigned int uid_t;
typedef unsigned int gid_t;

#ifndef NBBY
#define NBBY 8
#endif

/* not in mingw's errno.h */
#ifndef ECKSUM
#define ECKSUM 1122
#endif

size_t strlcpy(char *dst, const char *src, size_t size);
void explicit_bzero(void *p, size_t n);
int iscntrl(int c);
