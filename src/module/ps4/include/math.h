#pragma once

/* Freestanding PS4 worker libc surface: only what src/module/ps4/math.c defines. */

#ifdef __cplusplus
extern "C" {
#endif

float sinf(float value);
float cosf(float value);
float sqrtf(float value);
float fabsf(float value);

#ifdef __cplusplus
}
#endif
