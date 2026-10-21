#pragma once

#include <stdint.h>

#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _BYTE_ORDER _LITTLE_ENDIAN

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
