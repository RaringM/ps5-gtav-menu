#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t state[8];
  uint64_t bits;
  uint8_t block[64];
  uint32_t used;
} GtavSha256;

void gtav_sha256_init(GtavSha256* sha);
void gtav_sha256_update(GtavSha256* sha, const uint8_t* data, size_t size);
void gtav_sha256_final(GtavSha256* sha, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif
