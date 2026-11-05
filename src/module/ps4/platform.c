#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gtavmenu/ps4_stdio.h"
#include "gtavmenu/ps4_syscall.h"
#include GTAV_PS4_TARGET_HEADER

#ifdef __cplusplus
extern "C" {
#endif

/* stdio maps onto the bounded /data layer (stdio_fs.c), which fails closed until the host grants
   storage. FILE is struct GtavPs4File in the vendored <stdio.h>. */
FILE* fopen(const char* path, const char* mode) {
  return gtav_ps4_fopen(path, mode);
}

int fclose(FILE* stream) {
  return gtav_ps4_fclose(stream);
}

int feof(FILE* stream) {
  return gtav_ps4_feof(stream);
}

int ferror(FILE* stream) {
  return gtav_ps4_ferror(stream);
}

int fflush(FILE* stream) {
  return gtav_ps4_fflush(stream);
}

char* fgets(char* text, int size, FILE* stream) {
  return gtav_ps4_fgets(text, size, stream);
}

int fprintf(FILE* stream, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int result = gtav_ps4_vfprintf(stream, format, args);
  va_end(args);
  return result;
}

int fputs(const char* text, FILE* stream) {
  return gtav_ps4_fputs(text, stream);
}

size_t fread(void* data, size_t size, size_t count, FILE* stream) {
  return gtav_ps4_fread(data, size, count, stream);
}

size_t fwrite(const void* data, size_t size, size_t count, FILE* stream) {
  return gtav_ps4_fwrite(data, size, count, stream);
}

int fgetc(FILE* stream) {
  return gtav_ps4_fgetc(stream);
}

int fseek(FILE* stream, long offset, int whence) {
  return gtav_ps4_fseek(stream, offset, whence);
}

DIR* opendir(const char* path) {
  return gtav_ps4_opendir(path);
}

/* struct dirent is GtavPs4Dirent spelled the libc way. */
_Static_assert(sizeof(struct dirent) == sizeof(GtavPs4Dirent) &&
                   __builtin_offsetof(struct dirent, d_name) ==
                       __builtin_offsetof(GtavPs4Dirent, d_name),
               "struct dirent and GtavPs4Dirent differ");
struct dirent* readdir(DIR* dir) {
  return (struct dirent*)(void*)gtav_ps4_readdir(dir);
}

int closedir(DIR* dir) {
  return gtav_ps4_closedir(dir);
}

int unlink(const char* path) {
  return gtav_ps4_remove(path);
}

/* One int, not per thread: the menu's file work runs on the game thread inside the hook. The
   plugin's supervisor writes its own files through libkernel and never reads this errno. */
int* gtav_ps4_errno_location(void) {
  static int value;
  return &value;
}

int mkdir(const char* path, mode_t mode) {
  return gtav_ps4_mkdir(path, mode);
}

int remove(const char* path) {
  return gtav_ps4_remove(path);
}

int rename(const char* old_path, const char* new_path) {
  return gtav_ps4_rename(old_path, new_path);
}

int stat(const char* path, struct stat* info) {
  return gtav_ps4_stat(path, info);
}

time_t time(time_t* result) {
  static time_t s_high;
  time_t now = (time_t)(*(volatile uint32_t*)(uintptr_t)GTAV_PS4_GAME_TIMER / 1000u);
  if (now < s_high) now = s_high;
  s_high = now;
  if (result) *result = now;
  return now;
}

int usleep(useconds_t usec) {
  (void)usec;
  return 0;
}

int64_t gtav_ps4_syscall3(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
  uint64_t rax = number;
  uint64_t rdx = a3;
  uint8_t failed;
  /* FreeBSD amd64: arguments in rdi/rsi/rdx, carry set on error with the errno in rax. The
     kernel clobbers rcx and r11; r8-r10 are listed so no argument register is assumed live. */
  __asm__ volatile("syscall"
                   : "+a"(rax), "+d"(rdx), "=@ccc"(failed)
                   : "D"(a1), "S"(a2)
                   : "rcx", "r8", "r9", "r10", "r11", "memory");
  return failed ? -(int64_t)rax : (int64_t)rax;
}

#ifdef __cplusplus
}
#endif
