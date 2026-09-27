#include "gtavmenu/daemon_control.h"
#include "gtavmenu/runtime_config.h"
#include "gtavmenu/supervisor_lifecycle.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef GTAV_ETAHEN_PLUGIN_ID
#define GTAV_ETAHEN_PLUGIN_ID "GTAV00001"
#endif
#ifndef GTAV_ETAHEN_RUNTIME_ID
#define GTAV_ETAHEN_RUNTIME_ID "GTAV00002"
#endif
#ifndef GTAV_ETAHEN_RUNTIME_VERSION
#define GTAV_ETAHEN_RUNTIME_VERSION "1.00"
#endif

#define GTAV_ETAHEN_UTIL_SOCKET "/system_tmp/etaHEN_util_service"
#define GTAV_ETAHEN_RUNTIME_PATH GTAV_MENU_DEFAULT_DIR "/gtav-menu-etahen-runtime.plugin"
#define GTAV_ETAHEN_RUNTIME_PID_PATH "/system_tmp/" GTAV_ETAHEN_RUNTIME_ID ".PID"
#define GTAV_ETAHEN_HEADER_SIZE 29u
#define GTAV_ETAHEN_IPC_MAGIC 0xDEADBABEu
#define GTAV_ETAHEN_IPC_BUFFER_SIZE 0x1000u
#define GTAV_ETAHEN_LAUNCH_PLUGIN 0x08000007
#define GTAV_ETAHEN_RETURN_VALUE 0x08000002

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x20000
#endif

typedef struct {
  int32_t magic;
  int32_t command;
  int32_t error;
  char message[GTAV_ETAHEN_IPC_BUFFER_SIZE];
} GtavEtahenIpcMessage;

_Static_assert(sizeof(GtavEtahenIpcMessage) == 4108u, "etaHEN 2.5B IPC layout changed");

extern const uint8_t gtav_embedded_etahen_runtime_start[];
extern const uint8_t gtav_embedded_etahen_runtime_end[];

static volatile sig_atomic_t g_stopping;

static void request_stop(int signal_number) {
  (void)signal_number;
  g_stopping = 1;
}

static int write_all(int fd, const uint8_t* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t count = write(fd, data + written, size - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count == 0) errno = EIO;
      return -1;
    }
    written += (size_t)count;
  }
  return 0;
}

static int send_all(int fd, const void* data, size_t size) {
  const uint8_t* bytes = (const uint8_t*)data;
  size_t sent = 0;
  while (sent < size) {
    ssize_t count = send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count == 0) errno = EIO;
      return -1;
    }
    sent += (size_t)count;
  }
  return 0;
}

static int receive_all(int fd, void* data, size_t size) {
  uint8_t* bytes = (uint8_t*)data;
  size_t received = 0;
  while (received < size) {
    ssize_t count = recv(fd, bytes + received, size - received, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count == 0) errno = ECONNRESET;
      return -1;
    }
    received += (size_t)count;
  }
  return 0;
}

static int embedded_runtime_valid(const uint8_t* image, size_t size) {
  static const uint8_t magic[14] = {'e', 't', 'a', 'H', 'E', 'N', '_',
                                    'P', 'L', 'U', 'G', 'I', 'N', 0};
  return size > GTAV_ETAHEN_HEADER_SIZE + 4u && memcmp(image, magic, sizeof(magic)) == 0 &&
         memcmp(image + 14u, GTAV_ETAHEN_RUNTIME_ID "\0", 10u) == 0 &&
         memcmp(image + 24u, GTAV_ETAHEN_RUNTIME_VERSION "\0", 5u) == 0 &&
         memcmp(image + GTAV_ETAHEN_HEADER_SIZE,
                "\x7f"
                "ELF",
                4u) == 0;
}

