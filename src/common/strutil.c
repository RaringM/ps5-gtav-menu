#include "gtavmenu/strutil.h"

#include <string.h>

void gtav_copy_string(char* dst, size_t dst_len, const char* src) {
  if (!dst || dst_len == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = 0;
  /* Never leave a partial UTF-8 sequence at the end of a fixed buffer. Walk backward over
   * continuation bytes and discard the lead byte when the complete sequence did not fit. */
  size_t n = strlen(dst);
  if (n == 0 || src[n] == 0) return;
  size_t lead = n - 1u;
  while (lead > 0 && (((unsigned char)dst[lead] & 0xC0u) == 0x80u)) --lead;
  unsigned char c = (unsigned char)dst[lead];
  size_t expected = c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u));
  if (n - lead < expected) dst[lead] = 0;
}
