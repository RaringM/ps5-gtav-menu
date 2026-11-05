#include <limits.h>
#include <stdarg.h>
#include <stddef.h>

#define STB_SPRINTF_STATIC
#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"

#ifdef __cplusplus
extern "C" {
#endif

int vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
  const int count = size > INT_MAX ? INT_MAX : (int)size;
  return stbsp_vsnprintf(buffer, count, format, args);
}

int snprintf(char* buffer, size_t size, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsnprintf(buffer, size, format, args);
  va_end(args);
  return result;
}

static int scan_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void scan_whitespace(const char** text) {
  while (scan_space(**text)) ++*text;
}

static int scan_unsigned(const char** text, int base, unsigned long* value) {
  const char* current = *text;
  unsigned long result = 0;
  int digits = 0;
  for (;;) {
    int digit = *current >= '0' && *current <= '9'   ? *current - '0'
                : *current >= 'a' && *current <= 'f' ? *current - 'a' + 10
                : *current >= 'A' && *current <= 'F' ? *current - 'A' + 10
                                                     : -1;
    if (digit < 0 || digit >= base) break;
    result = result * (unsigned)base + (unsigned)digit;
    ++current;
    ++digits;
  }
  if (!digits) return 0;
  *text = current;
  *value = result;
  return 1;
}

static int scan_signed(const char** text, long* value) {
  const char* current = *text;
  int negative = 0;
  if (*current == '+' || *current == '-') {
    negative = *current == '-';
    ++current;
  }
  unsigned long magnitude = 0;
  if (!scan_unsigned(&current, 10, &magnitude)) return 0;
  *text = current;
  *value = negative ? -(long)magnitude : (long)magnitude;
  return 1;
}

static int scan_float(const char** text, float* value) {
  const char* current = *text;
  float sign = 1.0f;
  if (*current == '+' || *current == '-') {
    if (*current == '-') sign = -1.0f;
    ++current;
  }
  unsigned long whole = 0;
  const int digits = scan_unsigned(&current, 10, &whole);
  float result = (float)whole;
  if (*current == '.') {
    ++current;
    float place = 0.1f;
    while (*current >= '0' && *current <= '9') {
      result += (float)(*current - '0') * place;
      place *= 0.1f;
      ++current;
    }
  }
  if (!digits && current == *text) return 0;
  if (*current == 'e' || *current == 'E') {
    ++current;
    long exponent = 0;
    if (!scan_signed(&current, &exponent)) return 0;
    float scale = 1.0f;
    for (long i = 0; i < exponent; ++i) scale *= 10.0f;
    for (long i = 0; i > exponent; --i) scale *= 0.1f;
    result *= scale;
  }
  *text = current;
  *value = sign * result;
  return 1;
}

/* strtoull/strtoul: C semantics for the profile parser (base 0 detects 0x / leading 0; a leading
   '-' negates modulo 2^64; overflow saturates). */
unsigned long long strtoull(const char* text, char** end, int base) {
  const char* current = text;
  scan_whitespace(&current);
  int negative = 0;
  if (*current == '+' || *current == '-') {
    negative = *current == '-';
    ++current;
  }
  if ((base == 0 || base == 16) && current[0] == '0' && (current[1] == 'x' || current[1] == 'X')) {
    const char* digits = current + 2;
    const int first = *digits;
    if ((first >= '0' && first <= '9') || (first >= 'a' && first <= 'f') ||
        (first >= 'A' && first <= 'F')) {
      current = digits;
      base = 16;
    }
  }
  if (base == 0) base = current[0] == '0' ? 8 : 10;
  unsigned long long result = 0;
  int digits = 0;
  int overflow = 0;
  for (;;) {
    const int digit = *current >= '0' && *current <= '9'   ? *current - '0'
                      : *current >= 'a' && *current <= 'z' ? *current - 'a' + 10
                      : *current >= 'A' && *current <= 'Z' ? *current - 'A' + 10
                                                           : -1;
    if (digit < 0 || digit >= base) break;
    if (result > (~0ull - (unsigned)digit) / (unsigned)base) overflow = 1;
    result = result * (unsigned)base + (unsigned)digit;
    ++current;
    ++digits;
  }
  if (end) *end = (char*)(digits ? current : text);
  if (!digits) return 0;
  if (overflow) return ~0ull;
  return negative ? (unsigned long long)(0 - result) : result;
}

unsigned long strtoul(const char* text, char** end, int base) {
  return (unsigned long)strtoull(text, end, base);
}

int sscanf(const char* text, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const char* start = text;
  int assigned = 0;
  while (*format) {
    if (*format != '%') {
      if (*text != *format) break;
      ++text;
      ++format;
      continue;
    }
    ++format;
    if (*format == 'n') {
      *va_arg(args, int*) = (int)(text - start);
      ++format;
      continue;
    }
    scan_whitespace(&text);
    if (*format == 'd') {
      long value;
      if (!scan_signed(&text, &value)) break;
      *va_arg(args, int*) = (int)value;
    } else if (*format == 'x') {
      unsigned long value;
      if (!scan_unsigned(&text, 16, &value)) break;
      *va_arg(args, unsigned*) = (unsigned)value;
    } else if (*format == 'u') {
      unsigned long value;
      if (!scan_unsigned(&text, 10, &value)) break;
      *va_arg(args, unsigned*) = (unsigned)value;
    } else if (*format == 'f') {
      float value;
      if (!scan_float(&text, &value)) break;
      *va_arg(args, float*) = value;
    } else {
      break;
    }
    ++assigned;
    ++format;
  }
  va_end(args);
  return assigned;
}

#ifdef __cplusplus
}
#endif
