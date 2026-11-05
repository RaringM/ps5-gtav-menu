#include "gtavmenu/ps4_stdio.h"
#include "gtavmenu/ps4_syscall.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* FreeBSD open(2) flags. */
#define GTAV_PS4_O_RDONLY 0x0000
#define GTAV_PS4_O_WRONLY 0x0001
#define GTAV_PS4_O_APPEND 0x0008
#define GTAV_PS4_O_CREAT 0x0200
#define GTAV_PS4_O_TRUNC 0x0400
#define GTAV_PS4_O_DIRECTORY 0x20000

/* FreeBSD errno values the layer reports itself. */
#define GTAV_PS4_EIO 5
#define GTAV_PS4_EACCES 13
#define GTAV_PS4_EINVAL 22
#define GTAV_PS4_EMFILE 24
/* Archive hashing/copying uses 64 KiB chunks. Do not turn each into 32 buffered syscalls. */
#define GTAV_PS4_STDIO_TRANSFER_MAX 0x10000u

struct GtavPs4File {
  int used;
  int fd;
  int writing;
  int eof;
  int error;
  uint32_t length; /* bytes in buffer (read: valid bytes; write: pending bytes) */
  uint32_t offset; /* read cursor */
  char buffer[GTAV_PS4_STDIO_BUFFER];
};

static struct GtavPs4File g_files[GTAV_PS4_STDIO_SLOTS];
static int g_enabled;

void gtav_ps4_stdio_set_enabled(int enabled) {
  __atomic_store_n(&g_enabled, enabled ? 1 : 0, __ATOMIC_RELEASE);
}

int gtav_ps4_stdio_enabled(void) {
  return __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
}

static int64_t sys(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
  return gtav_ps4_syscall3(number, a1, a2, a3);
}

/* errno from a failed syscall result (-errno), or `fallback` when it carries none. Returns -1. */
static int fail(int64_t result, int fallback) {
  errno = result < 0 ? (int)-result : fallback;
  return -1;
}

/* A free slot, claimed with one compare-and-exchange: the game thread and the plugin's supervisor
   may both open files. */
