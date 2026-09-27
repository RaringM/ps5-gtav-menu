#include "gtavmenu/supervisor_lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int make_path(char* path, size_t size, const char* directory, const char* name) {
  int count = snprintf(path, size, "%s/%s", directory, name);
  if (count < 0 || (size_t)count >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int write_all(int fd, const void* data, size_t size) {
  const uint8_t* bytes = (const uint8_t*)data;
  size_t written = 0;
  while (written < size) {
    ssize_t count = write(fd, bytes + written, size - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count == 0) errno = EIO;
      return -1;
    }
    written += (size_t)count;
  }
  return 0;
}

static int read_exact_file(const char* path, void* data, size_t size) {
  uint8_t extra;
  size_t used = 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return -1;
  while (used < size) {
    ssize_t count = read(fd, (uint8_t*)data + used, size - used);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      int saved_errno = count == 0 ? EINVAL : errno;
      close(fd);
      errno = saved_errno;
      return -1;
    }
    used += (size_t)count;
  }
  for (;;) {
    ssize_t count = read(fd, &extra, 1);
    if (count < 0 && errno == EINTR) continue;
    if (count != 0) {
      int saved_errno = count < 0 ? errno : EINVAL;
      close(fd);
      errno = saved_errno;
      return -1;
    }
    break;
  }
  if (close(fd) != 0) return -1;
  return 0;
}

static int write_record_atomic(const char* path, const void* record, size_t size, uint64_t token) {
  char temporary[GTAV_DAEMON_PATH_MAX];
  int count =
      snprintf(temporary, sizeof(temporary), "%s.%d.%08x.tmp", path, getpid(), (unsigned int)token);
  if (count < 0 || (size_t)count >= sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if (fd < 0) return -1;
  int result = write_all(fd, record, size);
  int saved_errno = errno;
  if (result == 0 && fsync(fd) != 0) {
    result = -1;
    saved_errno = errno;
  }
  if (close(fd) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result == 0 && rename(temporary, path) != 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0) unlink(temporary);
  errno = saved_errno;
  return result;
}

static int session_valid(const GtavSupervisorSessionRecord* record) {
  return record->magic == GTAV_SUPERVISOR_SESSION_MAGIC &&
         record->abi_version == GTAV_SUPERVISOR_LIFECYCLE_ABI &&
         record->struct_size == sizeof(*record) && record->token != 0 &&
         record->supervisor_pid > 1 && record->owner_path[0] == '/' &&
         memchr(record->owner_path, '\0', sizeof(record->owner_path)) != NULL;
}

static int ack_valid(const GtavSupervisorAckRecord* record) {
  return record->magic == GTAV_SUPERVISOR_ACK_MAGIC &&
         record->abi_version == GTAV_SUPERVISOR_LIFECYCLE_ABI &&
         record->struct_size == sizeof(*record) && record->token != 0 && record->runtime_pid > 1;
}

static void unlink_session_if_token(const char* path, uint64_t token) {
  GtavSupervisorSessionRecord record;
  if (read_exact_file(path, &record, sizeof(record)) == 0 && session_valid(&record) &&
      record.token == token) {
    unlink(path);
  }
}

static void unlink_ack_if_token(const char* path, uint64_t token) {
  GtavSupervisorAckRecord record;
  if (read_exact_file(path, &record, sizeof(record)) == 0 && ack_valid(&record) &&
      record.token == token) {
    unlink(path);
  }
}

int gtav_supervisor_begin(GtavSupervisorLease* lease, const char* directory) {
  struct timespec monotonic;
  GtavSupervisorSessionRecord record;
  char owner_name[96];
  struct stat st;
  int count;

  if (lease == NULL || directory == NULL || lease->owner_fd >= 0) {
    errno = EINVAL;
    return -1;
  }
  if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) return -1;
  lease->token = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)monotonic.tv_sec ^
                 ((uint64_t)monotonic.tv_nsec << 1);
  if (lease->token == 0) lease->token = 1;
  count = snprintf(owner_name, sizeof(owner_name), "supervisor.%d.%016llx.owner", getpid(),
                   (unsigned long long)lease->token);
  if (count < 0 || (size_t)count >= sizeof(owner_name) ||
      make_path(lease->owner_path, sizeof(lease->owner_path), directory, owner_name) != 0 ||
      make_path(lease->session_path, sizeof(lease->session_path), directory,
                "supervisor.session") != 0 ||
      make_path(lease->ack_path, sizeof(lease->ack_path), directory, "supervisor.ack") != 0) {
    return -1;
  }

  lease->owner_fd = open(lease->owner_path, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW, 0600);
  if (lease->owner_fd < 0) return -1;
  if (fstat(lease->owner_fd, &st) != 0) {
    int saved_errno = errno;
    close(lease->owner_fd);
    lease->owner_fd = -1;
    unlink(lease->owner_path);
    errno = saved_errno;
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    close(lease->owner_fd);
    lease->owner_fd = -1;
    unlink(lease->owner_path);
    errno = EINVAL;
    return -1;
  }
  if (flock(lease->owner_fd, LOCK_EX | LOCK_NB) != 0) {
    int saved_errno = errno;
    close(lease->owner_fd);
    lease->owner_fd = -1;
    unlink(lease->owner_path);
    errno = saved_errno;
    return -1;
  }

  memset(&record, 0, sizeof(record));
  record.magic = GTAV_SUPERVISOR_SESSION_MAGIC;
  record.abi_version = GTAV_SUPERVISOR_LIFECYCLE_ABI;
  record.struct_size = sizeof(record);
  record.token = lease->token;
  record.supervisor_pid = getpid();
  memcpy(record.owner_path, lease->owner_path, strlen(lease->owner_path) + 1u);
  // Callers hold the global daemon-control gate here, so no runtime can own an acknowledgement.
  // Remove malformed or crashed-session leftovers before the new helper starts polling.
  if (unlink(lease->ack_path) != 0 && errno != ENOENT) {
    int saved_errno = errno;
    close(lease->owner_fd);
    lease->owner_fd = -1;
    unlink(lease->owner_path);
    errno = saved_errno;
    return -1;
  }
  if (write_record_atomic(lease->session_path, &record, sizeof(record), lease->token) != 0) {
    int saved_errno = errno;
    close(lease->owner_fd);
    lease->owner_fd = -1;
    unlink(lease->owner_path);
    errno = saved_errno;
    return -1;
  }
  return 0;
}

