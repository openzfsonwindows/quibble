/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of zfs-efi.
 *
 * zfs-efi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * zfs-efi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with zfs-efi.  If not, see <http://www.gnu.org/licenses/>. */

/*
 * The small amount of C library that the ZFS reader needs, as we have no libc
 * here: a heap on top of the UEFI pool allocator, string functions, and a
 * printf that goes to Quibble's info protocol. The memory and string functions
 * that already live in misc.c are not repeated.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "misc.h"

extern EFI_BOOT_SERVICES* bs;
void zfs_print(const char* s);

/* heap */

/* kept a multiple of 16 so that what we hand out is 16 byte aligned */
typedef struct {
    size_t size;
    size_t pad;
} heap_header;

void* malloc(size_t size) {
    heap_header* h;

    if (EFI_ERROR(bs->AllocatePool(EfiBootServicesData, size + sizeof(heap_header), (void**)&h)))
        return NULL;

    h->size = size;

    return h + 1;
}

void* calloc(size_t n, size_t size) {
    void* p;

    if (size != 0 && n > (size_t)-1 / size)
        return NULL;

    p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);

    return p;
}

void free(void* ptr) {
    if (ptr)
        bs->FreePool((heap_header*)ptr - 1);
}

void* realloc(void* ptr, size_t size) {
    void* p;
    size_t old;

    if (!ptr)
        return malloc(size);

    old = ((heap_header*)ptr - 1)->size;
    if (size <= old)
        return ptr;

    p = malloc(size);
    if (!p)
        return NULL;

    memcpy(p, ptr, old);
    free(ptr);

    return p;
}

/* memory and strings */

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;

    while (n >= sizeof(uint64_t)) {
        *(uint64_t*)d = *(const uint64_t*)s;

        d += sizeof(uint64_t);
        s += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }

    while (n > 0) {
        *d++ = *s++;
        n--;
    }

    return dest;
}

/* unlike misc.c's, which only copied forwards, this copes with overlap */
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = dest;
    const uint8_t* s = src;

    if (d == s || n == 0)
        return dest;

    if (d < s || d >= s + n) {
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    } else {
        d += n;
        s += n;

        while (n > 0) {
            *--d = *--s;
            n--;
        }
    }

    return dest;
}

void bcopy(const void* src, void* dest, size_t n) {
    memmove(dest, src, n);
}

void bzero(void* p, size_t n) {
    memset(p, 0, n);
}

void explicit_bzero(void* p, size_t n) {
    volatile uint8_t* v = p;

    while (n > 0) {
        *v++ = 0;
        n--;
    }
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

size_t strlcpy(char* dst, const char* src, size_t size) {
    size_t len = strlen(src);

    if (size > 0) {
        size_t n = len >= size ? size - 1 : len;

        memcpy(dst, src, n);
        dst[n] = 0;
    }

    return len;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;

    do {
        if (*s == (char)c)
            last = s;
    } while (*s++);

    return (char*)last;
}

char* strcat(char* dest, const char* src) {
    strcpy(dest + strlen(dest), src);

    return dest;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* p = malloc(len);

    if (p)
        memcpy(p, s, len);

    return p;
}

int iscntrl(int c) {
    return c < 0x20 || c == 0x7f;
}

/* printf */

typedef struct {
    char* buf;
    size_t size;
    size_t pos;
} out_state;

static void out_char(out_state* o, char c) {
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;

    o->pos++;
}

static void out_number(out_state* o, uint64_t v, unsigned int base, bool upper, bool neg, int width, bool zero, bool left) {
    char tmp[24];
    int n = 0, pad;

    do {
        unsigned int d = v % base;

        tmp[n++] = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
        v /= base;
    } while (v != 0);

    pad = width - n - (neg ? 1 : 0);

    if (!left && !zero) {
        while (pad-- > 0)
            out_char(o, ' ');
    }

    if (neg)
        out_char(o, '-');

    if (!left && zero) {
        while (pad-- > 0)
            out_char(o, '0');
    }

    while (n > 0)
        out_char(o, tmp[--n]);

    if (left) {
        while (pad-- > 0)
            out_char(o, ' ');
    }
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
    out_state o = { buf, size, 0 };

    while (*fmt) {
        bool left = false, zero = false;
        int width = 0, lng = 0;
        char c;

        if (*fmt != '%') {
            out_char(&o, *fmt++);
            continue;
        }

        fmt++;

        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                zero = true;
            else
                break;
        }

        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        /* h and hh are promoted to int anyway; l, ll, j, z and t are 64 bit */
        for (;; fmt++) {
            if (*fmt == 'h')
                lng = lng < 0 ? lng : -1;
            else if (*fmt == 'l' || *fmt == 'j' || *fmt == 'z' || *fmt == 't')
                lng = 1;
            else
                break;
        }

        c = *fmt;
        if (c == 0)
            break;

        fmt++;

        switch (c) {
            case 's': {
                const char* s = va_arg(ap, const char*);
                int len, pad;

                if (!s)
                    s = "(null)";

                len = (int)strlen(s);
                pad = width - len;

                while (!left && pad-- > 0)
                    out_char(&o, ' ');

                while (*s)
                    out_char(&o, *s++);

                while (left && pad-- > 0)
                    out_char(&o, ' ');

                break;
            }

            case 'c':
                out_char(&o, (char)va_arg(ap, int));
                break;

            case 'd':
            case 'i': {
                int64_t v = lng > 0 ? va_arg(ap, int64_t) : va_arg(ap, int);

                out_number(&o, v < 0 ? (uint64_t)-v : (uint64_t)v, 10, false, v < 0, width, zero, left);
                break;
            }

            case 'u':
            case 'x':
            case 'X': {
                uint64_t v = lng > 0 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);

                if (lng < 0 && c != 'u')
                    v &= 0xffff;

                out_number(&o, v, c == 'u' ? 10 : 16, c == 'X', false, width, zero, left);
                break;
            }

            case 'p':
                out_char(&o, '0');
                out_char(&o, 'x');
                out_number(&o, (uint64_t)(uintptr_t)va_arg(ap, void*), 16, false, false, 0, false, false);
                break;

            case '%':
                out_char(&o, '%');
                break;

            default:
                out_char(&o, '%');
                out_char(&o, c);
                break;
        }
    }

    if (size > 0)
        buf[o.pos < size ? o.pos : size - 1] = 0;

    return (int)o.pos;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return ret;
}

int printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    zfs_print(buf);

    return ret;
}

int asprintf(char** strp, const char* fmt, ...) {
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (len < 0)
        return -1;

    *strp = malloc(len + 1);
    if (!*strp)
        return -1;

    va_start(ap, fmt);
    vsnprintf(*strp, len + 1, fmt, ap);
    va_end(ap);

    return len;
}
