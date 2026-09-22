// Worker-thread teardown/suspend guard. See quit_guard.h. Drives the pure teardown_watchdog.h
// state machine from the live frame-hook heartbeat (and, optionally, the PS5 app-state background
// flag) and parks/unparks the scePad input path so the menu un-hooks itself before a force-close /
// suspend tears GTA down. Built into the worker; inert unless GTAV_MENU_ENABLE_QUIT_GUARD=1.

#define GTAV_LOG_DEFAULT_CATEGORY GTAV_LOG_CAT_MENU

#include "gtavmenu/quit_guard.h"

#include "gtavmenu/frame_hook.h"
#include "gtavmenu/log.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/status.h"
#include "gtavmenu/teardown_watchdog.h"

#include <stdint.h>
#include <string.h>

#ifndef GTAV_MENU_ENABLE_QUIT_GUARD
#define GTAV_MENU_ENABLE_QUIT_GUARD 0
#endif

// App-state listener: poll the PS5 system-service background flag (libSceSystemService) as a fast,
// explicit teardown/suspend signal on top of the frame-hook heartbeat fallback. Separately gated
// because the symbol + status-struct layout are firmware-specific and need in-game verification
// (the worker also links -lSceSystemService only when this is on).
#ifndef GTAV_MENU_QUIT_GUARD_APPSTATE
#define GTAV_MENU_QUIT_GUARD_APPSTATE 0
#endif

// Worker ticks of frame-hook silence before the game thread is presumed gone/dying. The worker
// runs ~60 Hz by default, so ~3 s. A force-close gives a short grace before SIGKILL; a long
// loading screen can also stall PLAYER_PED_ID, but a park there is harmless (it is reversible and
// the menu is usually closed) -- so this is deliberately a few seconds, not tens. Build-tunable.
#ifndef GTAV_MENU_QUIT_GUARD_STALE_TICKS
#define GTAV_MENU_QUIT_GUARD_STALE_TICKS 180u
#endif

#if GTAV_MENU_ENABLE_QUIT_GUARD

static GtavTeardownWatchdog g_watchdog;
static int g_watchdog_ready;

#if GTAV_MENU_QUIT_GUARD_APPSTATE
// libSceSystemService status getter. Layout per the Prospero SDK: a 32-bit eventNum followed by
// byte flags; isInBackgroundExecution is the one we read (a force-close plays its close animation
// with the app backgrounded, and rest mode suspends there too). Declared locally -- the codebase
// resolves Sony libs by extern, like scePad -- and over-sized so a firmware layout drift can never
// make us under-read. UNVERIFIED on this firmware: the first few reads are logged raw before the
// flag is acted on, so a wrong offset shows up in `logs --kernel` instead of silently mis-parking.
typedef struct {
  int32_t eventNum;
  uint8_t isSystemUiOverlaid;
  uint8_t isInBackgroundExecution;
  uint8_t isCpuMode7CpuMode;
  uint8_t isGameLiveStreamingOnAir;
  uint8_t isOutOfVrPlayArea;
  uint8_t reserved[128];
} GtavSceSystemServiceStatus;

extern int sceSystemServiceGetStatus(GtavSceSystemServiceStatus* status);

static uint32_t g_appstate_log_budget = 8u;  // raw-log the first few reads for HW validation
static int g_appstate_last_bg = -1;   // last acted-on bg flag (-1 = none yet) -> log on change
static int g_appstate_layout_ok = 1;  // cleared if the struct layout looks wrong (mis-offset)

