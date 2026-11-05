#pragma once

/* Bounded stdio over raw FreeBSD syscalls for the freestanding PS4 worker
 * (src/module/ps4/stdio_fs.c).
 *
 * The 01.57 sandbox exposes /data (a hardware probe confirmed it), so profile and map files can
 * live where the PS5 build keeps them. Streams are line-oriented text: a few fixed slots, each with
 * a small buffer; nothing allocates. Every call fails closed until gtav_ps4_stdio_set_enabled(1) --
 * the worker only enables it once the host has granted storage (GTAV_PS4_HOST_STORAGE_READY).
 * platform.c maps the libc names (fopen, fgets, ..., opendir) onto these. Failures set errno. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PS4_STDIO_SLOTS 4u
#define GTAV_PS4_STDIO_BUFFER 2048u

typedef struct GtavPs4File GtavPs4File;

void gtav_ps4_stdio_set_enabled(int enabled);
int gtav_ps4_stdio_enabled(void);

GtavPs4File* gtav_ps4_fopen(const char* path, const char* mode);
int gtav_ps4_fclose(GtavPs4File* stream);
int gtav_ps4_feof(GtavPs4File* stream);
int gtav_ps4_ferror(GtavPs4File* stream);
int gtav_ps4_fflush(GtavPs4File* stream);
char* gtav_ps4_fgets(char* text, int size, GtavPs4File* stream);
int gtav_ps4_fputs(const char* text, GtavPs4File* stream);
int gtav_ps4_vfprintf(GtavPs4File* stream, const char* format, va_list args);
int gtav_ps4_remove(const char* path);
int gtav_ps4_rename(const char* old_path, const char* new_path);
int gtav_ps4_mkdir(const char* path, uint32_t mode);
/* info points at a FreeBSD struct stat (GTAV_PS4_STAT_SIZE bytes). */
int gtav_ps4_stat(const char* path, void* info);
/* Whole-file binary read (the custom-pack descriptor and data files): the byte count, or -1 when
 * the file is missing, unreadable or larger than `capacity`. Fails closed like the rest. */
int64_t gtav_ps4_read_file(const char* path, void* buffer, uint64_t capacity);
/* Binary reads and writes through a stream's buffer (the pack selection files). */
size_t gtav_ps4_fread(void* data, size_t size, size_t count, GtavPs4File* stream);
size_t gtav_ps4_fwrite(const void* data, size_t size, size_t count, GtavPs4File* stream);
int gtav_ps4_fgetc(GtavPs4File* stream);
int gtav_ps4_fseek(GtavPs4File* stream, int64_t offset, int whence);

/* Directory listing over getdents(2): one directory open at a time, at most
 * GTAV_PS4_DIR_MAX_ENTRIES entries per listing (the pack root). Fails closed like the rest. */
#define GTAV_PS4_DIR_BUFFER 2048u
#define GTAV_PS4_DIR_MAX_ENTRIES 1024u

typedef struct GtavPs4Dir GtavPs4Dir;

/* One entry, in the FreeBSD 9 struct dirent layout (d_name NUL-terminated). */
typedef struct {
  uint32_t d_fileno;
  uint16_t d_reclen;
  uint8_t d_type;
  uint8_t d_namlen;
  char d_name[256];
} GtavPs4Dirent;

GtavPs4Dir* gtav_ps4_opendir(const char* path);
/* The next entry (valid until the next call), or NULL at the end, on an error or past the cap. */
GtavPs4Dirent* gtav_ps4_readdir(GtavPs4Dir* dir);
int gtav_ps4_closedir(GtavPs4Dir* dir);

#ifdef __cplusplus
}
#endif
