#pragma once

#include <stdint.h>

#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _BYTE_ORDER _LITTLE_ENDIAN

/*
 * lz4.c and skein_port.h test BYTE_ORDER == BIG_ENDIAN. With none of these
 * defined that is 0 == 0, which reads as true, and they turn into the big
 * endian versions. glibc's headers hide the problem on the host.
 */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER _BYTE_ORDER
#endif

#define bswap16(x) __builtin_bswap16(x)
#define bswap32(x) __builtin_bswap32(x)
#define bswap64(x) __builtin_bswap64(x)

#define be16toh(x) __builtin_bswap16(x)
#define be32toh(x) __builtin_bswap32(x)
#define be64toh(x) __builtin_bswap64(x)
#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)

static inline uint64_t le64dec(const void *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
static inline void le64enc(void *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
static inline uint32_t le32dec(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline void le32enc(void *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
static inline uint32_t be32dec(const void *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t be64dec(const void *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void be64enc(void *p, uint64_t v) { v = __builtin_bswap64(v); __builtin_memcpy(p, &v, 8); }
