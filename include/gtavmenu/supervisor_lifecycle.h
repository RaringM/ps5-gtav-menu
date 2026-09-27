#pragma once

// A process manager may stop its visible plugin without a cooperative callback. Managed delivery
// adapters can therefore supervise a separate runtime process. This file provides the file-backed
// lease that lets that runtime observe supervisor death and enter the loader's cooperative
// hook-retirement path instead of being killed while GTA is still patched.

#include "gtavmenu/daemon_lifecycle.h"

#include <stdint.h>
#include <sys/types.h>

#define GTAV_SUPERVISOR_SESSION_MAGIC 0x47545353u
#define GTAV_SUPERVISOR_ACK_MAGIC 0x47545341u
#define GTAV_SUPERVISOR_LIFECYCLE_ABI 1u

typedef struct {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t struct_size;
  uint64_t token;
  int32_t supervisor_pid;
  uint32_t reserved;
  char owner_path[GTAV_DAEMON_PATH_MAX];
} GtavSupervisorSessionRecord;

typedef struct {
  uint32_t magic;
  uint16_t abi_version;
  uint16_t struct_size;
  uint64_t token;
  int32_t runtime_pid;
  uint32_t reserved;
} GtavSupervisorAckRecord;

typedef struct {
  int owner_fd;
  uint64_t token;
  char owner_path[GTAV_DAEMON_PATH_MAX];
  char session_path[GTAV_DAEMON_PATH_MAX];
  char ack_path[GTAV_DAEMON_PATH_MAX];
} GtavSupervisorLease;

typedef struct {
  int supervisor_fd;
  int supervisor_lost;
  uint64_t token;
  char owner_path[GTAV_DAEMON_PATH_MAX];
  char session_path[GTAV_DAEMON_PATH_MAX];
  char ack_path[GTAV_DAEMON_PATH_MAX];
} GtavSupervisorRuntimeLease;

// Creates a unique, locked supervisor inode and atomically publishes the session record. Initialise
// the lease with {.owner_fd = -1}. Only call this after claiming the ordinary daemon-control gate.
int gtav_supervisor_begin(GtavSupervisorLease* lease, const char* directory);

// Reads the acknowledgement only when it belongs to this exact session. Returns 1 with pid_out set
// for a valid acknowledgement, 0 while none exists, and -1 for malformed/unreadable state.
int gtav_supervisor_ack(const GtavSupervisorLease* lease, pid_t* pid_out);

// Drops the lease. When clear_records is non-zero, session and acknowledgement records are removed
// only if they still carry this lease's token, so an old process cannot erase a newer session.
void gtav_supervisor_end(GtavSupervisorLease* lease, int clear_records);

// Attaches the helper runtime to the currently published session and proves the supervisor still
// owns the advertised inode. Initialise with {.supervisor_fd = -1}.
int gtav_supervisor_runtime_attach(GtavSupervisorRuntimeLease* lease, const char* directory);

// Publishes the exact runtime pid for the supervisor and validates the lease immediately before
// doing so. Returns 0 on success and -1 if the supervisor has already gone away.
int gtav_supervisor_runtime_publish_ack(GtavSupervisorRuntimeLease* lease, pid_t runtime_pid);

// Returns 1 while the original supervisor owns its unique lease, otherwise 0. Loss is sticky: a
// later supervisor cannot make an old runtime resume.
int gtav_supervisor_runtime_alive(GtavSupervisorRuntimeLease* lease);

// Closes the runtime's lease descriptor and removes only records belonging to its exact token.
void gtav_supervisor_runtime_release(GtavSupervisorRuntimeLease* lease);
