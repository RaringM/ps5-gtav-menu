#include "gtavmenu/daemon_control.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int make_path(char* path, size_t size, const char* directory, const char* name) {
  int n = snprintf(path, size, "%s/%s", directory, name);
  if (n < 0 || (size_t)n >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int open_lock(const char* path) {
  return open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0644);
}

static int request_stop(const GtavDaemonControl* control) {
  int fd = open_lock(control->stop_path);
  if (fd < 0) return -1;
  return close(fd);
}

// The old loader has only an mtime lease and removes daemon.lock on exit. Its
// marker is '1'. New loaders write '2' and hold daemon.owner until after cleanup;
// a '2' marker with an unlocked owner is therefore abandoned, regardless of age.
static int legacy_owner_active(const GtavDaemonControl* control) {
  struct stat st;
  char marker = 0;
  int fd = open(control->lock_path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT ? 0 : -1;
  ssize_t count = read(fd, &marker, 1);
  int rc = fstat(fd, &st);
  int saved_errno = errno;
  close(fd);
  if (count < 0 || rc != 0) {
    errno = saved_errno;
    return -1;
  }
  if (count == 1 && marker == '2') return 0;
  time_t now = time(NULL);
  long age = now == (time_t)-1 ? -1 : (now > st.st_mtime ? (long)(now - st.st_mtime) : 0);
  return !gtav_daemon_lock_is_stale(age, GTAV_MENU_DAEMON_STALE_SECONDS);
}

static int deadline_passed(const struct timespec* started, unsigned int timeout_ms) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
  int64_t elapsed_ns =
      (int64_t)(now.tv_sec - started->tv_sec) * 1000000000LL + now.tv_nsec - started->tv_nsec;
  if (elapsed_ns >= (int64_t)timeout_ms * 1000000LL) {
    errno = ETIMEDOUT;
    return -1;
  }
  return 0;
}

int gtav_daemon_control_claim(GtavDaemonControl* control, const char* directory,
                              unsigned int timeout_ms) {
  char startup_path[GTAV_DAEMON_PATH_MAX];
  char owner_path[GTAV_DAEMON_PATH_MAX];
  struct timespec started;
  if (control->owner_fd >= 0) {
    errno = EALREADY;
    return -1;
  }
  if (make_path(control->lock_path, sizeof(control->lock_path), directory, "daemon.lock") ||
      make_path(control->stop_path, sizeof(control->stop_path), directory, "daemon.stop") ||
      make_path(startup_path, sizeof(startup_path), directory, "daemon.startup") ||
      make_path(owner_path, sizeof(owner_path), directory, "daemon.owner") ||
      clock_gettime(CLOCK_MONOTONIC, &started) != 0)
    return -1;

  // Never unlink these flock files: replacing an inode would split ownership.
  int startup = open_lock(startup_path);
  if (startup < 0) return -1;
  if (flock(startup, LOCK_EX | LOCK_NB) != 0) {
    int saved_errno = errno;
    close(startup);
    errno = saved_errno;
    return -1;
  }
  int owner = open_lock(owner_path);
  int owns = 0;
  int requested = 0;
  if (owner < 0) goto failed;
  for (;;) {
    if (!owns) {
      if (flock(owner, LOCK_EX | LOCK_NB) == 0)
        owns = 1;
      else if (errno != EWOULDBLOCK && errno != EAGAIN)
        goto failed;
    }
    int legacy = owns ? legacy_owner_active(control) : 1;
    if (legacy < 0) goto failed;
    if (owns && !legacy) break;
    if (!requested) {
      if (request_stop(control) != 0) goto failed;
      requested = 1;
    }
    if (deadline_passed(&started, timeout_ms) != 0) goto failed;
    usleep(50000);
  }

  int marker = open(control->lock_path, O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0644);
  if (marker < 0) goto failed;
  ssize_t written = write(marker, "2", 1);
  int write_errno = errno;
  int closed = close(marker);
  if (written != 1 || closed != 0) {
    int saved_errno = written < 0 ? write_errno : (closed != 0 ? errno : EIO);
    unlink(control->lock_path);
    errno = saved_errno;
    goto failed;
  }
  if (unlink(control->stop_path) != 0 && errno != ENOENT) {
    int saved_errno = errno;
    unlink(control->lock_path);
    errno = saved_errno;
    goto failed;
  }
  control->owner_fd = owner;
  close(startup);
  return 0;

failed: {
  int saved_errno = errno;
  if (owner >= 0) close(owner);
  close(startup);
  errno = saved_errno;
  return -1;
}
}

int gtav_daemon_control_should_stop(const GtavDaemonControl* control) {
  struct stat st;
  if (stat(control->stop_path, &st) == 0) return 1;
  return errno != ENOENT;  // unreadable control state must not mean "continue"
}

void gtav_daemon_control_refresh(const GtavDaemonControl* control) {
  if (control->owner_fd < 0) return;
  int fd = open(control->lock_path, O_WRONLY | O_NOFOLLOW);
  if (fd < 0) return;
  ssize_t count = write(fd, "2", 1);
  (void)count;
  close(fd);
}

void gtav_daemon_control_release(GtavDaemonControl* control) {
  if (control->owner_fd < 0) return;
  unlink(control->lock_path);
  close(control->owner_fd);
  control->owner_fd = -1;
}
