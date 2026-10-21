/*
 * Host test for the case-insensitive ZAP lookups added to engine/zfsimpl.c.
 * Builds a micro ZAP and a fat ZAP leaf by hand and looks names up in them.
 *
 * What this shows is the mechanics: the folding, the comparison, the length
 * checks. It cannot show that the hash matches what ZFS itself computes, as
 * the test writes the hash with the same function the reader uses. That needs
 * a pool made by ZFS.
 *
 * It is built for the machine it runs on, not for EFI, from zfs/src with the
 * system headers wherever there are any and the shims in src/compat for the
 * rest. Make a copy of src/compat without cdefs.h, stdio.h, assert.h and
 * netinet/in.h (glibc has its own of those), call it hostcompat, then:
 *
 *   gcc -std=gnu11 -O0 -g -fsanitize=address,undefined -fno-sanitize=bounds  *       -w -include hostcompat/prefix.h -I hostcompat -I src/engine -I src  *       -o zap_case_test test/zap_case_test.c src/engine/nvlist.c  *       src/engine/skein.c src/engine/skein_block.c src/engine/list.c  *       src/engine/blake3_stub.c src/engine/lz4.c
 */

#include <stand.h>
#include "libzfs.h"
#include "zfsimpl.c"

static int fails;

static void
check(const char* what, int got, int want)
{
    printf("%s  %s (got %d, want %d)\n", got == want ? "PASS" : "FAIL", what, got, want);
    if (got != want)
        fails++;
}

/* things the real driver provides, or never calls */
int
pager_output(const char* s)
{
    return 0;
}

uint64_t
ldi_get_size(void* priv)
{
    return 0;
}

size_t
strlcpy(char* dst, const char* src, size_t size)
{
    size_t len = strlen(src);

    if (size) {
        size_t n = len >= size ? size - 1 : len;
        memcpy(dst, src, n);
        dst[n] = 0;
    }

    return len;
}

/* ---- micro ZAP ---- */

static union {
    mzap_phys_t mz;
    char buf[1024];
} mu;

static void
mset(int i, const char* name, uint64_t value)
{
    strlcpy(mu.mz.mz_chunk[i].mze_name, name, MZAP_NAME_LEN);
    mu.mz.mz_chunk[i].mze_value = value;
}

static int
mlook(uint64_t normflags, const char* name, uint64_t want)
{
    uint64_t v = 0;
    int rc;

    mu.mz.mz_normflags = normflags;
    rc = mzap_lookup(&mu.mz, sizeof(mu.buf), name, &v);

    return rc == 0 ? (v == want ? 1 : -1) : 0;
}

/* ---- fat ZAP leaf ---- */

#define LEAF_BS 12 /* 4096 byte leaf */

static union {
    zap_leaf_phys_t phys;
    char buf[1 << LEAF_BS];
} lu;

static zap_leaf_t leaf;

/* chunk 0 is the entry, 1 holds the name, 2 the value */
static void
leaf_add(const char* name, uint64_t value, uint64_t salt, uint64_t normflags)
{
    char norm[ZAP_MAXNAMELEN];
    zap_leaf_chunk_t* e;
    zap_leaf_chunk_t* n;
    zap_leaf_chunk_t* v;
    uint64_t hash;
    uint16_t* hp;
    int i;

    memset(&lu, 0, sizeof(lu));
    leaf.l_bs = LEAF_BS;
    leaf.l_phys = &lu.phys;

    lu.phys.l_hdr.lh_prefix_len = 0;

    for (i = 0; i < ZAP_LEAF_HASH_NUMENTRIES(&leaf); i++)
        lu.phys.l_hash[i] = CHAIN_END;

    zap_normalize_name(normflags, name, norm, sizeof(norm));
    hash = zap_hash(salt, norm);

    e = &ZAP_LEAF_CHUNK(&leaf, 0);
    n = &ZAP_LEAF_CHUNK(&leaf, 1);
    v = &ZAP_LEAF_CHUNK(&leaf, 2);

    e->l_entry.le_type = ZAP_CHUNK_ENTRY;
    e->l_entry.le_next = CHAIN_END;
    e->l_entry.le_name_chunk = 1;
    e->l_entry.le_name_numints = strlen(name) + 1;
    e->l_entry.le_value_chunk = 2;
    e->l_entry.le_value_intlen = 8;
    e->l_entry.le_value_numints = 1;
    e->l_entry.le_hash = hash;

    n->l_array.la_type = ZAP_CHUNK_ARRAY;
    memcpy(n->l_array.la_array, name, strlen(name) + 1);
    n->l_array.la_next = CHAIN_END;

    v->l_array.la_type = ZAP_CHUNK_ARRAY;
    for (i = 0; i < 8; i++)
        v->l_array.la_array[i] = (value >> (8 * (7 - i))) & 0xff;
    v->l_array.la_next = CHAIN_END;

    hp = LEAF_HASH_ENTPTR(&leaf, hash);
    *hp = 0;
}

