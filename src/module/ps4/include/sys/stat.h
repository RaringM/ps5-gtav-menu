#pragma once

/* FreeBSD 9 amd64 struct stat (120 bytes), as the PS4 kernel fills it. */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gtav_ps4_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct stat {
  uint32_t st_dev;
  uint32_t st_ino;
  mode_t st_mode;
  uint16_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t st_rdev;
  struct gtav_ps4_timespec st_atim;
  struct gtav_ps4_timespec st_mtim;
  struct gtav_ps4_timespec st_ctim;
  off_t st_size;
  int64_t st_blocks;
  uint32_t st_blksize;
  uint32_t st_flags;
  uint32_t st_gen;
  int32_t st_lspare;
  struct gtav_ps4_timespec st_birthtim;
};

_Static_assert(sizeof(struct stat) == 120, "FreeBSD 9 amd64 struct stat size");
_Static_assert(__builtin_offsetof(struct stat, st_mode) == 8, "FreeBSD 9 amd64 st_mode offset");

#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)

int stat(const char* path, struct stat* info);
int mkdir(const char* path, mode_t mode);

#ifdef __cplusplus
}
#endif
