/*
 * A plain C interface to FreeBSD's boot-time ZFS reader (engine/). The EFI
 * driver talks to this, and nothing else in the driver sees the reader's own
 * headers, which clash with gnu-efi's.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

struct zfse_pool;
struct zfse_mount;
struct zfse_file;

typedef int (*zfse_read_fn)(void *priv, uint64_t offset, void *buf, size_t bytes);

struct zfse_stat {
    uint64_t size;
    uint32_t mode;
    uint64_t objnum;
    int is_dir;
};

struct zfse_dirent {
    char name[256];
    uint64_t objnum;
    uint64_t size;
    int is_dir;
};

/* All return 0 or an errno-style error. */
int zfse_init(void);
int zfse_probe(zfse_read_fn rd, uint64_t size, void* priv, struct zfse_pool** pool);
uint64_t zfse_pool_guid(struct zfse_pool* pool);
int zfse_mount(struct zfse_pool* pool, struct zfse_mount** mnt);
void zfse_unmount(struct zfse_mount* mnt);
int zfse_open(struct zfse_mount* mnt, const char* path, struct zfse_file** f);
void zfse_close(struct zfse_file* f);
int zfse_stat(struct zfse_file* f, struct zfse_stat* st);
int zfse_read(struct zfse_file* f, uint64_t offset, void* buf, size_t len, size_t* got);
int zfse_readdir(struct zfse_file* f, struct zfse_dirent* d); /* ENOENT at the end */
void zfse_rewind(struct zfse_file* f); /* start the directory again */
