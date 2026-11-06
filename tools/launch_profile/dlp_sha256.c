/* SPDX-License-Identifier: Apache-2.0 */
#include "dlp_sha256.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t value, uint32_t shift)
{
	return (value >> shift) | (value << (32 - shift));
}

static void sha256_transform(struct dlp_sha256_ctx *ctx, const uint8_t data[64])
{
	uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];

	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)data[i * 4] << 24) |
			((uint32_t)data[i * 4 + 1] << 16) |
			((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = (rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18)) |
			(w[i - 15] >> 3);
		uint32_t s1 = (rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19)) |
			(w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];
	f = ctx->state[5];
	g = ctx->state[6];
	h = ctx->state[7];
	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);

		t1 = h + s1 + ch + sha256_k[i] + w[i];
		t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void dlp_sha256_init(struct dlp_sha256_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
}

void dlp_sha256_update(struct dlp_sha256_ctx *ctx, const void *data, size_t length)
{
	const uint8_t *input = data;

	for (size_t i = 0; i < length; i++) {
		ctx->data[ctx->datalen++] = input[i];
		if (ctx->datalen == 64) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

void dlp_sha256_final(struct dlp_sha256_ctx *ctx, uint8_t digest[32])
{
	size_t i = ctx->datalen;

	ctx->bitlen += (uint64_t)ctx->datalen * 8;
	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < 64)
			ctx->data[i++] = 0;
		sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56)
		ctx->data[i++] = 0;
	for (int j = 7; j >= 0; j--)
		ctx->data[i++] = (uint8_t)(ctx->bitlen >> (j * 8));
	sha256_transform(ctx, ctx->data);
	for (i = 0; i < 8; i++) {
		digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4 + 3] = (uint8_t)ctx->state[i];
	}
}

int dlp_sha256_fd(int fd, uint8_t digest[32])
{
	struct dlp_sha256_ctx ctx;
	uint8_t buffer[64 * 1024];
	off_t offset = 0;

	dlp_sha256_init(&ctx);
	for (;;) {
		ssize_t read_size = pread(fd, buffer, sizeof(buffer), offset);

		if (read_size == 0)
			break;
		if (read_size < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		dlp_sha256_update(&ctx, buffer, (size_t)read_size);
		if ((uint64_t)offset > INT64_MAX - (uint64_t)read_size) {
			errno = EOVERFLOW;
			return -1;
		}
		offset += read_size;
	}
	dlp_sha256_final(&ctx, digest);
	return 0;
}
