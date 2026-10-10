#include "gtavmenu/aes256.h"

#include <string.h>

// FIPS-197 section 5.1.1 S-box and section 5.3.2 inverse S-box.
static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

// Round constants for the 7 AES-256 key-expansion steps that use them (FIPS-197 section 5.2).
static const uint8_t kRcon[7] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40};

static uint8_t xtime(uint8_t value) {
  return (uint8_t)((uint8_t)(value << 1) ^ (uint8_t)(0x1bu * (uint8_t)(value >> 7)));
}

// GF(2^8) multiply with a fixed 8-step loop (no data-dependent branch).
static uint8_t gf_mul(uint8_t value, uint8_t factor) {
  uint8_t product = 0;
  uint32_t bit;
  for (bit = 0; bit < 8u; ++bit) {
    product ^= (uint8_t)(value & (uint8_t)(0u - (uint32_t)((factor >> bit) & 1u)));
    value = xtime(value);
  }
  return product;
}

static void add_round_key(uint8_t state[16], const uint8_t* round_key) {
  uint32_t i;
  for (i = 0; i < 16u; ++i) state[i] ^= round_key[i];
}

// The state is FIPS-197 column major: byte (row r, column c) is state[c * 4 + r].
static void sub_shift_rows(uint8_t state[16]) {
  uint8_t out[16];
  uint32_t row;
  uint32_t col;
  for (col = 0; col < 4u; ++col) {
    for (row = 0; row < 4u; ++row) {
      out[col * 4u + row] = kSbox[state[((col + row) & 3u) * 4u + row]];
    }
  }
  memcpy(state, out, sizeof(out));
}

static void inv_shift_sub_rows(uint8_t state[16]) {
  uint8_t out[16];
  uint32_t row;
  uint32_t col;
  for (col = 0; col < 4u; ++col) {
    for (row = 0; row < 4u; ++row) {
      out[((col + row) & 3u) * 4u + row] = kInvSbox[state[col * 4u + row]];
    }
  }
  memcpy(state, out, sizeof(out));
}