static int
llook(uint64_t salt, uint64_t normflags, const char* name, uint64_t want)
{
    char norm[ZAP_MAXNAMELEN];
    const char* k = name;
    uint64_t v = 0;
    uint64_t hash;
    int rc;

    /* what fzap_lookup() does before it calls zap_leaf_lookup() */
    if (normflags & ZAP_NORM_TOUPPER) {
        if (zap_normalize_name(normflags, name, norm, sizeof(norm)) != 0)
            return -2;
        k = norm;
    }

    hash = zap_hash(salt, k);
    rc = zap_leaf_lookup(&leaf, hash, k, normflags, 8, 1, &v);

    return rc == 0 ? (v == want ? 1 : -1) : 0;
}

int
main(void)
{
    const uint64_t salt = 0x123456789abcdefULL;
    char longname[ZAP_MAXNAMELEN + 20];

    zfs_init_crc();

    memset(&mu, 0, sizeof(mu));
    mu.mz.mz_block_type = ZBT_MICRO;
    mset(0, "System32", 42);
    mset(1, "Windows", 7);
    mset(2, "readme.TXT", 99);

    printf("--- micro ZAP, case insensitive (normflags 2)\n");
    check("exact case", mlook(2, "System32", 42), 1);
    check("lower case", mlook(2, "system32", 42), 1);
    check("upper case", mlook(2, "SYSTEM32", 42), 1);
    check("mixed case", mlook(2, "sYsTeM32", 42), 1);
    check("second entry, lower", mlook(2, "windows", 7), 1);
    check("stored in lower, asked in upper", mlook(2, "README.txt", 99), 1);
    check("absent", mlook(2, "System33", 0), 0);
    check("prefix of a name", mlook(2, "System3", 0), 0);
    check("longer than a name", mlook(2, "System320", 0), 0);
    /*
     * An empty name matches an unused slot (empty name, value 0), as it did
     * with the strcmp this replaced. zfs_lookup never asks for one.
     */
    check("empty matches only an unused slot", mlook(2, "", 0), 1);

    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    check("name too long", mlook(2, longname, 0), 0);

    printf("--- micro ZAP, case sensitive (normflags 0) must not change\n");
    check("exact case", mlook(0, "System32", 42), 1);
    check("lower case is not found", mlook(0, "system32", 0), 0);
    check("exact, second", mlook(0, "Windows", 7), 1);

    printf("--- fat ZAP leaf, case insensitive\n");
    leaf_add("System32", 1234, salt, ZAP_NORM_TOUPPER);
    check("exact case", llook(salt, ZAP_NORM_TOUPPER, "System32", 1234), 1);
    check("lower case", llook(salt, ZAP_NORM_TOUPPER, "system32", 1234), 1);
    check("upper case", llook(salt, ZAP_NORM_TOUPPER, "SYSTEM32", 1234), 1);
    check("absent", llook(salt, ZAP_NORM_TOUPPER, "System33", 0), 0);
    check("prefix of the stored name", llook(salt, ZAP_NORM_TOUPPER, "System3", 0), 0);
    check("longer than the stored name", llook(salt, ZAP_NORM_TOUPPER, "System322", 0), 0);

    printf("--- fat ZAP leaf, case sensitive\n");
    leaf_add("System32", 1234, salt, 0);
    check("exact case", llook(salt, 0, "System32", 1234), 1);
    check("lower case is not found", llook(salt, 0, "system32", 0), 0);

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);

    return fails != 0;
}
