#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* destination, int value, size_t size) {
  unsigned char* out = (unsigned char*)destination;
  for (size_t i = 0; i < size; ++i) out[i] = (unsigned char)value;
  return destination;
}

void* memcpy(void* destination, const void* source, size_t size) {
  unsigned char* out = (unsigned char*)destination;
  const unsigned char* in = (const unsigned char*)source;
  for (size_t i = 0; i < size; ++i) out[i] = in[i];
  return destination;
}

int strcmp(const char* left, const char* right) {
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char* left, const char* right, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const unsigned char a = (unsigned char)left[i];
    const unsigned char b = (unsigned char)right[i];
    if (a != b) return a - b;
    if (!a) return 0;
  }
  return 0;
}

int memcmp(const void* left, const void* right, size_t size) {
  const unsigned char* a = (const unsigned char*)left;
  const unsigned char* b = (const unsigned char*)right;
  for (size_t i = 0; i < size; ++i) {
    if (a[i] != b[i]) return a[i] - b[i];
  }
  return 0;
}

void* memchr(const void* data, int value, size_t size) {
  const unsigned char* in = (const unsigned char*)data;
  for (size_t i = 0; i < size; ++i) {
    if (in[i] == (unsigned char)value) return (void*)(in + i);
  }
  return 0;
}

void* memmove(void* destination, const void* source, size_t size) {
  unsigned char* out = (unsigned char*)destination;
  const unsigned char* in = (const unsigned char*)source;
  if (out < in) {
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
  } else {
    for (size_t i = size; i > 0; --i) out[i - 1] = in[i - 1];
  }
  return destination;
}

size_t strlen(const char* text) {
  size_t length = 0;
  while (text[length]) ++length;
  return length;
}

char* strncpy(char* destination, const char* source, size_t size) {
  size_t i = 0;
  for (; i < size && source[i]; ++i) destination[i] = source[i];
  for (; i < size; ++i) destination[i] = 0;
  return destination;
}

char* strchr(const char* text, int character) {
  do {
    if (*text == (char)character) return (char*)text;
  } while (*text++);
  return 0;
}

char* strrchr(const char* text, int character) {
  const char* found = 0;
  do {
    if (*text == (char)character) found = text;
  } while (*text++);
  return (char*)found;
}

char* strstr(const char* text, const char* needle) {
  if (!*needle) return (char*)text;
  for (; *text; ++text) {
    size_t i = 0;
    while (needle[i] && text[i] == needle[i]) ++i;
    if (!needle[i]) return (char*)text;
  }
  return 0;
}

#ifdef __cplusplus
}
#endif
