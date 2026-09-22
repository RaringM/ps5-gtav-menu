#pragma once

// Pure, host-testable teardown watchdog.
//
// The live menu leaves two hooks inside GTA with no teardown trigger on a force-close: the
// PLAYER_PED_ID .text jump (frame_hook) and the scePadReadState GOT swap (pad_input). On a
// graceful Stop Runtime the worker restores them (gtav_menu_shutdown); on a force-close it
// never runs, so the game can keep calling into the (about-to-unmap) worker. This watchdog
// gives the worker a chance to *park* -- defensively restore the scePad GOT and stand down --
// BEFORE the process is torn down, then *unpark* (re-install) if the game comes back.
//
// The signal is the frame hook's own per-fire counter (gtav_frame_hook_call_count): it climbs
// once per game-thread call of the hooked native, so it is a heartbeat of the game/script
// thread. If it stops climbing for a while AND the hook is still active AND it had been firing,
// the game thread is gone or dying (force-close in progress, or a deep hang) -- park. When it
// starts climbing again (a long load finished, or the app resumed) -- unpark. Parking is
// reversible on purpose, so a generous-but-not-glacial threshold is safe: a false park during a
// long loading screen only drops input suppression for a beat, then unparks.
//
// An optional external app-state signal (the worker polling the PS5 system-service background
// flag) can force a park/unpark immediately; it wins over the heartbeat heuristic. The pure
// state machine below is identical whether the trip came from the counter or the app state, so
// it is fully unit-testable without any PS5 library.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_TEARDOWN_WATCHDOG_NOCHANGE 0
#define GTAV_TEARDOWN_WATCHDOG_PARK 1
#define GTAV_TEARDOWN_WATCHDOG_UNPARK 2

// External app-state hint passed to gtav_teardown_watchdog_step():
#define GTAV_TEARDOWN_SIGNAL_NONE 0        // no app-state signal -- let the heartbeat decide
#define GTAV_TEARDOWN_SIGNAL_BACKGROUND 1  // app went to background/suspend -> park now
#define GTAV_TEARDOWN_SIGNAL_FOREGROUND 2  // app is foreground again -> unpark now

typedef struct GtavTeardownWatchdog {
  uint32_t last_count;   // last observed frame-hook call_count
  uint32_t stale_ticks;  // consecutive worker ticks with no new hook fire (saturating)
  uint8_t ever_live;     // the hook fired at least once, so silence is meaningful
  uint8_t parked;        // we have parked (GOT restored) and are awaiting resume
} GtavTeardownWatchdog;

static inline void gtav_teardown_watchdog_init(GtavTeardownWatchdog* w) {
  w->last_count = 0u;
  w->stale_ticks = 0u;
  w->ever_live = 0u;
  w->parked = 0u;
}

static inline int gtav_teardown_watchdog_is_parked(const GtavTeardownWatchdog* w) {
  return w->parked != 0u;
}

// Step the watchdog once per worker tick. Returns PARK / UNPARK / NOCHANGE (the *transition*,
// so the caller acts only on the edge).
//   hook_active:      the frame hook is installed/active. When 0, silence is expected (Stop
//                     Runtime / never installed), so the heartbeat heuristic never parks.
//   cur_count:        current frame-hook call_count (climbs once per game-thread fire).
//   stale_threshold:  worker ticks of silence before the game thread is presumed gone. 0
//                     disables the heartbeat heuristic (external signal only).
//   signal:           GTAV_TEARDOWN_SIGNAL_* -- an app-state override, or NONE.
static inline int gtav_teardown_watchdog_step(GtavTeardownWatchdog* w, int hook_active,
                                              uint32_t cur_count, uint32_t stale_threshold,
                                              int signal) {
  const int fired = (cur_count != w->last_count);
  if (fired) {
    w->last_count = cur_count;
    w->stale_ticks = 0u;
    w->ever_live = 1u;
  } else if (w->stale_ticks != 0xFFFFFFFFu) {
    w->stale_ticks++;
  }

  // An explicit app-state signal is authoritative: a backgrounded/suspended app is about to be
  // torn down (or frozen), so park regardless of what the counter says; a foreground app may
  // resume immediately, so unpark.
  if (signal == GTAV_TEARDOWN_SIGNAL_BACKGROUND) {
    if (!w->parked) {
      w->parked = 1u;
      return GTAV_TEARDOWN_WATCHDOG_PARK;
    }
    return GTAV_TEARDOWN_WATCHDOG_NOCHANGE;
  }
  if (signal == GTAV_TEARDOWN_SIGNAL_FOREGROUND) {
    if (w->parked) {
      w->parked = 0u;
      w->stale_ticks = 0u;
      return GTAV_TEARDOWN_WATCHDOG_UNPARK;
    }
    return GTAV_TEARDOWN_WATCHDOG_NOCHANGE;
  }

  // No external signal: the frame-hook heartbeat decides.
  if (w->parked) {
    // Only the demonstrable return of the game thread (a fresh fire) lifts a park.
    if (fired) {
      w->parked = 0u;
      return GTAV_TEARDOWN_WATCHDOG_UNPARK;
    }
    return GTAV_TEARDOWN_WATCHDOG_NOCHANGE;
  }
  if (hook_active && w->ever_live && stale_threshold != 0u && w->stale_ticks >= stale_threshold) {
    w->parked = 1u;
    return GTAV_TEARDOWN_WATCHDOG_PARK;
  }
  return GTAV_TEARDOWN_WATCHDOG_NOCHANGE;
}

#ifdef __cplusplus
}
#endif
