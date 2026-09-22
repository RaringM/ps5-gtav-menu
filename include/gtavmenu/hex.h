#pragma once

#include <stddef.h>
#include <stdint.h>

// Hex <-> byte helpers (pure, host-testable, no allocation). All buffers are caller-owned.
// Functions follow the project's 0/-1 convention.

#ifdef __cplusplus
extern "C" {
#endif

// Parse a hex byte string from `text` into `out` (capacity `out_cap`); writes the decoded
// length to `*out_len`. Whitespace and ':' ',' '-' separators are skipped, as are 'x'/'X'
// (so an "0x" prefix is tolerated). Returns 0 on success, -1 on an invalid character, an
// odd number of hex digits, overflow past `out_cap`, or a NULL argument.
int gtav_hex_parse_bytes(const char* text, uint8_t* out, size_t out_cap, size_t* out_len);
// Format `len` bytes as space-separated UPPERCASE hex (e.g. "DE AD") into `out`,
// NUL-terminated and truncated to fit `out_cap`. No-op when `out` is NULL or `out_cap` is
// 0; a NULL `data` yields an empty string.
void gtav_hex_format(const uint8_t* data, size_t len, char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif
