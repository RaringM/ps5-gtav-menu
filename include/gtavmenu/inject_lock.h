#pragma once

// Single source for the per-pid double-inject lock path.
//
// The loader claims the lock (open O_CREAT|O_EXCL) before mapping the worker, so a
// second loader run cannot double-load the same game process. A worker may remove it on
// clean shutdown only when all process mutations are actually reversible. The production
// external-frame-hook lane deliberately retains it: its eboot jump and mapped gateway outlive
// worker shutdown, so another inject must wait for a game relaunch/instance-token change. The
// loader and worker live in separate ELFs, so the filename format MUST come from one place or the
// two sides silently drift and the ownership policy stops matching the claim.

#include "gtavmenu/runtime_config.h"  // GTAV_MENU_DEFAULT_DIR

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Longest path is GTAV_MENU_DEFAULT_DIR + "/inject-" + up to 10 pid digits + ".claim".
#define GTAV_INJECT_LOCK_PATH_MAX 160

// Diagnostic lease ceiling. The injected worker refreshes its lock's mtime on a ~3s wall-clock
// heartbeat (gtav_menu_worker_tick), so age remains useful evidence in logs/status. It is NOT
// authority to reclaim a same/unknown live instance: a crashed or partially-mapped worker cannot be
// safely unmapped, and stacking another worker after an arbitrary timeout is worse than requiring a
// game relaunch. Reclaim requires definite process-instance evidence below. Keep this threshold for
// diagnostics and call-site compatibility.
#ifndef GTAV_INJECT_LOCK_STALE_SECONDS
#define GTAV_INJECT_LOCK_STALE_SECONDS 15
#endif

// Format the lock path for `pid` into `buf`. `pid` is the GAME process id (the loader's
// inject target; the worker's own getpid() while injected), not the loader's own pid.
static inline void gtav_inject_lock_path(char* buf, size_t len, int pid) {
  snprintf(buf, len, "%s/inject-%d.lock", GTAV_MENU_DEFAULT_DIR, pid);
}

// Per-pid transaction mutex used by current loaders to serialize stale-lock reclamation. It is held
// from claim until the mapping outcome is durably represented by the main lock. The mutex is never
// age-reclaimed: a loader crash leaves a fail-closed guard rather than allowing an ABA unlink race.
static inline void gtav_inject_claim_guard_path(char* buf, size_t len, int pid) {
  snprintf(buf, len, "%s/inject-%d.claim", GTAV_MENU_DEFAULT_DIR, pid);
}

// Pure staleness decision for an existing lock (no I/O -- host-testable).
//   target_is_live: 1 the lock's pid is still our running game, 0 it is gone or recycled to a
//                   different app, -1 unknown.
//   lock_age_seconds: seconds since the lock's last heartbeat (< 0 if unknown).
//   max_age_seconds:  the lease ceiling (e.g. GTAV_INJECT_LOCK_STALE_SECONDS).
//   instance_token_matches: 1 the lock's stored per-instance token matches the live game (or
//                   either side is unknown), 0 the token DIFFERS -- the same pid is now a different
//                   game instance (a fast relaunch reused the pid before the lease expired). A
//                   definite mismatch reclaims immediately. Pass 1 when no token is available: an
//                   unknown token fails closed exactly like a matching token.
// Returns 1 only when the old lock is definitely from another/dead process instance. Lock age alone
// never permits reclaim, including when liveness or either token is unknown.
static inline int gtav_inject_lock_is_stale(int target_is_live, long lock_age_seconds,
                                            long max_age_seconds, int instance_token_matches) {
  (void)lock_age_seconds;  // diagnostic evidence only; never sufficient authority to stack workers
  (void)max_age_seconds;
  if (target_is_live == 0) {
    return 1;  // the game pid is gone or recycled -> the lock is orphaned
  }
  if (instance_token_matches == 0) {
    return 1;  // same pid, but a different game instance now owns it -> the old lock is orphaned
  }
  // Same live instance, or incomplete evidence (unknown liveness/token): keep the lock regardless
  // of age. A crash/partial mapping needs a relaunch or a future verified-unmap path, not
  // re-inject.
  return 0;
}

