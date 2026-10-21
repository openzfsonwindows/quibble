/*
 * Wraps FreeBSD's boot-time ZFS reader (engine/zfsimpl.c) in the plain C
 * interface of zfsengine.h.
 *
 * The reader is included as a source file, as FreeBSD's own zfs.c does: its
 * workings are static functions. The directory reading below follows
 * zfs_readdir() in FreeBSD's stand/libsa/zfs/zfs.c.
 */

#include <stand.h>
#include "libzfs.h"
#include "zfsimpl.c"
#include "zfsengine.h"

struct zfse_dev {
    zfse_read_fn rd;
    uint64_t size;
    void* priv;
};

struct zfse_pool {
    spa_t* spa;
    bool inited;
    int init_rc;
    struct zfse_pool* next;
};

struct zfse_mount {
    struct zfsmount mnt;
    struct zfse_pool* pool;
};

struct zfse_file {
    struct zfse_mount* mount;
    dnode_phys_t dnode;
    uint64_t objnum;
    uint64_t size;
    uint32_t mode;
    int is_dir;

    /* directory iteration */
    off_t seekp;
    uint64_t zap_type;
    uint64_t num_leafs;
    zap_leaf_phys_t* zap_leaf;
};

static struct zfse_pool* pools;

uint64_t
ldi_get_size(void* priv)
{
    return ((struct zfse_dev*)priv)->size;
}

static int
zfse_vdev_read(vdev_t* vdev, void* priv, off_t offset, void* buf, size_t bytes)
{
    struct zfse_dev* d = priv;

    return d->rd(d->priv, (uint64_t)offset, buf, bytes);
}

static int
zfse_vdev_write(vdev_t* vdev, off_t offset, void* buf, size_t bytes)
{
    return EROFS;
}

int
zfse_init(void)
{
    zfs_init();

    if (!dnode_cache_buf)
        return ENOMEM;

    return 0;
}

int
zfse_probe(zfse_read_fn rd, uint64_t size, void* priv, struct zfse_pool** out)
{
    struct zfse_dev* d;
    struct zfse_pool* p;
    spa_t* spa = NULL;
    int rc;

    d = calloc(1, sizeof(*d));
    if (!d)
        return ENOMEM;

    d->rd = rd;
    d->size = size;
    d->priv = priv;

    /* the reader keeps d for the life of the vdev */
    rc = vdev_probe(zfse_vdev_read, zfse_vdev_write, d, &spa);
    if (rc != 0 || !spa) {
        free(d);
        return rc ? rc : EIO;
    }

    for (p = pools; p; p = p->next) {
        if (p->spa == spa)
            break;
    }

    if (!p) {
        p = calloc(1, sizeof(*p));
        if (!p)
            return ENOMEM;

        p->spa = spa;
        p->next = pools;
        pools = p;
    }

    if (!p->inited) {
        p->init_rc = zfs_spa_init(spa);
        p->inited = true;
    }

    if (p->init_rc)
        return p->init_rc;

    *out = p;

    return 0;
}

uint64_t
zfse_pool_guid(struct zfse_pool* pool)
{
    return pool->spa->spa_guid;
}

int
zfse_mount(struct zfse_pool* pool, struct zfse_mount** out)
{
    struct zfse_mount* m;
    int rc;

    m = calloc(1, sizeof(*m));
    if (!m)
        return ENOMEM;

    m->pool = pool;

    /* a root of 0 means the pool bootfs, or failing that its root dataset */
    rc = zfs_mount_impl(pool->spa, 0, &m->mnt);
    if (rc == 0 && m->mnt.objset.os_type != DMU_OST_ZFS) {
        printf("ZFS: unexpected object set type %ju\n", (uintmax_t)m->mnt.objset.os_type);
        rc = EIO;
    }

    if (rc != 0) {
        free(m);
        return rc;
    }

    *out = m;

    return 0;
}

void
zfse_unmount(struct zfse_mount* mnt)
{
    free(mnt);
}

int
zfse_open(struct zfse_mount* mnt, const char* path, struct zfse_file** out)
{
    struct zfse_file* f;
    struct stat sb;
    int rc;

    f = calloc(1, sizeof(*f));
    if (!f)
        return ENOMEM;

    f->mount = mnt;

    rc = zfs_lookup(&mnt->mnt, path, &f->dnode, &f->objnum);
    if (rc == 0)
        rc = zfs_dnode_stat(&mnt->mnt, &f->dnode, &sb, mnt->mnt.fsid_guid, f->objnum);

    if (rc != 0) {
        free(f);
        return rc;
    }

    f->size = sb.st_size;
    f->mode = sb.st_mode;
    f->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;

    *out = f;

    return 0;
}

void
zfse_close(struct zfse_file* f)
{
    /*
     * The reader caches a block against the address of the dnode it was read
     * for, so the cache must not outlive this struct.
     */
    dnode_cache_obj = NULL;

    free(f->zap_leaf);
    free(f);
}

int
zfse_stat(struct zfse_file* f, struct zfse_stat* st)
{
    st->size = f->size;
    st->mode = f->mode;
    st->objnum = f->objnum;
    st->is_dir = f->is_dir;

    return 0;
}

