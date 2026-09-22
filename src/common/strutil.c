#include "gtavmenu/strutil.h"

#include <string.h>

void gtav_copy_string(char* dst, size_t dst_len, const char* src) {
  if (!dst || dst_len == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = 0;
}
