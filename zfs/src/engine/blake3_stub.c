/*
 * The blake3 checksum is not implemented yet. These stubs produce a hash that
 * will not match, so a block checksummed with blake3 reads as corrupt instead
 * of being silently accepted.
 */

#include <string.h>
#include <sys/blake3.h>

void
Blake3_Init(BLAKE3_CTX *ctx)
{
}

void
Blake3_InitKeyed(BLAKE3_CTX *ctx, const uint8_t key[BLAKE3_KEY_LEN])
{
}

void
Blake3_Update(BLAKE3_CTX *ctx, const void *input, size_t input_len)
{
}

void
Blake3_Final(const BLAKE3_CTX *ctx, uint8_t *out)
{
	memset(out, 0xa5, BLAKE3_OUT_LEN);
}

void
Blake3_FinalSeek(const BLAKE3_CTX *ctx, uint64_t seek, uint8_t *out,
    size_t out_len)
{
	memset(out, 0xa5, out_len);
}