static void mix_columns(uint8_t state[16]) {
  uint32_t col;
  for (col = 0; col < 4u; ++col) {
    uint8_t* c = state + col * 4u;
    uint8_t a0 = c[0];
    uint8_t a1 = c[1];
    uint8_t a2 = c[2];
    uint8_t a3 = c[3];
    uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
    c[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
    c[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
    c[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
    c[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
  }
}

static void inv_mix_columns(uint8_t state[16]) {
  uint32_t col;
  for (col = 0; col < 4u; ++col) {
    uint8_t* c = state + col * 4u;
    uint8_t a0 = c[0];
    uint8_t a1 = c[1];
    uint8_t a2 = c[2];
    uint8_t a3 = c[3];
    c[0] = (uint8_t)(gf_mul(a0, 0x0e) ^ gf_mul(a1, 0x0b) ^ gf_mul(a2, 0x0d) ^ gf_mul(a3, 0x09));
    c[1] = (uint8_t)(gf_mul(a0, 0x09) ^ gf_mul(a1, 0x0e) ^ gf_mul(a2, 0x0b) ^ gf_mul(a3, 0x0d));
    c[2] = (uint8_t)(gf_mul(a0, 0x0d) ^ gf_mul(a1, 0x09) ^ gf_mul(a2, 0x0e) ^ gf_mul(a3, 0x0b));
    c[3] = (uint8_t)(gf_mul(a0, 0x0b) ^ gf_mul(a1, 0x0d) ^ gf_mul(a2, 0x09) ^ gf_mul(a3, 0x0e));
  }
}

int gtav_aes256_init(GtavAes256* aes, const uint8_t key[32]) {
  uint8_t* words;
  uint32_t i;
  if (aes == NULL || key == NULL) return -1;
  words = aes->round_keys;
  memcpy(words, key, GTAV_AES256_KEY_BYTES);
  // 60 four-byte words; Nk = 8.
  for (i = 8u; i < 60u; ++i) {
    uint8_t temp[4];
    memcpy(temp, words + (i - 1u) * 4u, 4u);
    if (i % 8u == 0u) {
      uint8_t first = temp[0];
      temp[0] = (uint8_t)(kSbox[temp[1]] ^ kRcon[i / 8u - 1u]);
      temp[1] = kSbox[temp[2]];
      temp[2] = kSbox[temp[3]];
      temp[3] = kSbox[first];
    } else if (i % 8u == 4u) {
      temp[0] = kSbox[temp[0]];
      temp[1] = kSbox[temp[1]];
      temp[2] = kSbox[temp[2]];
      temp[3] = kSbox[temp[3]];
    }
    words[i * 4u + 0u] = (uint8_t)(words[(i - 8u) * 4u + 0u] ^ temp[0]);
    words[i * 4u + 1u] = (uint8_t)(words[(i - 8u) * 4u + 1u] ^ temp[1]);
    words[i * 4u + 2u] = (uint8_t)(words[(i - 8u) * 4u + 2u] ^ temp[2]);
    words[i * 4u + 3u] = (uint8_t)(words[(i - 8u) * 4u + 3u] ^ temp[3]);
  }
  return 0;
}

void gtav_aes256_wipe(GtavAes256* aes) {
  volatile uint8_t* bytes;
  size_t i;
  if (aes == NULL) return;
  bytes = aes->round_keys;
  for (i = 0; i < sizeof(aes->round_keys); ++i) bytes[i] = 0;
}

int gtav_aes256_encrypt_block(const GtavAes256* aes, const uint8_t in[16], uint8_t out[16]) {
  uint8_t state[16];
  uint32_t round;
  if (aes == NULL || in == NULL || out == NULL) return -1;
  memcpy(state, in, sizeof(state));
  add_round_key(state, aes->round_keys);
  for (round = 1u; round < GTAV_AES256_ROUNDS; ++round) {
    sub_shift_rows(state);
    mix_columns(state);
    add_round_key(state, aes->round_keys + round * GTAV_AES256_BLOCK_BYTES);
  }
  sub_shift_rows(state);
  add_round_key(state, aes->round_keys + GTAV_AES256_ROUNDS * GTAV_AES256_BLOCK_BYTES);
  memcpy(out, state, sizeof(state));
  return 0;
}

int gtav_aes256_decrypt_block(const GtavAes256* aes, const uint8_t in[16], uint8_t out[16]) {
  uint8_t state[16];
  uint32_t round;
  if (aes == NULL || in == NULL || out == NULL) return -1;
  memcpy(state, in, sizeof(state));
  add_round_key(state, aes->round_keys + GTAV_AES256_ROUNDS * GTAV_AES256_BLOCK_BYTES);
  for (round = GTAV_AES256_ROUNDS - 1u; round > 0u; --round) {
    inv_shift_sub_rows(state);
    add_round_key(state, aes->round_keys + round * GTAV_AES256_BLOCK_BYTES);
    inv_mix_columns(state);
  }
  inv_shift_sub_rows(state);
  add_round_key(state, aes->round_keys);
  memcpy(out, state, sizeof(state));
  return 0;
}

static void ecb_blocks(const GtavAes256* aes, uint8_t* data, size_t blocks, int encrypt) {
  size_t i;
  for (i = 0; i < blocks; ++i) {
    uint8_t* block = data + i * GTAV_AES256_BLOCK_BYTES;
    if (encrypt) {
      (void)gtav_aes256_encrypt_block(aes, block, block);
    } else {
      (void)gtav_aes256_decrypt_block(aes, block, block);
    }
  }
}

int gtav_aes256_ecb(const GtavAes256* aes, uint8_t* data, size_t bytes, int encrypt) {
  if (aes == NULL || (data == NULL && bytes != 0u)) return -1;
  if (bytes % GTAV_AES256_BLOCK_BYTES != 0u) return -1;
  ecb_blocks(aes, data, bytes / GTAV_AES256_BLOCK_BYTES, encrypt);
  return 0;
}

// Points `*base` at the text the engine hashes; -1 when the engine would hash garbage.
static int toc_basename(const char* name, const char** base) {
  static const char kMemoryPrefix[] = "memory:";
  const char* cursor;
  if (strncmp(name, kMemoryPrefix, sizeof(kMemoryPrefix) - 1u) == 0) {
    const char* colon = strchr(name + sizeof(kMemoryPrefix) - 1u, ':');
    if (colon == NULL) return -1;
    *base = colon + 1;
    return 0;
  }
  cursor = name + strlen(name);
  while (cursor > name && cursor[-1] != '/' && cursor[-1] != '\\') --cursor;
  *base = cursor;
  return 0;
}

static uint32_t toc_fold(uint8_t byte) {
  if (byte >= (uint8_t)'A' && byte <= (uint8_t)'Z') return (uint32_t)byte + 0x20u;
  if (byte == (uint8_t)'\\') return (uint32_t)'/';
  return byte;
}

static uint32_t toc_hash_base(const char* base) {
  const uint8_t* cursor = (const uint8_t*)base;
  uint32_t hash = 0;
  int quoted = *cursor == (uint8_t)'"';
  if (quoted) ++cursor;
  for (; *cursor != 0u; ++cursor) {
    if (quoted && *cursor == (uint8_t)'"') break;
    hash += toc_fold(*cursor);
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

uint32_t gtav_rpf_toc_name_hash(const char* name) {
  const char* base;
  if (name == NULL || toc_basename(name, &base) != 0) return 0;
  return toc_hash_base(base);
}

int32_t gtav_rpf_toc_index(const char* name, uint64_t size) {
  const char* base;
  if (name == NULL || toc_basename(name, &base) != 0) return -1;
  return (int32_t)((toc_hash_base(base) + (uint32_t)size) % GTAV_RPF_TOC_KEY_COUNT);
}

int gtav_rpf_toc_crypt(uint8_t* toc, size_t bytes, const uint8_t key[32], int encrypt) {
  GtavAes256 aes;
  if (key == NULL || (toc == NULL && bytes != 0u)) return -1;
  if (gtav_aes256_init(&aes, key) != 0) return -1;
  ecb_blocks(&aes, toc, bytes / GTAV_AES256_BLOCK_BYTES, encrypt);
  gtav_aes256_wipe(&aes);
  return 0;
}
