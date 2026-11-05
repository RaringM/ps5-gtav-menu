#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t time_t;

time_t time(time_t* result);

#ifdef __cplusplus
}
#endif
