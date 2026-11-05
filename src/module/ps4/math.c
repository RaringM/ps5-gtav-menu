#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static float sin_poly(float x) {
  const float x2 = x * x;
  return x * (1.0f - x2 / 6.0f + x2 * x2 / 120.0f - x2 * x2 * x2 / 5040.0f +
              x2 * x2 * x2 * x2 / 362880.0f - x2 * x2 * x2 * x2 * x2 / 39916800.0f);
}

static float cos_poly(float x) {
  const float x2 = x * x;
  return 1.0f - x2 / 2.0f + x2 * x2 / 24.0f - x2 * x2 * x2 / 720.0f + x2 * x2 * x2 * x2 / 40320.0f -
         x2 * x2 * x2 * x2 * x2 / 3628800.0f + x2 * x2 * x2 * x2 * x2 * x2 / 479001600.0f;
}

static int quadrant(float* value) {
  const float half_pi = 1.57079632679489661923f;
  const int32_t q = (int32_t)(*value * (2.0f / 3.14159265358979323846f));
  *value -= (float)q * half_pi;
  return q & 3;
}

float sqrtf(float value) {
  float result;
  __asm__("sqrtss %1,%0" : "=x"(result) : "x"(value));
  return result;
}

float fabsf(float value) {
  return __builtin_fabsf(value);
}

float sinf(float value) {
  switch (quadrant(&value)) {
    case 0:
      return sin_poly(value);
    case 1:
      return cos_poly(value);
    case 2:
      return -sin_poly(value);
    default:
      return -cos_poly(value);
  }
}

float cosf(float value) {
  switch (quadrant(&value)) {
    case 0:
      return cos_poly(value);
    case 1:
      return -sin_poly(value);
    case 2:
      return -cos_poly(value);
    default:
      return sin_poly(value);
  }
}

#ifdef __cplusplus
}
#endif
