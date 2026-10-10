#pragma once

// Small software AES-256 (FIPS-197) plus the PS5 RPF7 table-of-contents helpers.
//
// The cipher is byte oriented and uses only two constant 256-byte tables (S-box and inverse
// S-box); it never allocates and keeps all state in the caller's GtavAes256. It is not hardened
// against cache-timing side channels: its job is to reproduce the engine's archive TOC cipher
// (AES-256-ECB), not to protect secrets.
//
// No key material lives here or in the matching source: the RPF TOC keys are read by the caller
// from the game's own key table at run time.
//
// Every function returns 0 on success and -1 on invalid arguments (AGENTS.md), except the pure
// hash helper, which cannot fail.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_AES256_KEY_BYTES 32u
#define GTAV_AES256_BLOCK_BYTES 16u
#define GTAV_AES256_ROUNDS 14u

typedef struct {
  // Expanded encryption key schedule: (rounds + 1) round keys of 16 bytes, FIPS-197 byte order.
  uint8_t round_keys[(GTAV_AES256_ROUNDS + 1u) * GTAV_AES256_BLOCK_BYTES];
} GtavAes256;

// Expands `key` into `aes`. The same schedule serves encryption and decryption.
int gtav_aes256_init(GtavAes256* aes, const uint8_t key[32]);

// Overwrites the key schedule with zeros (the compiler may not elide it).
void gtav_aes256_wipe(GtavAes256* aes);

// One 16-byte block. `in` and `out` may alias.
int gtav_aes256_encrypt_block(const GtavAes256* aes, const uint8_t in[16], uint8_t out[16]);
int gtav_aes256_decrypt_block(const GtavAes256* aes, const uint8_t in[16], uint8_t out[16]);

// In-place ECB over `bytes`, which must be a whole number of blocks: a partial trailing block is
// refused (-1, `data` untouched). `encrypt` nonzero encrypts, zero decrypts.
int gtav_aes256_ecb(const GtavAes256* aes, uint8_t* data, size_t bytes, int encrypt);

// PS5 RPF7 header words (little endian at offsets 0 and 0xc).
#define GTAV_RPF_MAGIC 0x52504637u
// Per-archive keyed TOC: key = key_table[gtav_rpf_toc_index(name, size)].
#define GTAV_RPF_TAG_KEYED 0x0FFEFFFFu
// "OPEN" on disk: plaintext TOC. Host-only interchange form; the engine traps on it, so it must be
// converted to GTAV_RPF_TAG_KEYED before any engine call sees the archive.
#define GTAV_RPF_TAG_OPEN 0x4E45504Fu
#define GTAV_RPF_TOC_KEY_COUNT 101u
#define GTAV_RPF_TOC_KEY_BYTES 32u

// The engine's TOC name hash (01.010.002, parser 0x2b37d40 at 0x2b37dc4..0x2b37ed7):
//   - `name` starting with "memory:" (case-sensitive): the text after the next ':' that follows the
//     prefix, i.e. "name.rpf" in "memory:$0x...,N,0:name.rpf"; if that ':' is missing the engine
//     reads an uninitialised hash, so this returns 0 and gtav_rpf_toc_index refuses it;
//   - otherwise the text after the last '/' or '\' (the whole string when there is none);
//   - a basename starting with '"' hashes only up to the next '"' (atStringHash quoting);
//   - each byte is folded through the engine's table 0x3a3d6a0 (ASCII 'A'..'Z' -> lowercase,
//     '\' -> '/', every other byte unchanged), then Jenkins one-at-a-time with the standard finish.
// For a plain lowercase ASCII basename this equals the menu's lowercase joaat.
uint32_t gtav_rpf_toc_name_hash(const char* name);

// Key-table index the engine uses for an archive opened as `name` (a basename, a path, or a
// memory-device name; see gtav_rpf_toc_name_hash) whose file size is `size` bytes:
//   (uint32_t)(hash + (uint32_t)size) % 101.
// Returns 0..100, or -1 for NULL or a "memory:" name without its second ':'.
int32_t gtav_rpf_toc_index(const char* name, uint64_t size);

// In-place AES-256-ECB over the TOC (entries + name pool, the bytes after the 16-byte header),
// encrypt nonzero = host/worker direction, zero = the engine's decrypt direction. Like the engine
// (0x2b0d030 / 0x2b0d150) it processes `bytes & ~15` and leaves a trailing partial block
// unchanged; the writers pad the name pool so no such tail exists. The expanded key is wiped
// before return.
int gtav_rpf_toc_crypt(uint8_t* toc, size_t bytes, const uint8_t key[32], int encrypt);

#ifdef __cplusplus
}
#endif
