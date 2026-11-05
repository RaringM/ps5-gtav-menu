#pragma once

/* Freestanding PS4 worker: the directory listing src/module/ps4/stdio_fs.c implements. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavPs4Dir DIR;

/* FreeBSD 9 layout, the same as GtavPs4Dirent (gtavmenu/ps4_stdio.h). */
struct dirent {
  uint32_t d_fileno;
  uint16_t d_reclen;
  uint8_t d_type;
  uint8_t d_namlen;
  char d_name[256];
};

#ifdef __cplusplus
static_assert(sizeof(struct dirent) == 264, "FreeBSD 9 struct dirent size");
#else
_Static_assert(sizeof(struct dirent) == 264, "FreeBSD 9 struct dirent size");
#endif

DIR* opendir(const char* path);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);

#ifdef __cplusplus
}
#endif