int gtav_supervisor_ack(const GtavSupervisorLease* lease, pid_t* pid_out) {
  GtavSupervisorAckRecord record;
  if (lease == NULL || pid_out == NULL || lease->owner_fd < 0) {
    errno = EINVAL;
    return -1;
  }
  if (read_exact_file(lease->ack_path, &record, sizeof(record)) != 0) {
    if (errno == ENOENT) return 0;
    return -1;
  }
  if (!ack_valid(&record)) {
    errno = EINVAL;
    return -1;
  }
  if (record.token != lease->token) return 0;
  *pid_out = (pid_t)record.runtime_pid;
  return 1;
}

void gtav_supervisor_end(GtavSupervisorLease* lease, int clear_records) {
  if (lease == NULL) return;
  if (clear_records && lease->token != 0) {
    unlink_session_if_token(lease->session_path, lease->token);
    unlink_ack_if_token(lease->ack_path, lease->token);
  }
  if (lease->owner_fd >= 0) close(lease->owner_fd);
  lease->owner_fd = -1;
  if (lease->owner_path[0] != '\0') unlink(lease->owner_path);
}

int gtav_supervisor_runtime_attach(GtavSupervisorRuntimeLease* lease, const char* directory) {
  GtavSupervisorSessionRecord record;
  char expected_owner[GTAV_DAEMON_PATH_MAX];
  char owner_name[96];
  struct stat st;
  int count;
  if (lease == NULL || directory == NULL || lease->supervisor_fd >= 0) {
    errno = EINVAL;
    return -1;
  }
  if (make_path(lease->session_path, sizeof(lease->session_path), directory,
                "supervisor.session") != 0 ||
      make_path(lease->ack_path, sizeof(lease->ack_path), directory, "supervisor.ack") != 0 ||
      read_exact_file(lease->session_path, &record, sizeof(record)) != 0) {
    return -1;
  }
  if (!session_valid(&record)) {
    errno = EINVAL;
    return -1;
  }
  count = snprintf(owner_name, sizeof(owner_name), "supervisor.%d.%016llx.owner",
                   record.supervisor_pid, (unsigned long long)record.token);
  if (count < 0 || (size_t)count >= sizeof(owner_name) ||
      make_path(expected_owner, sizeof(expected_owner), directory, owner_name) != 0 ||
      strcmp(record.owner_path, expected_owner) != 0) {
    errno = EINVAL;
    return -1;
  }
  memcpy(lease->owner_path, record.owner_path, sizeof(lease->owner_path));
  lease->token = record.token;
  lease->supervisor_fd = open(lease->owner_path, O_RDWR | O_NOFOLLOW);
  if (lease->supervisor_fd < 0) return -1;
  if (fstat(lease->supervisor_fd, &st) != 0) {
    int saved_errno = errno;
    close(lease->supervisor_fd);
    lease->supervisor_fd = -1;
    errno = saved_errno;
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    close(lease->supervisor_fd);
    lease->supervisor_fd = -1;
    errno = EINVAL;
    return -1;
  }
  if (flock(lease->supervisor_fd, LOCK_EX | LOCK_NB) == 0) {
    flock(lease->supervisor_fd, LOCK_UN);
    close(lease->supervisor_fd);
    lease->supervisor_fd = -1;
    errno = ESRCH;
    return -1;
  }
  if (errno != EWOULDBLOCK && errno != EAGAIN) {
    int saved_errno = errno;
    close(lease->supervisor_fd);
    lease->supervisor_fd = -1;
    errno = saved_errno;
    return -1;
  }
  lease->supervisor_lost = 0;
  return 0;
}

