#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copy a NUL-terminated string into a fixed-size buffer, always
// NUL-terminating and tolerating NULL dst/src. Shared by the status,
// command-mailbox and patch-broker writers.
void gtav_copy_string(char* dst, size_t dst_len, const char* src);

#ifdef __cplusplus
}
#endif