static int stage_embedded_runtime(uint64_t token) {
  const uint8_t* image = gtav_embedded_etahen_runtime_start;
  size_t size = (size_t)(gtav_embedded_etahen_runtime_end - gtav_embedded_etahen_runtime_start);
  char temporary[GTAV_DAEMON_PATH_MAX];
  int count;
  int fd;
  int result = -1;
  int saved_errno;

  if (!embedded_runtime_valid(image, size)) {
    errno = ENOEXEC;
    return -1;
  }
  count = snprintf(temporary, sizeof(temporary), "%s.%d.%08x.tmp", GTAV_ETAHEN_RUNTIME_PATH,
                   getpid(), (unsigned int)token);
  if (count < 0 || (size_t)count >= sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
  if (fd < 0) return -1;
  if (write_all(fd, image, size) != 0 || fsync(fd) != 0) goto staged;
  if (close(fd) != 0) {
    fd = -1;
    goto staged;
  }
  fd = -1;
  if (rename(temporary, GTAV_ETAHEN_RUNTIME_PATH) == 0) result = 0;
staged:
  saved_errno = errno;
  if (fd >= 0) close(fd);
  if (result != 0) unlink(temporary);
  errno = saved_errno;
  return result;
}

// etaHEN 2.5B's utility daemon is the supported path that gives the helper its own payload_args
// and SDK kernel binding. A fork from the Toolbox-managed plugin would inherit invalid runtime
// state. The command is defined by stock etaHEN's msg.hpp and accepts this fixed JSON request.
static int launch_runtime_with_etahen(void) {
  static const char request_json[] = "{\"plugin_path\":\"" GTAV_ETAHEN_RUNTIME_PATH
                                     "\",\"title_id\":\"" GTAV_ETAHEN_RUNTIME_ID "\"}";
  struct sockaddr_un address;
  struct timeval timeout;
  GtavEtahenIpcMessage request;
  GtavEtahenIpcMessage response;
  int fd;
  int result = -1;

  memset(&request, 0, sizeof(request));
  request.magic = (int32_t)GTAV_ETAHEN_IPC_MAGIC;
  request.command = GTAV_ETAHEN_LAUNCH_PLUGIN;
  memcpy(request.message, request_json, sizeof(request_json));

  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeout.tv_sec = 30;
  timeout.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, GTAV_ETAHEN_UTIL_SOCKET, sizeof(GTAV_ETAHEN_UTIL_SOCKET));
  if (connect(fd, (struct sockaddr*)&address, SUN_LEN(&address)) == 0 &&
      send_all(fd, &request, sizeof(request)) == 0 &&
      receive_all(fd, &response, sizeof(response)) == 0) {
    if (response.magic == (int32_t)GTAV_ETAHEN_IPC_MAGIC &&
        response.command == GTAV_ETAHEN_RETURN_VALUE && response.error == 0) {
      result = 0;
    } else {
      errno = EPROTO;
    }
  }
  {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
  }
  return result;
}

static int watch_process(pid_t pid, int* process_kqueue) {
  struct kevent change;
  int kq = kqueue();
  if (kq < 0) return -1;
  EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
  if (kevent(kq, &change, 1, NULL, 0, NULL) != 0) {
    close(kq);
    return -1;
  }
  *process_kqueue = kq;
  return 0;
}

static int process_exited(int kq, int wait_ms) {
  struct kevent event;
  struct timespec timeout;
  int count;
  timeout.tv_sec = wait_ms / 1000;
  timeout.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
  do {
    count = kevent(kq, NULL, 0, &event, 1, &timeout);
  } while (count < 0 && errno == EINTR && !g_stopping);
  return count != 0;
}

static int wait_for_runtime_ack(const GtavSupervisorLease* lease, pid_t* runtime_pid) {
  int poll;
  for (poll = 0; poll < 600 && !g_stopping; ++poll) {
    int result = gtav_supervisor_ack(lease, runtime_pid);
    if (result != 0) return result > 0 ? 0 : -1;
    usleep(50000);
  }
  errno = g_stopping ? EINTR : ETIMEDOUT;
  return -1;
}

int main(void) {
  GtavDaemonControl gate = {.owner_fd = -1};
  GtavSupervisorLease supervisor = {.owner_fd = -1};
  pid_t runtime_pid = -1;
  int process_kqueue = -1;
  int result = 1;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, request_stop);
  signal(SIGINT, request_stop);
  mkdir("/data", 0777);
  mkdir(GTAV_MENU_DEFAULT_DIR, 0777);

  // Serialise replacement before asking stock etaHEN to launch GTAV00002. That daemon otherwise
  // SIGKILLs the pid from its stale PID file; cooperative ownership proves the prior runtime has
  // already exact-retired, and removing the stale file prevents that unsafe fallback.
  if (gtav_daemon_control_claim(&gate, GTAV_MENU_DEFAULT_DIR, 120000) != 0 || g_stopping) goto done;
  if (gtav_supervisor_begin(&supervisor, GTAV_MENU_DEFAULT_DIR) != 0 || g_stopping) goto done;
  if (stage_embedded_runtime(supervisor.token) != 0 || g_stopping) goto done;
  if (unlink(GTAV_ETAHEN_RUNTIME_PID_PATH) != 0 && errno != ENOENT) goto done;
  if (launch_runtime_with_etahen() != 0) goto done;

  // The helper is now waiting for this temporary daemon owner. Hand ownership over, then require
  // its token-matched acknowledgement before accepting a process id to watch.
  gtav_daemon_control_release(&gate);
  if (wait_for_runtime_ack(&supervisor, &runtime_pid) != 0 ||
      watch_process(runtime_pid, &process_kqueue) != 0) {
    goto done;
  }
  result = 0;
  while (!g_stopping && !process_exited(process_kqueue, 50)) {
  }

done:
  if (gate.owner_fd >= 0) gtav_daemon_control_release(&gate);
  // Closing this unique flock is the stop request. The helper notices it on its next poll, retires
  // the worker and loader-owned hooks, releases daemon ownership, and only then exits.
  gtav_supervisor_end(&supervisor, process_kqueue < 0);
  if (process_kqueue >= 0) close(process_kqueue);
  return result;
}
