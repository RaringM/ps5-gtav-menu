#pragma once

// Lifecycle helpers for the persistent re-inject daemon (`watch --persist` /
// GTAV_PAYLOAD_PERSISTENT). The one-shot loader injects once and exits; the persistent daemon
// stays resident and re-injects on every GTA relaunch. That resident loop needs two things the
// one-shot lane never did:
//   * a way to be TOLD to stop. `menu-ctl stop` signals the injected WORKER. The host drops a
//     sentinel to request full resident retirement: the daemon stops the worker if needed, verifies
//     its in-process callback restoration, exact-restores the loader-owned frame detour, and exits.
//   * a SINGLE-INSTANCE guard, so a second `watch --persist` deploy doesn't leave two daemons both
//     racing to inject (harmless to GTA thanks to the per-pid inject lock, but they would consume a
//     single stop request non-deterministically -- one stop would only kill one daemon).
//
// Both are file-based under GTAV_MENU_DEFAULT_DIR so the host (menu-ctl over FTP) and the daemon
// agree without any IPC channel:
//   * stop sentinel (daemon.stop): host drops it; the daemon begins cooperative retirement at its
//     next poll tick and exits only after cleanup succeeds or the exact game instance disappears.
//     It is NOT deleted by the daemon on sight -- so if two daemons ever coexist BOTH see it -- and
//     is cleared once by a freshly-started daemon instead.
//   * daemon marker (daemon.lock): retained and heartbeated for older loaders. New loaders use
//     a lifetime flock on daemon.owner plus a startup flock on daemon.startup. A second deploy
//     requests cooperative shutdown and waits for ownership (daemon_control.h).
//
// As in inject_lock.h, the pure decisions (same-instance, staleness) are header-only inlines so
// they are host-testable; filesystem ownership lives in src/common/daemon_control.c and is tested
// with real host processes independently of the PS5 loader.

#include "gtavmenu/runtime_config.h"  // GTAV_MENU_DEFAULT_DIR

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Longest path is GTAV_MENU_DEFAULT_DIR + "/daemon.stop" (or "/daemon.lock").
#define GTAV_DAEMON_PATH_MAX 160

// Compatibility lease for a legacy loader's '1' marker. New loaders' '2' markers rely on an
// advisory owner lock instead: age alone can never evict a live new loader.
#ifndef GTAV_MENU_DAEMON_STALE_SECONDS
#define GTAV_MENU_DAEMON_STALE_SECONDS 30
#endif

// Sentinel the host drops (over FTP) to ask a resident daemon to exit.
static inline void gtav_daemon_stop_path(char* buf, size_t len) {
  snprintf(buf, len, "%s/daemon.stop", GTAV_MENU_DEFAULT_DIR);
}

// Single-instance lock the resident daemon holds while running.
static inline void gtav_daemon_lock_path(char* buf, size_t len) {
  snprintf(buf, len, "%s/daemon.lock", GTAV_MENU_DEFAULT_DIR);
}

// Pure re-arm decision (no I/O -- host-testable). Returns 1 when the currently-foreground GTA is
// the SAME instance the daemon already served (keep idling), 0 when it is a fresh instance to
// inject.
//
// Keying on the raw pid alone is wrong: if the console recycles the pid for the relaunched game
// (served_pid == cur_pid but a genuinely new instance), a pid-only check idles forever and never
// re-injects. The per-instance token (the game's app id, gtav_proc_app_id) disambiguates, exactly
// as the inject lock uses it. An unknown token on either side (0) falls back to pid-only equality,
// so behaviour is never worse than the pre-token logic.
static inline int gtav_daemon_same_instance(int cur_pid, uint64_t cur_token, int served_pid,
                                            uint64_t served_token) {
  if (served_pid < 0 || cur_pid != served_pid) {
    return 0;  // nothing served yet, or a different pid -> a new instance
  }
  if (served_token != 0 && cur_token != 0 && served_token != cur_token) {
    return 0;  // same pid, but a DEFINITE token difference -> the pid was recycled to a new game
  }
  return 1;  // same pid and (matching or unknown) token -> still the instance we served
}

// Pure staleness decision for an existing daemon lock (no I/O -- host-testable). A lock whose last
// heartbeat (mtime) is older than the lease is treated as abandoned by a crashed/killed daemon and
// may be reclaimed; a fresh lock means a live daemon still owns it (refuse the second deploy).
//   lock_age_seconds: seconds since the lock's mtime (< 0 if it cannot be stat'd -> treated as
//   fresh
//                     so a transient stat error never steals a live daemon's lock).
//   max_age_seconds:  the lease ceiling (<= 0 disables expiry).
// Returns 1 if the lock is stale and may be reclaimed, 0 if a live daemon still holds it.
static inline int gtav_daemon_lock_is_stale(long lock_age_seconds, long max_age_seconds) {
  return lock_age_seconds >= 0 && max_age_seconds > 0 && lock_age_seconds > max_age_seconds;
}