int
zfse_read(struct zfse_file* f, uint64_t offset, void* buf, size_t len, size_t* got)
{
    size_t n = len;
    int rc;

    if (f->is_dir)
        return EISDIR;

    if (offset >= f->size) {
        *got = 0;
        return 0;
    }

    if (n > f->size - offset)
        n = f->size - offset;

    rc = dnode_read(f->mount->pool->spa, &f->dnode, offset, buf, n);
    if (rc != 0)
        return rc;

    *got = n;

    return 0;
}

void
zfse_rewind(struct zfse_file* f)
{
    free(f->zap_leaf);
    f->zap_leaf = NULL;
    f->seekp = 0;
    f->zap_type = 0;
    f->num_leafs = 0;
}

int
zfse_readdir(struct zfse_file* fp, struct zfse_dirent* d)
{
    const spa_t* spa = fp->mount->pool->spa;
    struct zfsmount* mnt = &fp->mount->mnt;
    size_t bsize = fp->dnode.dn_datablkszsec << SPA_MINBLOCKSHIFT;
    char name[256];
    uint64_t fileno;
    dnode_phys_t cdn;
    struct stat sb;
    int rc;

    if (!fp->is_dir)
        return ENOTDIR;

    /* first time: find out what sort of ZAP this is */
    if (fp->seekp == 0) {
        rc = dnode_read(spa, &fp->dnode, 0, &fp->zap_type, sizeof(fp->zap_type));
        if (rc)
            return rc;

        if (fp->zap_type == ZBT_MICRO) {
            fp->seekp = offsetof(mzap_phys_t, mz_chunk);
        } else {
            rc = dnode_read(spa, &fp->dnode, offsetof(zap_phys_t, zap_num_leafs), &fp->num_leafs, sizeof(fp->num_leafs));
            if (rc)
                return rc;

            fp->seekp = bsize;
            fp->zap_leaf = malloc(bsize);
            if (!fp->zap_leaf)
                return ENOMEM;

            rc = dnode_read(spa, &fp->dnode, fp->seekp, fp->zap_leaf, bsize);
            if (rc)
                return rc;
        }
    }

    if (fp->zap_type == ZBT_MICRO) {
        mzap_ent_phys_t mze;

        do {
            if (fp->seekp >= (off_t)bsize)
                return ENOENT;

            rc = dnode_read(spa, &fp->dnode, fp->seekp, &mze, sizeof(mze));
            if (rc)
                return rc;

            fp->seekp += sizeof(mze);
        } while (!mze.mze_name[0]);

        strlcpy(name, mze.mze_name, sizeof(name));
        fileno = ZFS_DIRENT_OBJ(mze.mze_value);
    } else {
        zap_leaf_t zl;
        zap_leaf_chunk_t *zc, *nc;
        size_t namelen;
        char* p;

        /* so that we can use the ZAP size calculating macros */
        zl.l_bs = ilog2(bsize);
        zl.l_phys = fp->zap_leaf;

        /*
         * Work out which chunk we are looking at, and move on to the next
         * leaf when this one is used up. The low bits of seekp are the
         * chunk index.
         */
        do {
            int chunk = fp->seekp & (bsize - 1);

            if (chunk == ZAP_LEAF_NUMCHUNKS(&zl)) {
                fp->seekp = rounddown2(fp->seekp, bsize) + bsize;
                chunk = 0;

                if ((uint64_t)fp->seekp >= bsize * fp->num_leafs)
                    return ENOENT;

                rc = dnode_read(spa, &fp->dnode, fp->seekp, fp->zap_leaf, bsize);
                if (rc)
                    return rc;
            }

            zc = &ZAP_LEAF_CHUNK(&zl, chunk);
            fp->seekp++;
        } while (zc->l_entry.le_type != ZAP_CHUNK_ENTRY);

        namelen = zc->l_entry.le_name_numints;
        if (namelen > sizeof(name) - 1)
            namelen = sizeof(name) - 1;

        /* the name is spread over a chain of array chunks */
        nc = &ZAP_LEAF_CHUNK(&zl, zc->l_entry.le_name_chunk);
        p = name;
        while (namelen > 0) {
            size_t len = namelen;

            if (len > ZAP_LEAF_ARRAY_BYTES)
                len = ZAP_LEAF_ARRAY_BYTES;

            memcpy(p, nc->l_array.la_array, len);
            p += len;
            namelen -= len;
            nc = &ZAP_LEAF_CHUNK(&zl, nc->l_array.la_next);
        }
        *p = 0;

        /* the first eight bytes of the value are a uint64_t */
        fileno = ZFS_DIRENT_OBJ(fzap_leaf_value(&zl, zc));
    }

    memset(d, 0, sizeof(*d));
    strlcpy(d->name, name, sizeof(d->name));
    d->objnum = fileno;

    if (objset_get_dnode(spa, &mnt->objset, fileno, &cdn) == 0 &&
        zfs_dnode_stat(mnt, &cdn, &sb, mnt->fsid_guid, fileno) == 0) {
        d->size = sb.st_size;
        d->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;
    }

    return 0;
}
