#include "gtavmenu/ini_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char* gtav_ini_trim(char* s) {
  while (*s && isspace((unsigned char)*s)) ++s;
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) {
    *--end = 0;
  }
  return s;
}

int gtav_ini_split(char* line, char** key, char** value) {
  char* cursor = gtav_ini_trim(line);
  if (!cursor[0] || cursor[0] == '#') return -1;

  char* equals = strchr(cursor, '=');
  if (!equals) return -1;
  *equals = 0;

  *key = gtav_ini_trim(cursor);
  *value = gtav_ini_trim(equals + 1);
  return 0;
}

uint32_t gtav_ini_parse_u32(const char* value) {
  if (!value) return 0;
  return (uint32_t)strtoul(value, NULL, 0);
}

uint64_t gtav_ini_parse_u64(const char* value) {
  if (!value) return 0;
  return (uint64_t)strtoull(value, NULL, 0);
}

int gtav_ini_truthy(const char* value) {
  return value && (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "yes") ||
                   !strcmp(value, "on"));
}