int gtav_supervisor_runtime_alive(GtavSupervisorRuntimeLease* lease) {
  if (lease == NULL || lease->supervisor_fd < 0 || lease->supervisor_lost) return 0;
  if (flock(lease->supervisor_fd, LOCK_EX | LOCK_NB) == 0) {
    flock(lease->supervisor_fd, LOCK_UN);
    lease->supervisor_lost = 1;
    return 0;
  }
  if (errno == EWOULDBLOCK || errno == EAGAIN) return 1;
  lease->supervisor_lost = 1;
  return 0;
}

int gtav_supervisor_runtime_publish_ack(GtavSupervisorRuntimeLease* lease, pid_t runtime_pid) {
  GtavSupervisorAckRecord record;
  if (lease == NULL || runtime_pid <= 1 || !gtav_supervisor_runtime_alive(lease)) {
    errno = ESRCH;
    return -1;
  }
  memset(&record, 0, sizeof(record));
  record.magic = GTAV_SUPERVISOR_ACK_MAGIC;
  record.abi_version = GTAV_SUPERVISOR_LIFECYCLE_ABI;
  record.struct_size = sizeof(record);
  record.token = lease->token;
  record.runtime_pid = runtime_pid;
  return write_record_atomic(lease->ack_path, &record, sizeof(record), lease->token);
}

void gtav_supervisor_runtime_release(GtavSupervisorRuntimeLease* lease) {
  if (lease == NULL) return;
  if (lease->token != 0) {
    unlink_ack_if_token(lease->ack_path, lease->token);
    unlink_session_if_token(lease->session_path, lease->token);
  }
  if (lease->supervisor_fd >= 0) close(lease->supervisor_fd);
  lease->supervisor_fd = -1;
  if (lease->owner_path[0] != '\0') unlink(lease->owner_path);
}