// Lock-file layout: byte 0 is the transaction state, byte 1 is a heartbeat byte, and bytes 8..15
// hold a little-endian uint64 process-start token. QUARANTINED is written and verified at claim,
// before any remote allocation. A completely successful production inject changes it to FINALIZED;
// guarded ps5debug injection uses MANUAL_FINALIZED. Both routes use the same type-marked token, so
// a finalized record can be reclaimed after a definite process relaunch without either route
// needing the other's app-id source. A failed/partial transaction remains fail-closed even if the
// pid is recycled. The worker heartbeat writes only byte 1, so it cannot race finalization or
// clobber the token.
#define GTAV_INJECT_LOCK_HEARTBEAT_OFFSET 1
#define GTAV_INJECT_LOCK_TOKEN_OFFSET 8
#define GTAV_INJECT_LOCK_CONTENT_LEN 16
#define GTAV_INJECT_LOCK_STATE_QUARANTINED ((unsigned char)'Q')
#define GTAV_INJECT_LOCK_STATE_FINALIZED ((unsigned char)'I')
#define GTAV_INJECT_LOCK_STATE_MANUAL_FINALIZED ((unsigned char)'M')
#define GTAV_INJECT_LOCK_STATE_LEGACY ((unsigned char)'1')

// Encode the kernel's immutable pstats.p_start timeval as a nonzero, type-marked cross-route token.
// tv_usec needs 20 bits (<1,000,000); the remaining 43 payload bits hold tv_sec. The loader obtains
// it through an uncached allproc walk; the guarded ps5debug route uses the same wire encoding.
#define GTAV_INJECT_PROCESS_START_TOKEN_MARKER (1ull << 63)
#define GTAV_INJECT_PROCESS_START_TOKEN_SECONDS_MASK ((1ull << 43) - 1ull)
static inline uint64_t gtav_inject_process_start_token(int64_t seconds, int64_t microseconds) {
  if (seconds <= 0 || (uint64_t)seconds > GTAV_INJECT_PROCESS_START_TOKEN_SECONDS_MASK ||
      microseconds < 0 || microseconds >= 1000000) {
    return 0;
  }
  return GTAV_INJECT_PROCESS_START_TOKEN_MARKER | ((uint64_t)seconds << 20) |
         (uint64_t)microseconds;
}

// Protocol/source compatibility for the first guarded-manual implementation. New code should use
// the route-neutral names above; these aliases intentionally preserve the exact bit representation.
#define GTAV_INJECT_MANUAL_TOKEN_MARKER GTAV_INJECT_PROCESS_START_TOKEN_MARKER
#define GTAV_INJECT_MANUAL_TOKEN_SECONDS_MASK GTAV_INJECT_PROCESS_START_TOKEN_SECONDS_MASK
static inline uint64_t gtav_inject_manual_start_token(int64_t seconds, int64_t microseconds) {
  return gtav_inject_process_start_token(seconds, microseconds);
}

// Refresh the lock's mtime as a liveness heartbeat (called from the injected worker). Best
// effort: touches only an EXISTING lock (no O_CREAT, so a guard-off build stays lock-less) and
// ignores every error. `pid` is the worker's own getpid() (the game pid).
//
// Non-destructive: byte 1 is reserved for the heartbeat, so neither the transaction state at byte 0
// nor the token can suffer a stale read/write lost update. We avoid utime/futimens, which the live
// path deliberately never relied on, and bump mtime via a real write as before.
static inline void gtav_inject_lock_refresh(int pid) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  int fd;
  unsigned char b = 'H';
  ssize_t io;
  gtav_inject_lock_path(path, sizeof(path), pid);
  fd = open(path, O_WRONLY);  // existing lock only; no O_CREAT
  if (fd < 0) {
    return;
  }
  if (lseek(fd, GTAV_INJECT_LOCK_HEARTBEAT_OFFSET, SEEK_SET) == GTAV_INJECT_LOCK_HEARTBEAT_OFFSET) {
    io = write(fd, &b, 1);  // bumps mtime; state/token are disjoint
    (void)io;
  }
  close(fd);
}

