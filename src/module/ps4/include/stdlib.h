#pragma once

/* Freestanding PS4 worker: no allocator; only the conversions src/module/ps4/format.c defines. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned long strtoul(const char* text, char** end, int base);
unsigned long long strtoull(const char* text, char** end, int base);

#ifdef __cplusplus
}
#endif
