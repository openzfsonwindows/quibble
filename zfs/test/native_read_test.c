/*
 * Host test: open a raw pool image with zfsengine and look files up.
 * Build as in zap_case_test.c, but with this file and no engine/zfsimpl.c
 * (zfsengine.c includes it). Usage: native_read_test image offset size
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int open(const char *, int, ...);
extern long pread(int, void *, unsigned long, long);

#include "../src/zfsengine.c"

int
pager_output(const char* s)
{
    fputs(s, stdout);
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

static int fd;
static uint64_t base;

static int
rd(void* priv, uint64_t off, void* buf, size_t n)
{
    return pread(fd, buf, n, base + off) == (ssize_t)n ? 0 : 5;
}

static int fails;

static void
look(struct zfse_mount* m, const char* path, int want_ok)
{
    struct zfse_file* f;
    struct zfse_stat st;
    int rc = zfse_open(m, path, &f);

    if (rc == 0) {
        zfse_stat(f, &st);
        printf("%s  %-40s size %llu dir %d\n", want_ok ? "PASS" : "FAIL", path,
               (unsigned long long)st.size, st.is_dir);
        zfse_close(f);
    } else {
        printf("%s  %-40s rc %d\n", want_ok ? "FAIL" : "PASS", path, rc);
    }

    if ((rc == 0) != want_ok)
        fails++;
}

int
main(int argc, char** argv)
{
    struct zfse_pool* pool;
    struct zfse_mount* m;
    struct zfse_file* f;
    struct zfse_dirent d;
    char buf[64];
    size_t got;
    int rc, n = 0;

    fd = open(argv[1], 0);
    base = strtoull(argv[2], 0, 0);

    zfse_init();
    rc = zfse_probe(rd, strtoull(argv[3], 0, 0), NULL, &pool);
    printf("probe rc %d\n", rc);
    if (rc) return 2;
    rc = zfse_mount(pool, &m);
    printf("mount rc %d\n", rc);
    if (rc) return 2;

    look(m, "/hello.txt", 1);
    look(m, "/HELLO.TXT", 1);
    look(m, "/Hello.Txt", 1);
    look(m, "/Windows/System32/ntoskrnl.exe", 1);
    look(m, "/windows/system32/NTOSKRNL.EXE", 1);
    look(m, "/WINDOWS/SYSTEM32/CONFIG/system", 1);
    look(m, "/Windows/System32/File0001.Dll", 1);
    look(m, "/windows/system32/file0123.dll", 1);
    look(m, "/WINDOWS/SYSTEM32/FILE0400.DLL", 1);
    look(m, "/Windows/System32/File0401.Dll", 0);
    look(m, "/nothere.txt", 0);

    if (zfse_open(m, "/windows/system32", &f) == 0) {
        while (zfse_readdir(f, &d) == 0)
            n++;
        printf("%s  System32 entries: %d (want 402)\n", n == 402 ? "PASS" : "FAIL", n);
        if (n != 402) fails++;
        zfse_close(f);
    }

    if (zfse_open(m, "/hello.txt", &f) == 0) {
        memset(buf, 0, sizeof(buf));
        zfse_read(f, 0, buf, 9, &got);
        printf("hello.txt: '%.9s' (%zu)\n", buf, got);
        zfse_close(f);
    }

    if (zfse_open(m, "/Windows/System32/ntoskrnl.exe", &f) == 0) {
        zfse_read(f, 0, buf, 14, &got);
        printf("ntoskrnl.exe: '%.14s' (%zu)\n", buf, got);
        zfse_close(f);
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