static int quit_guard_appstate_signal(void) {
  GtavSceSystemServiceStatus st;
  memset(&st, 0, sizeof(st));
  if (sceSystemServiceGetStatus(&st) != 0) {
    return GTAV_TEARDOWN_SIGNAL_NONE;  // query failed -> defer to the heartbeat heuristic
  }
  if (g_appstate_log_budget) {
    --g_appstate_log_budget;
    gtav_logf("quit-guard appstate eventNum=%d ui=%u bg=%u stream=%u", st.eventNum,
              (unsigned)st.isSystemUiOverlaid, (unsigned)st.isInBackgroundExecution,
              (unsigned)st.isGameLiveStreamingOnAir);
  }
  // Layout sanity gate: these fields are SDK booleans, so any value > 1 means the firmware-specific
  // struct offset is wrong (the symbol/layout is UNVERIFIED on this firmware). A mis-read byte that
  // happened to be non-zero would spuriously PARK during normal play -- dropping input + rendering.
  // So if the layout looks wrong, stop trusting the app-state signal entirely (defer to the
  // heartbeat) and say so once, loudly, in the kernel log for HW validation.
  if (st.isInBackgroundExecution > 1u || st.isSystemUiOverlaid > 1u ||
      st.isGameLiveStreamingOnAir > 1u) {
    if (g_appstate_layout_ok) {
      g_appstate_layout_ok = 0;
      gtav_logf(
          "quit-guard appstate layout SUSPECT (bg=%u ui=%u stream=%u not boolean) -- "
          "ignoring app-state, heartbeat only",
          (unsigned)st.isInBackgroundExecution, (unsigned)st.isSystemUiOverlaid,
          (unsigned)st.isGameLiveStreamingOnAir);
    }
  }
  if (!g_appstate_layout_ok) {
    return GTAV_TEARDOWN_SIGNAL_NONE;
  }
  const int bg = st.isInBackgroundExecution ? 1 : 0;
  if (bg != g_appstate_last_bg) {
    // Log every transition (not just the first 8 reads) so a force-close / rest-mode capture shows
    // the exact moment the flag flips -- the signal that drives the suspend-safe park.
    g_appstate_last_bg = bg;
    gtav_logf("quit-guard appstate bg -> %d (eventNum=%d)", bg, st.eventNum);
  }
  return bg ? GTAV_TEARDOWN_SIGNAL_BACKGROUND : GTAV_TEARDOWN_SIGNAL_FOREGROUND;
}
#endif  // GTAV_MENU_QUIT_GUARD_APPSTATE

void gtav_quit_guard_reset(void) {
  gtav_teardown_watchdog_init(&g_watchdog);
  g_watchdog_ready = 1;
}

void gtav_quit_guard_tick(void) {
  if (!g_watchdog_ready) {
    gtav_quit_guard_reset();
  }

  int signal = GTAV_TEARDOWN_SIGNAL_NONE;
#if GTAV_MENU_QUIT_GUARD_APPSTATE
  signal = quit_guard_appstate_signal();
#endif

  const int hook_active = gtav_frame_hook_is_active();
  const uint32_t calls = gtav_frame_hook_call_count();
  const int decision = gtav_teardown_watchdog_step(&g_watchdog, hook_active, calls,
                                                   GTAV_MENU_QUIT_GUARD_STALE_TICKS, signal);
  if (decision == GTAV_TEARDOWN_WATCHDOG_PARK) {
    gtav_logf("quit-guard PARK: frame hook silent/backgrounded (calls=%u active=%d) -- un-hooking",
              calls, hook_active);
    gtav_status_eventf(GTAV_MENU_EVENT_SHUTDOWN, "quit-guard park calls=%u active=%d", calls,
                       hook_active);
    // Stand down BOTH the live input path AND the renderer. Parking input alone is not enough for a
    // clean suspend: a worker that keeps issuing DRAW_* natives keeps GTA's GPU graphics pipe
    // non-idle, so the OS app-suspend handshake times out and the console reports a fatal
    // SYSTEM_SUSPEND_BLOCK_TIMEOUT (0xa0024301) instead of suspending. Render park lets the GPU go
    // idle so a force-close / rest-mode suspend completes gracefully.
    gtav_native_bridge_park();
    gtav_pad_input_park();
  } else if (decision == GTAV_TEARDOWN_WATCHDOG_UNPARK) {
    gtav_logf("quit-guard UNPARK: game thread resumed (calls=%u) -- re-arming", calls);
    gtav_status_eventf(GTAV_MENU_EVENT_SHUTDOWN, "quit-guard unpark calls=%u", calls);
    gtav_native_bridge_unpark();
    gtav_pad_input_unpark();
  }
}

#else  // GTAV_MENU_ENABLE_QUIT_GUARD

void gtav_quit_guard_reset(void) {}
void gtav_quit_guard_tick(void) {}

#endif  // GTAV_MENU_ENABLE_QUIT_GUARD
