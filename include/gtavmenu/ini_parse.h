#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal in-place INI line parsing shared by runtime.cfg (runtime_config.c) and
// the feature profile (feature_profile.c). Both files read the same way: trim
// whitespace, skip blank/#comment lines, split "key=value" at the first '=', and
// parse base-0 integers.

// Trim leading/trailing ASCII whitespace in place; returns the first non-space.
char* gtav_ini_trim(char* s);

// Parse one line in place. On a real "key=value" line, sets *key and *value to
// the trimmed halves and returns 0. On a blank, comment ('#'), or '='-less line,
// returns -1 so the caller can skip it.
int gtav_ini_split(char* line, char** key, char** value);

// Base-0 integer parse (accepts 0x-prefixed hex and decimal); NULL -> 0.
uint32_t gtav_ini_parse_u32(const char* value);
uint64_t gtav_ini_parse_u64(const char* value);

// 1 for "1"/"true"/"yes"/"on", otherwise 0.
int gtav_ini_truthy(const char* value);

#ifdef __cplusplus
}
#endif
