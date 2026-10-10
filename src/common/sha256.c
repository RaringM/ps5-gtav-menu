#include "gtavmenu/sha256.h"

#include <string.h>

static uint32_t rotate_right(uint32_t value, uint32_t shift) {
  return (value >> shift) | (value << (32u - shift));
}

static void transform(GtavSha256* sha, const uint8_t block[64]) {
  static const uint32_t constants[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
      0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
      0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
      0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
      0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
      0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
      0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
      0xc67178f2u};
  uint32_t words[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t i;

  for (i = 0; i < 16; ++i) {
    words[i] = ((uint32_t)block[i * 4u] << 24) | ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) | block[i * 4u + 3u];
  }
  for (; i < 64; ++i) {
    uint32_t s0 =
        rotate_right(words[i - 15u], 7) ^ rotate_right(words[i - 15u], 18) ^ (words[i - 15u] >> 3);
    uint32_t s1 =
        rotate_right(words[i - 2u], 17) ^ rotate_right(words[i - 2u], 19) ^ (words[i - 2u] >> 10);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }
  a = sha->state[0];
  b = sha->state[1];
  c = sha->state[2];
  d = sha->state[3];
  e = sha->state[4];
  f = sha->state[5];
  g = sha->state[6];
  h = sha->state[7];
  for (i = 0; i < 64; ++i) {
    uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + choose + constants[i] + words[i];
    uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  sha->state[0] += a;
  sha->state[1] += b;
  sha->state[2] += c;
  sha->state[3] += d;
  sha->state[4] += e;
  sha->state[5] += f;
  sha->state[6] += g;
  sha->state[7] += h;
}

void gtav_sha256_init(GtavSha256* sha) {
  static const uint32_t initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  memcpy(sha->state, initial, sizeof(initial));
  sha->bits = 0;
  sha->used = 0;
}

void gtav_sha256_update(GtavSha256* sha, const uint8_t* data, size_t size) {
  while (size) {
    size_t take = 64u - sha->used;
    if (take > size) take = size;
    memcpy(sha->block + sha->used, data, take);
    sha->used += (uint32_t)take;
    sha->bits += (uint64_t)take * 8u;
    data += take;
    size -= take;
    if (sha->used == 64u) {
      transform(sha, sha->block);
      sha->used = 0;
    }
  }
}

void gtav_sha256_final(GtavSha256* sha, uint8_t digest[32]) {
  uint32_t i;

  sha->block[sha->used++] = 0x80;
  if (sha->used > 56u) {
    memset(sha->block + sha->used, 0, 64u - sha->used);
    transform(sha, sha->block);
    sha->used = 0;
  }
  memset(sha->block + sha->used, 0, 56u - sha->used);
  for (i = 0; i < 8; ++i) sha->block[63u - i] = (uint8_t)(sha->bits >> (i * 8u));
  transform(sha, sha->block);
  for (i = 0; i < 8; ++i) {
    digest[i * 4u] = (uint8_t)(sha->state[i] >> 24);
    digest[i * 4u + 1u] = (uint8_t)(sha->state[i] >> 16);
    digest[i * 4u + 2u] = (uint8_t)(sha->state[i] >> 8);
    digest[i * 4u + 3u] = (uint8_t)sha->state[i];
  }
}