static int claim(int* used) {
  int expected = 0;
  return __atomic_compare_exchange_n(used, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void release(int* used) {
  __atomic_store_n(used, 0, __ATOMIC_RELEASE);
}

static int write_all(GtavPs4File* stream, const char* data, uint32_t size) {
  while (size) {
    const int64_t wrote =
        sys(GTAV_PS4_SYS_WRITE, (uint64_t)stream->fd, (uint64_t)(uintptr_t)data, size);
    if (wrote <= 0) {
      stream->error = 1;
      return fail(wrote, GTAV_PS4_EIO);
    }
    data += wrote;
    size -= (uint32_t)wrote;
  }
  return 0;
}

GtavPs4File* gtav_ps4_fopen(const char* path, const char* mode) {
  if (!gtav_ps4_stdio_enabled() || !path || !mode) {
    errno = GTAV_PS4_EACCES;
    return NULL;
  }
  uint64_t flags;
  int writing;
  if (mode[0] == 'r') {
    flags = GTAV_PS4_O_RDONLY;
    writing = 0;
  } else if (mode[0] == 'w') {
    flags = GTAV_PS4_O_WRONLY | GTAV_PS4_O_CREAT | GTAV_PS4_O_TRUNC;
    writing = 1;
  } else if (mode[0] == 'a') {
    flags = GTAV_PS4_O_WRONLY | GTAV_PS4_O_CREAT | GTAV_PS4_O_APPEND;
    writing = 1;
  } else {
    errno = GTAV_PS4_EINVAL;
    return NULL;
  }
  GtavPs4File* stream = NULL;
  for (uint32_t i = 0; i < GTAV_PS4_STDIO_SLOTS; ++i) {
    if (claim(&g_files[i].used)) {
      stream = &g_files[i];
      break;
    }
  }
  if (!stream) {
    errno = GTAV_PS4_EMFILE;
    return NULL;
  }
  const int64_t fd = sys(GTAV_PS4_SYS_OPEN, (uint64_t)(uintptr_t)path, flags, 0666);
  if (fd < 0) {
    release(&stream->used);
    fail(fd, GTAV_PS4_EIO);
    return NULL;
  }
  stream->fd = (int)fd;
  stream->writing = writing;
  stream->eof = 0;
  stream->error = 0;
  stream->length = 0;
  stream->offset = 0;
  return stream;
}

int gtav_ps4_fflush(GtavPs4File* stream) {
  if (!stream || !stream->used) return EOF;
  if (!stream->writing || stream->length == 0) return 0;
  const int result = write_all(stream, stream->buffer, stream->length);
  stream->length = 0;
  return result == 0 ? 0 : EOF;
}

int gtav_ps4_fclose(GtavPs4File* stream) {
  if (!stream || !stream->used) return EOF;
  int result = gtav_ps4_fflush(stream);
  const int64_t closed = sys(GTAV_PS4_SYS_CLOSE, (uint64_t)stream->fd, 0, 0);
  if (closed < 0) result = fail(closed, GTAV_PS4_EIO);
  if (stream->error) result = EOF;
  stream->fd = -1;
  stream->length = 0;
  release(&stream->used);
  return result;
}

int gtav_ps4_feof(GtavPs4File* stream) {
  return stream && stream->used ? stream->eof : 1;
}

int gtav_ps4_ferror(GtavPs4File* stream) {
  return stream && stream->used ? stream->error : 1;
}

static int fill(GtavPs4File* stream) {
  const int64_t got = sys(GTAV_PS4_SYS_READ, (uint64_t)stream->fd,
                          (uint64_t)(uintptr_t)stream->buffer, sizeof(stream->buffer));
  if (got < 0) {
    stream->error = 1;
    return fail(got, GTAV_PS4_EIO);
  }
  stream->length = (uint32_t)got;
  stream->offset = 0;
  if (got == 0) stream->eof = 1;
  return (int)got;
}

char* gtav_ps4_fgets(char* text, int size, GtavPs4File* stream) {
  if (!text || size <= 0 || !stream || !stream->used || stream->writing) return NULL;
  int count = 0;
  while (count < size - 1) {
    if (stream->offset >= stream->length && fill(stream) <= 0) break;
    const char c = stream->buffer[stream->offset++];
    text[count++] = c;
    if (c == '\n') break;
  }
  text[count] = 0;
  return count ? text : NULL;
}

/* Buffer or write `size` bytes. 0, or -1 with errno set; *accepted (when given) is how many bytes
   were written or buffered before a failure. */
static int put(GtavPs4File* stream, const char* data, uint32_t size, uint32_t* accepted) {
  uint32_t done = 0;
  int result = 0;
  if (accepted) *accepted = 0;
  if (!stream || !stream->used || !stream->writing) return fail(0, GTAV_PS4_EINVAL);
  while (size) {
    if (stream->length == 0 && size >= sizeof(stream->buffer)) {
      const uint32_t chunk =
          size < GTAV_PS4_STDIO_TRANSFER_MAX ? size : GTAV_PS4_STDIO_TRANSFER_MAX;
      if (write_all(stream, data, chunk) != 0) {
        result = -1;
        break;
      }
      data += chunk;
      size -= chunk;
      done += chunk;
      continue;
    }
    uint32_t room = (uint32_t)sizeof(stream->buffer) - stream->length;
    if (room == 0) {
      if (gtav_ps4_fflush(stream) != 0) {
        result = -1;
        break;
      }
      room = (uint32_t)sizeof(stream->buffer);
    }
    const uint32_t chunk = size < room ? size : room;
    memcpy(stream->buffer + stream->length, data, chunk);
    stream->length += chunk;
    data += chunk;
    size -= chunk;
    done += chunk;
  }
  if (accepted) *accepted = done;
  return result;
}

int gtav_ps4_fputs(const char* text, GtavPs4File* stream) {
  if (!text) return EOF;
  return put(stream, text, (uint32_t)strlen(text), NULL) == 0 ? 0 : EOF;
}

int gtav_ps4_vfprintf(GtavPs4File* stream, const char* format, va_list args) {
  char line[512];
  const int length = vsnprintf(line, sizeof(line), format, args);
  if (length < 0 || (size_t)length >= sizeof(line)) {
    if (stream) stream->error = 1; /* a profile line never needs this much; refuse to truncate */
    return -1;
  }
  return put(stream, line, (uint32_t)length, NULL) == 0 ? length : -1;
}

/* One path syscall: 0, or -1 with errno. */
static int path_call(uint64_t number, const char* path, uint64_t a2) {
  if (!gtav_ps4_stdio_enabled() || !path) return fail(0, GTAV_PS4_EACCES);
  const int64_t result = sys(number, (uint64_t)(uintptr_t)path, a2, 0);
  return result < 0 ? fail(result, GTAV_PS4_EIO) : 0;
}

int gtav_ps4_remove(const char* path) {
  return path_call(GTAV_PS4_SYS_UNLINK, path, 0);
}

int gtav_ps4_rename(const char* old_path, const char* new_path) {
  if (!new_path) return fail(0, GTAV_PS4_EINVAL);
  return path_call(GTAV_PS4_SYS_RENAME, old_path, (uint64_t)(uintptr_t)new_path);
}

int gtav_ps4_mkdir(const char* path, uint32_t mode) {
  return path_call(GTAV_PS4_SYS_MKDIR, path, mode);
}

int gtav_ps4_stat(const char* path, void* info) {
  if (!info) return fail(0, GTAV_PS4_EINVAL);
  return path_call(GTAV_PS4_SYS_STAT, path, (uint64_t)(uintptr_t)info);
}

int64_t gtav_ps4_read_file(const char* path, void* buffer, uint64_t capacity) {
  if (!gtav_ps4_stdio_enabled() || !path || !buffer) return fail(0, GTAV_PS4_EACCES);
  const int64_t fd = sys(GTAV_PS4_SYS_OPEN, (uint64_t)(uintptr_t)path, GTAV_PS4_O_RDONLY, 0);
  if (fd < 0) return fail(fd, GTAV_PS4_EIO);
  uint64_t done = 0;
  int64_t result = 0;
  for (;;) {
    char probe;
    /* One byte past the capacity tells a file that is too large from one that fits exactly. */
    void* into = done < capacity ? (uint8_t*)buffer + done : (void*)&probe;
    const uint64_t want = done < capacity ? capacity - done : 1u;
    const int64_t got = sys(GTAV_PS4_SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)into,
                            want < 0x10000u ? want : 0x10000u);
    if (got < 0) {
      result = fail(got, GTAV_PS4_EIO);
      break;
    }
    if (got == 0) {
      result = (int64_t)done;
      break;
    }
    if (done >= capacity) {
      result = fail(0, GTAV_PS4_EINVAL);  // larger than the caller's buffer
      break;
    }
    done += (uint64_t)got;
  }
  (void)sys(GTAV_PS4_SYS_CLOSE, (uint64_t)fd, 0, 0);
  return result;
}

size_t gtav_ps4_fread(void* data, size_t size, size_t count, GtavPs4File* stream) {
  if (!data || !size || !count || !stream || !stream->used || stream->writing) return 0;
  if (count > (size_t)-1 / size) {
    stream->error = 1;
    return 0;
  }
  const size_t want = size * count;
  size_t done = 0;
  while (done < want) {
    if (stream->offset >= stream->length && want - done >= sizeof(stream->buffer)) {
      const size_t left = want - done;
      const size_t chunk = left < GTAV_PS4_STDIO_TRANSFER_MAX ? left : GTAV_PS4_STDIO_TRANSFER_MAX;
      const int64_t got =
          sys(GTAV_PS4_SYS_READ, (uint64_t)stream->fd, (uintptr_t)((char*)data + done), chunk);
      stream->length = stream->offset = 0;
      if (got < 0) {
        stream->error = 1;
        fail(got, GTAV_PS4_EIO);
        break;
      }
      if (got == 0) {
        stream->eof = 1;
        break;
      }
      done += (size_t)got;
      continue;
    }
    if (stream->offset >= stream->length && fill(stream) <= 0) break;
    size_t chunk = stream->length - stream->offset;
    if (chunk > want - done) chunk = want - done;
    memcpy((char*)data + done, stream->buffer + stream->offset, chunk);
    stream->offset += (uint32_t)chunk;
    done += chunk;
  }
  return done / size;
}

int gtav_ps4_fgetc(GtavPs4File* stream) {
  if (!stream || !stream->used || stream->writing) return EOF;
  if (stream->offset >= stream->length && fill(stream) <= 0) return EOF;
  return (unsigned char)stream->buffer[stream->offset++];
}

int gtav_ps4_fseek(GtavPs4File* stream, int64_t offset, int whence) {
  if (!stream || !stream->used || whence < 0 || whence > 2) return fail(0, GTAV_PS4_EINVAL);
  if (stream->writing && gtav_ps4_fflush(stream) != 0) return -1;
  if (!stream->writing && whence == SEEK_CUR) {
    const int64_t unread = (int64_t)(stream->length - stream->offset);
    if (offset < INT64_MIN + unread) return fail(0, GTAV_PS4_EINVAL);
    offset -= unread; /* the kernel cursor is ahead of the buffered reader's logical cursor */
  }
  const int64_t result =
      sys(GTAV_PS4_SYS_LSEEK, (uint64_t)stream->fd, (uint64_t)offset, (uint64_t)whence);
  if (result < 0) return fail(result, GTAV_PS4_EIO);
  stream->length = 0;
  stream->offset = 0;
  stream->eof = 0;
  return 0;
}

size_t gtav_ps4_fwrite(const void* data, size_t size, size_t count, GtavPs4File* stream) {
  if (!data || !size || !count) return 0;
  if (count > UINT32_MAX / size) {
    if (stream && stream->used) stream->error = 1;
    return 0;
  }
  /* Whole items written or buffered before a failure, as C's fwrite reports them. */
  uint32_t accepted = 0;
  if (put(stream, (const char*)data, (uint32_t)(size * count), &accepted) == 0) return count;
  return accepted / size;
}

struct GtavPs4Dir {
  int used;
  int fd;
  int done;         /* end, error, malformed record or the entry cap */
  uint32_t entries; /* entries returned so far */
  uint32_t length;  /* bytes the last getdents filled */
  uint32_t offset;  /* next record in buffer */
  GtavPs4Dirent entry;
  char buffer[GTAV_PS4_DIR_BUFFER];
};

static struct GtavPs4Dir g_dir;

GtavPs4Dir* gtav_ps4_opendir(const char* path) {
  if (!gtav_ps4_stdio_enabled() || !path) {
    errno = GTAV_PS4_EACCES;
    return NULL;
  }
  if (!claim(&g_dir.used)) {
    errno = GTAV_PS4_EMFILE;
    return NULL;
  }
  const int64_t fd = sys(GTAV_PS4_SYS_OPEN, (uint64_t)(uintptr_t)path,
                         GTAV_PS4_O_RDONLY | GTAV_PS4_O_DIRECTORY, 0);
  if (fd < 0) {
    release(&g_dir.used);
    fail(fd, GTAV_PS4_EIO);
    return NULL;
  }
  g_dir.fd = (int)fd;
  g_dir.done = 0;
  g_dir.entries = 0;
  g_dir.length = 0;
  g_dir.offset = 0;
  return &g_dir;
}

/* FreeBSD 9 records: u32 d_fileno, u16 d_reclen, u8 d_type, u8 d_namlen, then the name. */
GtavPs4Dirent* gtav_ps4_readdir(GtavPs4Dir* dir) {
  if (!dir || !dir->used) return NULL;
  while (!dir->done) {
    if (dir->offset >= dir->length) {
      const int64_t got = sys(GTAV_PS4_SYS_GETDENTS, (uint64_t)dir->fd,
                              (uint64_t)(uintptr_t)dir->buffer, sizeof(dir->buffer));
      if (got <= 0 || (uint64_t)got > sizeof(dir->buffer)) {
        if (got != 0) fail(got, GTAV_PS4_EIO);
        dir->done = 1;
        break;
      }
      dir->length = (uint32_t)got;
      dir->offset = 0;
    }
    const uint8_t* record = (const uint8_t*)dir->buffer + dir->offset;
    const uint32_t left = dir->length - dir->offset;
    uint32_t fileno = 0;
    uint16_t reclen = 0;
    if (left >= 8u) {
      memcpy(&fileno, record, sizeof(fileno));
      memcpy(&reclen, record + 4, sizeof(reclen));
    }
    const uint32_t namlen = left >= 8u ? record[7] : 0u;
    if (left < 8u || reclen < 8u || reclen > left || 8u + namlen > reclen) {
      errno = GTAV_PS4_EIO;  // a malformed record ends the listing
      dir->done = 1;
      break;
    }
    dir->offset += reclen;
    if (fileno == 0 || namlen == 0) continue;  // an empty slot
    if (dir->entries >= GTAV_PS4_DIR_MAX_ENTRIES) {
      dir->done = 1;
      break;
    }
    dir->entries++;
    GtavPs4Dirent* entry = &dir->entry;
    entry->d_fileno = fileno;
    entry->d_reclen = reclen;
    entry->d_type = record[6];
    entry->d_namlen = (uint8_t)namlen;
    memcpy(entry->d_name, record + 8, namlen);
    entry->d_name[namlen] = 0;
    return entry;
  }
  return NULL;
}

int gtav_ps4_closedir(GtavPs4Dir* dir) {
  if (!dir || !dir->used) return fail(0, GTAV_PS4_EINVAL);
  const int64_t closed = sys(GTAV_PS4_SYS_CLOSE, (uint64_t)dir->fd, 0, 0);
  dir->fd = -1;
  release(&dir->used);
  return closed < 0 ? fail(closed, GTAV_PS4_EIO) : 0;
}
