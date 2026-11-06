/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DLP_SHA256_H
#define DLP_SHA256_H

#include <stddef.h>
#include <stdint.h>

struct dlp_sha256_ctx {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t data[64];
	size_t datalen;
};

void dlp_sha256_init(struct dlp_sha256_ctx *ctx);
void dlp_sha256_update(struct dlp_sha256_ctx *ctx, const void *data, size_t length);
void dlp_sha256_final(struct dlp_sha256_ctx *ctx, uint8_t digest[32]);
int dlp_sha256_fd(int fd, uint8_t digest[32]);

#endif
