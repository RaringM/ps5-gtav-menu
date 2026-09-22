#include "gtavmenu/hex.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int hex_value(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

int gtav_hex_parse_bytes(const char* text, uint8_t* out, size_t out_cap, size_t* out_len) {
  size_t len = 0;
  int high = -1;

  if (!text || !out || !out_len) return -1;

  for (const char* p = text; *p; ++p) {
    if (isspace((unsigned char)*p) || *p == ':' || *p == ',' || *p == '-') {
      continue;
    }
    if (*p == 'x' || *p == 'X') {
      continue;
    }

    int value = hex_value((unsigned char)*p);
    if (value < 0) return -1;

    if (high < 0) {
      high = value;
    } else {
      if (len >= out_cap) return -1;
      out[len++] = (uint8_t)((high << 4) | value);
      high = -1;
    }
  }

  if (high >= 0) return -1;
  *out_len = len;
  return 0;
}

void gtav_hex_format(const uint8_t* data, size_t len, char* out, size_t out_cap) {
  size_t pos = 0;

  if (!out || out_cap == 0) return;
  out[0] = 0;
  if (!data) return;

  for (size_t i = 0; i < len && pos + 3 < out_cap; ++i) {
    int n = snprintf(out + pos, out_cap - pos, "%02X%s", data[i], i + 1 == len ? "" : " ");
    if (n < 0) break;
    pos += (size_t)n;
  }
}
