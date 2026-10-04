#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gtavmenu/custom_mount.h"

int gtav_custom_sandbox_index(const char* name, const char* title, long* index_out) {
  size_t n;
  const char* digits;
  char* end = NULL;
  long index;

  if (!name || !title || !*title) return 0;
  n = strlen(title);
  if (strncmp(name, title, n) != 0 || name[n] != '_') return 0;
  digits = name + n + 1;
  if (*digits < '0' || *digits > '9') return 0;
  index = strtol(digits, &end, 10);
  if (!end || *end != '\0' || index < 0) return 0;
  if (index_out) *index_out = index;
  return 1;
}

int gtav_custom_pick_sandbox(const char* const* names, const int* live, int count,
                             const char* title, char* out, size_t out_size) {
  long best = -1;
  int best_i = -1;

  if (!names || !live || !out || out_size == 0) return -1;
  for (int i = 0; i < count; ++i) {
    long index;
    if (live[i] && gtav_custom_sandbox_index(names[i], title, &index) && index > best) {
      best = index;
      best_i = i;
    }
  }
  if (best_i < 0 || snprintf(out, out_size, "%s", names[best_i]) >= (int)out_size) return -1;
  return 0;
}
