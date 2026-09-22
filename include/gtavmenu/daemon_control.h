#pragma once

#include "gtavmenu/daemon_lifecycle.h"

// Filesystem lifecycle only; independent of process discovery and game operations.
typedef struct {
  int owner_fd;
  char lock_path[GTAV_DAEMON_PATH_MAX];
  char stop_path[GTAV_DAEMON_PATH_MAX];
} GtavDaemonControl;

// Initialise with {.owner_fd = -1}. A bounded, cooperative replacement: on timeout
// the current owner keeps its lock and its stop request remains pending. No signals
// are sent to PIDs. Concurrent starters fail rather than replacing each other.
int gtav_daemon_control_claim(GtavDaemonControl* control, const char* directory,
                              unsigned int timeout_ms);
int gtav_daemon_control_should_stop(const GtavDaemonControl* control);
void gtav_daemon_control_refresh(const GtavDaemonControl* control);
void gtav_daemon_control_release(GtavDaemonControl* control);
