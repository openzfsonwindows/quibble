/*
 * The few things the ZFS reader takes from FreeBSD's loader libzfs.h. The
 * loader parts (devdesc, boot environments, the nvstore) are not wanted.
 */

#pragma once

#include <zfsimpl.h>
#include "nvlist.h"

#define ZFS_MAXNAMELEN 256

int zfs_get_bootenv(void *, nvlist_t **);
int zfs_set_bootenv(void *, nvlist_t *);
uint64_t ldi_get_size(void *);
nvlist_t *vdev_read_bootenv(vdev_t *);
