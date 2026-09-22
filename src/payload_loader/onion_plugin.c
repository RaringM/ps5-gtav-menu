#include "gtavmenu/onion_plugin.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef GTAV_ONION_PLUGIN_VERSION
#define GTAV_ONION_PLUGIN_VERSION "1.01"
#endif

#ifndef GTAV_ONION_ELFLDR_PORT
#define GTAV_ONION_ELFLDR_PORT 9020
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x20000
#endif

const GtavOnionPluginDescriptor gtav_onion_plugin_descriptor __attribute__((
    used, retain, section(".onion_plugin"), aligned(8))) = {
    sizeof(GtavOnionPluginDescriptor),
    GTAV_ONION_PLUGIN_ABI_VERSION,
    GTAV_ONION_CAP_NOTIFY | GTAV_ONION_CAP_PROCESS | GTAV_ONION_CAP_INJECT | GTAV_ONION_CAP_KERNEL,
    GTAV_ONION_FLAG_LONG_RUNNING | GTAV_ONION_FLAG_STOP_SUPPORTED,
    "GTAV00001",
    GTAV_ONION_PLUGIN_VERSION,
    "GTAV Menu",
};

extern const uint8_t gtav_embedded_loader_start[];
extern const uint8_t gtav_embedded_loader_end[];

static volatile sig_atomic_t g_stopping;

static void request_stop(int signal_number) {
  (void)signal_number;
  g_stopping = 1;
}

static int send_all(int fd, const uint8_t* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    ssize_t count = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return -1;
    sent += (size_t)count;
  }
  return 0;
}

static pid_t read_spawn_pid(int fd) {
  char response[64];
  size_t used = 0;
  char* end = NULL;
  long parsed;

  while (used < sizeof(response) - 1) {
    ssize_t count = recv(fd, response + used, sizeof(response) - 1 - used, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return -1;
    used += (size_t)count;
    if (memchr(response, '\n', used) != NULL) break;
  }
  response[used] = '\0';
  if (used < 5 || memcmp(response, "OK ", 3) != 0 || response[used - 1] != '\n') return -1;

  errno = 0;
  parsed = strtol(response + 3, &end, 10);
  if (errno != 0 || end == response + 3 || parsed <= 1 || parsed > INT_MAX || *end != '\n' ||
      end[1] != '\0') {
    return -1;
  }
  return (pid_t)parsed;
}

// OnionHEN's private loopback loader owns the supported process-creation path. Sending the
// embedded daemon ELF through it is important: the new process receives its own payload_args and
// therefore its own valid SDK kernel R/W binding. A plain fork of the managed plugin does not
// establish that runtime and has caused both a false "no kernel R/W" result and console failure.
static pid_t spawn_embedded_loader(void) {
  const uint8_t* image = gtav_embedded_loader_start;
  size_t image_size = (size_t)(gtav_embedded_loader_end - gtav_embedded_loader_start);
  struct sockaddr_in address;
  struct timeval timeout;
  int fd;
  pid_t pid;

  if (image_size < 4 || image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
    return -1;
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  timeout.tv_sec = 120;
  timeout.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(GTAV_ONION_ELFLDR_PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0 ||
      send_all(fd, image, image_size) != 0) {
    close(fd);
    return -1;
  }

  // The server knows the ELF length from its section table. Closing only the write side makes the
  // request boundary explicit while preserving the exact-PID response channel.
  (void)shutdown(fd, SHUT_WR);
  pid = read_spawn_pid(fd);
  close(fd);
  return pid;
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

int main(void) {
  pid_t daemon_pid;
  int process_kqueue = -1;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, request_stop);
  signal(SIGINT, request_stop);

  daemon_pid = spawn_embedded_loader();
  if (daemon_pid <= 1 || watch_process(daemon_pid, &process_kqueue) != 0) return 1;

  while (!g_stopping && !process_exited(process_kqueue, 50)) {
  }

  if (g_stopping && !process_exited(process_kqueue, 0)) {
    // The kqueue registration still identifies the exact process returned by OnionHEN, avoiding a
    // signal to a recycled PID. The daemon defers the request across committed hook transactions.
    (void)kill(daemon_pid, SIGTERM);
  }
  close(process_kqueue);
  return 0;
}
