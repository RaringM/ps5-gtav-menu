#pragma once

/* Freestanding PS4 worker libc surface: only what src/module/ps4 defines (platform.c, format.c). */

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavPs4File FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
int fflush(FILE* stream);
char* fgets(char* text, int size, FILE* stream);
int fprintf(FILE* stream, const char* format, ...);
int fputs(const char* text, FILE* stream);
size_t fread(void* data, size_t size, size_t count, FILE* stream);
size_t fwrite(const void* data, size_t size, size_t count, FILE* stream);
int fgetc(FILE* stream);
int fseek(FILE* stream, long offset, int whence);
int remove(const char* path);
int rename(const char* old_path, const char* new_path);
int snprintf(char* buffer, size_t size, const char* format, ...);
int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
int sscanf(const char* text, const char* format, ...);

#ifdef __cplusplus
}
#endif