// Read a complete lock record. Unknown states, short/old-corrupt records, and I/O failures return
// -1 so claim logic can keep the lock rather than infer ownership. The legacy '1' sentinel is
// accepted as FINALIZED for locks written by earlier builds.
static inline int gtav_inject_lock_read_record(const char* path, unsigned char* state_out,
                                               uint64_t* token_out) {
  int fd, i;
  unsigned char buf[GTAV_INJECT_LOCK_CONTENT_LEN];
  ssize_t got;
  uint64_t token = 0;
  if (path == NULL || state_out == NULL || token_out == NULL) {
    return -1;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  got = read(fd, buf, sizeof(buf));
  close(fd);
  if (got != (ssize_t)sizeof(buf) ||
      (buf[0] != GTAV_INJECT_LOCK_STATE_QUARANTINED && buf[0] != GTAV_INJECT_LOCK_STATE_FINALIZED &&
       buf[0] != GTAV_INJECT_LOCK_STATE_MANUAL_FINALIZED &&
       buf[0] != GTAV_INJECT_LOCK_STATE_LEGACY)) {
    return -1;
  }
  for (i = 0; i < 8; ++i) {
    token |= ((uint64_t)buf[GTAV_INJECT_LOCK_TOKEN_OFFSET + i]) << (8 * i);
  }
  *state_out = buf[0];
  *token_out = token;
  return 0;
}

// Loader-side: initialize an already O_EXCL-claimed lock as a verified QUARANTINED transaction
// bound to the game's current process-start token. A later loader may inspect the token for
// diagnostics, but it must never reclaim this state on mismatch. `pid` is the game pid. Returns 0
// on a complete verified write, -1 on any open/seek/short-I/O/readback failure.
static inline int gtav_inject_lock_write_token(int pid, uint64_t token) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  int fd, i;
  unsigned char buf[GTAV_INJECT_LOCK_CONTENT_LEN];
  unsigned char check[GTAV_INJECT_LOCK_CONTENT_LEN];
  ssize_t io;
  gtav_inject_lock_path(path, sizeof(path), pid);
  fd = open(path, O_RDWR);  // existing (claimed) lock only
  if (fd < 0) {
    return -1;
  }
  memset(buf, 0, sizeof(buf));
  buf[0] = GTAV_INJECT_LOCK_STATE_QUARANTINED;
  for (i = 0; i < 8; ++i) {
    buf[GTAV_INJECT_LOCK_TOKEN_OFFSET + i] = (unsigned char)((token >> (8 * i)) & 0xFFu);
  }
  if (lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return -1;
  }
  io = write(fd, buf, sizeof(buf));
  if (io != (ssize_t)sizeof(buf) || lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return -1;
  }
  io = read(fd, check, sizeof(check));
  close(fd);
  return io == (ssize_t)sizeof(check) && memcmp(check, buf, sizeof(buf)) == 0 ? 0 : -1;
}

// Publish a successful transaction as FINALIZED. The state byte is written last, after verifying
// the existing complete QUARANTINED record and its token, so any earlier or partial failure remains
// non-reclaimable. A one-byte state write that lands despite a readback failure is still safe: the
// caller invokes this only after the mapping/broker completed for the verified token. Returns 0 on
// verified finalization, -1 otherwise.
static inline int gtav_inject_lock_finalize(int pid, uint64_t expected_token) {
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  unsigned char state = 0;
  unsigned char final_state = GTAV_INJECT_LOCK_STATE_FINALIZED;
  uint64_t token = 0;
  int fd;
  ssize_t io;
  gtav_inject_lock_path(path, sizeof(path), pid);
  if (gtav_inject_lock_read_record(path, &state, &token) != 0 ||
      state != GTAV_INJECT_LOCK_STATE_QUARANTINED || token != expected_token) {
    return -1;
  }
  fd = open(path, O_RDWR);
  if (fd < 0) {
    return -1;
  }
  if (lseek(fd, 0, SEEK_SET) != 0 || write(fd, &final_state, 1) != 1 ||
      lseek(fd, 0, SEEK_SET) != 0) {
    close(fd);
    return -1;
  }
  io = read(fd, &state, 1);
  close(fd);
  return io == 1 && state == GTAV_INJECT_LOCK_STATE_FINALIZED ? 0 : -1;
}

// Loader-side: read the per-instance token a prior loader stamped, or 0 if none / unreadable / an
// old short-format lock. `path` is a formatted lock path (gtav_inject_lock_path).
static inline uint64_t gtav_inject_lock_read_token(const char* path) {
  unsigned char state = 0;
  uint64_t token = 0;
  return gtav_inject_lock_read_record(path, &state, &token) == 0 ? token : 0;
}
