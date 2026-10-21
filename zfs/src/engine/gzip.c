/*
 * FreeBSD's gzip.c needs zlib. Not wired in yet, so datasets compressed with
 * gzip can't be read. The ones we boot from use lz4 or zstd.
 */

static int
gzip_decompress(void *s_start, void *d_start, size_t s_len, size_t d_len, int n)
{
	return (-1);
}
