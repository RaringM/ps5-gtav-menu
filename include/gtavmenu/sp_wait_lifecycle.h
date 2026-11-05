#pragma once

/* Pure decisions of the loader's player-world readiness wait (src/payload_loader/main.c
 * wait_for_sp_ready): no I/O, host-testable like gtavmenu/daemon_lifecycle.h.
 *
 * A pid the wait bound to can die or be recycled while every read still succeeds (the reads then
 * land in another process, or in a relaunched game whose world is still loading), so the wait
 * re-proves its target on a fixed cadence whatever the reads did, and a one-shot watch gives up
 * after a finite time instead of waiting forever on a target that never becomes ready. */

#include <stdint.h>

/* How often a wait re-proves its target, in waited time. */
#define GTAV_SP_WAIT_RECHECK_USEC 2000000UL

/* One-shot watch: how long a bound wait may last before the loader exits without injecting
 * (0 = no limit). A cold Story Mode load takes two to three minutes. The persistent daemon re-arms
 * on its own and has no limit. */
#ifndef GTAV_PAYLOAD_ONESHOT_SP_WATCHDOG_SEC
#define GTAV_PAYLOAD_ONESHOT_SP_WATCHDOG_SEC 1200UL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the wait has run a full period since the last recheck (a period of 0 rechecks every
   poll). */
static inline int gtav_sp_wait_recheck_due(unsigned long waited_us, unsigned long last_check_us,
                                           unsigned long period_us) {
  return period_us == 0 || waited_us < last_check_us || waited_us - last_check_us >= period_us;
}

/* 1 while the target is still the instance the wait bound to. `live` is the foreground probe (the
   title still owns this pid); a definite token change (both known and different) is a recycled pid.
   An unknown token on either side falls back to the probe alone. */
static inline int gtav_sp_wait_same_instance(int live, uint64_t bound_token,
                                             uint64_t current_token) {
  if (!live) return 0;
  if (bound_token != 0 && current_token != 0 && bound_token != current_token) return 0;
  return 1;
}

/* 1 once a finite limit has passed (0 = no limit). */
static inline int gtav_sp_wait_expired(unsigned long waited_us, unsigned long limit_us) {
  return limit_us != 0 && waited_us >= limit_us;
}

#ifdef __cplusplus
}
#endif
