#pragma once

/* FreeBSD errno values. errno is one int in platform.c (the worker is single-threaded); the
 * bounded /data layer (stdio_fs.c) sets it on every failure. */

#ifdef __cplusplus
extern "C" {
#endif

int* gtav_ps4_errno_location(void);
#define errno (*gtav_ps4_errno_location())

#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define EACCES 13
#define EMFILE 24
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define ENAMETOOLONG 63
#define ENOSYS 78

#ifdef __cplusplus
}
#endif
