#pragma once

/* Raw FreeBSD 9 syscalls for the freestanding PS4 worker (no libkernel import). Numbers follow
 * the PS4 payload SDK (libPS4/source/file.c). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PS4_SYS_READ 3
#define GTAV_PS4_SYS_WRITE 4
#define GTAV_PS4_SYS_OPEN 5
#define GTAV_PS4_SYS_CLOSE 6
#define GTAV_PS4_SYS_UNLINK 10
#define GTAV_PS4_SYS_GETPID 20
#define GTAV_PS4_SYS_MINCORE 78
#define GTAV_PS4_SYS_RENAME 128
#define GTAV_PS4_SYS_MKDIR 136
#define GTAV_PS4_SYS_STAT 188
#define GTAV_PS4_SYS_GETDENTS 272
#define GTAV_PS4_SYS_LSEEK 478

/* FreeBSD 9 amd64 struct stat is 120 bytes; st_mode is the uint16 at offset 8. */
#define GTAV_PS4_STAT_SIZE 120u
#define GTAV_PS4_STAT_MODE_OFFSET 8u
#define GTAV_PS4_S_IFMT 0170000u
#define GTAV_PS4_S_IFDIR 0040000u

/* Returns the syscall result, or a negative errno when the kernel reports failure. */
int64_t gtav_ps4_syscall3(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3);

#ifdef __cplusplus
}
#endif
