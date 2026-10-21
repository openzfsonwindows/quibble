/* mingw's stdio.h redirects printf to its own runtime. We have our own. */

#pragma once

#include <stdarg.h>
#include <stddef.h>

int printf(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
