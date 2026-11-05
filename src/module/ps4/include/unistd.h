#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int usleep(useconds_t usec);
int unlink(const char* path);

#ifdef __cplusplus
}
#endif
