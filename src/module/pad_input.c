#include "gtavmenu/pad_input.h"

#include "gtavmenu/abi.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/status.h"

#include <stddef.h>
#include <string.h>
#include <unistd.h>

#ifndef GTAV_MENU_ENABLE_PAD_INPUT
#define GTAV_MENU_ENABLE_PAD_INPUT 0
#endif

// Dpad hold auto-repeat cadence, in poll ticks. Defaults are deliberate: a
// short settle delay before the first repeat, then a steady scroll. Both are
// overridable at build time.
#ifndef GTAV_MENU_PAD_REPEAT_DELAY_TICKS
#define GTAV_MENU_PAD_REPEAT_DELAY_TICKS 14u
#endif
#ifndef GTAV_MENU_PAD_REPEAT_RATE_TICKS
#define GTAV_MENU_PAD_REPEAT_RATE_TICKS 4u
#endif
// Auto-repeat acceleration: the longer a direction is held, the shorter the effective repeat
// interval, so long single-direction runs through a big list speed up. The interval sheds one
// tick every ACCEL_TICKS polls past the initial delay, down to RATE_MIN (fastest sustained).
#ifndef GTAV_MENU_PAD_REPEAT_RATE_MIN
#define GTAV_MENU_PAD_REPEAT_RATE_MIN 1u
#endif
#ifndef GTAV_MENU_PAD_REPEAT_ACCEL_TICKS
#define GTAV_MENU_PAD_REPEAT_ACCEL_TICKS 30u
#endif
// L1/R1 double-tap window (polls) for the HOME/END jump, and the Circle hold (polls) for the
// BACK_ROOT collapse. At ~60 Hz polling, 18 ~= 300 ms (double-tap) and 24 ~= 400 ms (hold).
#ifndef GTAV_MENU_PAD_DOUBLE_TAP_POLLS
#define GTAV_MENU_PAD_DOUBLE_TAP_POLLS 18u
#endif
#ifndef GTAV_MENU_PAD_CIRCLE_HOLD_POLLS
#define GTAV_MENU_PAD_CIRCLE_HOLD_POLLS 24u
#endif

// Active keybind table (combo -> action). Set via gtav_pad_input_set_binds and read by both
// the hotkey detection in gtav_pad_input_map and the game-input suppression hook. File-local
// so it is shared across both builds (the suppression hook is live-only). g_pad_hotkey_action
// latches the last fired action for the live poll path's accessor.
static GtavKeybind g_binds[GTAV_KEYBIND_SLOTS];
static uint32_t g_bind_count;
static uint32_t g_pad_hotkey_action;

void gtav_pad_input_set_binds(const GtavKeybind* binds, uint32_t count) {
  if (count > GTAV_KEYBIND_SLOTS) count = GTAV_KEYBIND_SLOTS;
  for (uint32_t i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
    if (binds && i < count) {
      g_binds[i] = binds[i];
    } else {
      g_binds[i].combo_mask = 0u;
      g_binds[i].action = 0u;
    }
  }
  g_bind_count = count;
}

uint32_t gtav_pad_input_last_hotkey_action(void) {
  return g_pad_hotkey_action;
}

// Runtime-overridable D-pad auto-repeat cadence (poll ticks), seeded from the build defaults. The
// Menu Settings Nav Delay / Nav Speed cyclers set these via gtav_pad_input_set_repeat so the scroll
// cadence is tunable live (and persisted through the profile) without a rebuild. Read by the live
// poll path; the pure mapper still takes delay/rate as parameters so host tests stay explicit.
static uint32_t g_repeat_delay = GTAV_MENU_PAD_REPEAT_DELAY_TICKS;
static uint32_t g_repeat_rate = GTAV_MENU_PAD_REPEAT_RATE_TICKS;

void gtav_pad_input_set_repeat(uint32_t delay_ticks, uint32_t rate_ticks) {
  g_repeat_delay = delay_ticks;
  g_repeat_rate = rate_ticks;
}

// Touchpad gesture thresholds in raw touch units. PROVISIONAL: the DualSense touch coordinate range
// (assumed y ~[0,1080]) and the finger-down encoding must be confirmed on PS5 hardware; only the
// scale of these constants changes after that, not the mapper logic. TODO-calibrate-on-HW.
#ifndef GTAV_MENU_TOUCH_STEP_PX
#define GTAV_MENU_TOUCH_STEP_PX 80  // vertical travel per cursor row-step
#endif
#ifndef GTAV_MENU_TOUCH_FLICK_PX
#define GTAV_MENU_TOUCH_FLICK_PX 220  // single-poll vertical delta that pages instead of stepping
#endif
#ifndef GTAV_MENU_TOUCH_TAP_PX
#define GTAV_MENU_TOUCH_TAP_PX 28  // max total travel for a contact to count as a tap (-> SELECT)
#endif

uint32_t gtav_pad_input_map_touch(GtavPadTouchMapState* state, int x, int y, int finger_down,
                                  int clicked, int enabled, int visible) {
  (void)x;  // horizontal gestures reserved for a later pass (cycler adjust); v1 is vertical + tap
  if (!state) return GTAV_MENU_COMMAND_NONE;
  if (!enabled || !visible) {
    state->active = 0;
    state->prev_finger = 0u;
    state->prev_click = 0u;
    state->accum_y = 0;
    state->travel = 0;
    return GTAV_MENU_COMMAND_NONE;
  }

  // Physical touchpad click -> SELECT, edge-only (a firm press, distinct from a light touch).
  if (clicked && !state->prev_click) {
    state->prev_click = 1u;
    state->prev_finger = finger_down ? 1u : 0u;
    return GTAV_MENU_COMMAND_SELECT;
  }
  state->prev_click = clicked ? 1u : 0u;

  if (finger_down && !state->prev_finger) {
    // Contact start: begin tracking; classify (tap vs swipe) on release / as travel accrues.
    state->active = 1;
    state->last_y = y;
    state->accum_y = 0;
    state->travel = 0;
    state->prev_finger = 1u;
    return GTAV_MENU_COMMAND_NONE;
  }

  if (finger_down && state->active) {
    const int dy = y - state->last_y;
    state->last_y = y;
    const int ady = dy < 0 ? -dy : dy;
    state->travel += ady;
    state->prev_finger = 1u;
    if (ady >= GTAV_MENU_TOUCH_FLICK_PX) {  // fast flick -> page
      state->accum_y = 0;
      return dy > 0 ? GTAV_MENU_COMMAND_PAGE_NEXT : GTAV_MENU_COMMAND_PAGE_PREV;
    }
    state->accum_y += dy;
    if (state->accum_y >= GTAV_MENU_TOUCH_STEP_PX) {
      state->accum_y -= GTAV_MENU_TOUCH_STEP_PX;
      return GTAV_MENU_COMMAND_NEXT;
    }
    if (state->accum_y <= -GTAV_MENU_TOUCH_STEP_PX) {
      state->accum_y += GTAV_MENU_TOUCH_STEP_PX;
      return GTAV_MENU_COMMAND_PREV;
    }
    return GTAV_MENU_COMMAND_NONE;
  }

  if (!finger_down && state->prev_finger) {
    // Contact end: a tap (barely moved) selects; a swipe already emitted its steps.
    const int was_tap = state->active && state->travel <= GTAV_MENU_TOUCH_TAP_PX;
    state->active = 0;
    state->prev_finger = 0u;
    state->accum_y = 0;
    return was_tap ? GTAV_MENU_COMMAND_SELECT : GTAV_MENU_COMMAND_NONE;
  }

  state->prev_finger = finger_down ? 1u : 0u;
  return GTAV_MENU_COMMAND_NONE;
}

uint32_t gtav_pad_input_map(GtavPadMapState* state, uint32_t buttons, int visible,
                            uint32_t repeat_delay, uint32_t repeat_rate, int lr_repeat_ok) {
  if (!state) {
    return GTAV_MENU_COMMAND_NONE;
  }

  const uint32_t prev = state->prev_buttons;
  const uint32_t just = buttons & ~prev;
  state->prev_buttons = buttons;
  // Monotonic poll clock for the L1/R1 double-tap timing. Incremented once per call on every path
  // (it is only a clock); stored timestamps are therefore always >= 1, so 0 reliably means "none".
  ++state->poll_seq;

  // Open/close chord: R1 + DpadLeft, in either press order. Latch it so holding
  // the chord emits a single TOGGLE instead of one per poll, and suppress
  // navigation while the chord is down so DpadLeft does not leak into nav.
  const int chord = (buttons & GTAV_PAD_OPEN_CHORD_MASK) == GTAV_PAD_OPEN_CHORD_MASK;
  if (chord) {
    if (state->chord_latch) {
      // The chord is still held after its decision was made: suppress (no repeat TOGGLE, and
      // keep DpadLeft off nav).
      state->repeat_bit = 0;
      state->repeat_poll = 0;
      return GTAV_MENU_COMMAND_NONE;
    }
    state->chord_latch = 1;  // decide once per chord-hold, either way
    // While the menu is OPEN, distinguish a deliberate close from "R1 still held from paging + a
    // Left tap on a value cycler": if R1 was already held last poll and Left is the bit that just
    // arrived, route it to the value-adjust path (fall through to COMMAND_LEFT below) instead of
    // closing the menu mid-interaction. A deliberate close is R1+Left pressed together, or Left
    // held then R1 completing the chord -- both still TOGGLE. While closed (visible == 0) there is
    // no paging to protect, so the chord always opens, in either press order, exactly as before.
    const int accidental_cycle = visible && (prev & GTAV_PAD_BTN_R1) && (just & GTAV_PAD_BTN_LEFT);
    if (!accidental_cycle) {
      state->repeat_bit = 0;
      state->repeat_poll = 0;
      return GTAV_MENU_COMMAND_TOGGLE;
    }
    // accidental_cycle: fall through; the bare DpadLeft becomes COMMAND_LEFT in the nav section.
  } else {
    state->chord_latch = 0;
  }

  if (!visible) {
    state->repeat_bit = 0;
    state->repeat_poll = 0;
    // Keybinds: a bound combo held in full fires its action once per press while the menu is
    // closed. The open chord was already handled above, so it can never also fire a hotkey;
    // matching only here (closed) means a bound combo never steals navigation. First match
    // wins; combos are curated to be multi-button and to avoid the open chord.
    uint32_t fired = 0u;
    uint32_t i;
    for (i = 0; i < g_bind_count && i < GTAV_KEYBIND_SLOTS; ++i) {
      const uint32_t m = g_binds[i].combo_mask;
      if (m && (buttons & m) == m) {
        fired = m;
        state->hotkey_action = g_binds[i].action;
        break;
      }
    }
    if (fired) {
      if (state->hotkey_latch != fired) {
        state->hotkey_latch = fired;
        g_pad_hotkey_action = state->hotkey_action;
        return GTAV_MENU_COMMAND_HOTKEY;
      }
      return GTAV_MENU_COMMAND_NONE;
    }
    state->hotkey_latch = 0u;
    return GTAV_MENU_COMMAND_NONE;
  }

  // Dpad up/down/left/right auto-repeat while held. The initial press still fires
  // through the edge checks below; this only adds the repeated fires. Left/Right
  // here are bare D-pad (the R1+Left open chord was already handled above), so they
  // become menu value-adjust commands. Suppression keeps them off the game.
  const uint32_t dir_mask =
      GTAV_PAD_BTN_UP | GTAV_PAD_BTN_DOWN | GTAV_PAD_BTN_LEFT | GTAV_PAD_BTN_RIGHT;
  const uint32_t dir_held = buttons & dir_mask;
  const uint32_t dir_just = just & dir_mask;
  if (dir_just) {
    state->repeat_bit = dir_just;
    state->repeat_poll = 0;
  } else if (state->repeat_bit && (dir_held & state->repeat_bit)) {
    ++state->repeat_poll;
    if (repeat_delay && state->repeat_poll >= repeat_delay) {
      const uint32_t since = state->repeat_poll - repeat_delay;
      // Accelerate: shrink the effective interval the longer the dir is held (rate -> RATE_MIN).
      // Integer-only and monotone; at most a +/-1 row phase shift at a rate boundary, which is
      // imperceptible mid-scroll. With the default ACCEL window (30) the short-horizon repeat
      // tests never reach the first step, so they keep the configured rate.
      uint32_t eff = repeat_rate;
      if (eff > GTAV_MENU_PAD_REPEAT_RATE_MIN) {
        const uint32_t shed = since / GTAV_MENU_PAD_REPEAT_ACCEL_TICKS;
        eff = (shed >= eff - GTAV_MENU_PAD_REPEAT_RATE_MIN) ? GTAV_MENU_PAD_REPEAT_RATE_MIN
                                                            : eff - shed;
      }
      if (eff == 0u || (since % eff) == 0u) {
        if (state->repeat_bit & GTAV_PAD_BTN_UP) return GTAV_MENU_COMMAND_PREV;
        if (state->repeat_bit & GTAV_PAD_BTN_DOWN) return GTAV_MENU_COMMAND_NEXT;
        // Left/Right repeat only on value cyclers; elsewhere Left is a one-shot Back
        // (the initial press still fires through the edge checks below).
        if (lr_repeat_ok && (state->repeat_bit & GTAV_PAD_BTN_LEFT)) return GTAV_MENU_COMMAND_LEFT;
        if (lr_repeat_ok && (state->repeat_bit & GTAV_PAD_BTN_RIGHT))
          return GTAV_MENU_COMMAND_RIGHT;
      }
    }
  } else {
    state->repeat_bit = 0;
    state->repeat_poll = 0;
  }

  if (just & GTAV_PAD_BTN_UP) return GTAV_MENU_COMMAND_PREV;
  if (just & GTAV_PAD_BTN_DOWN) return GTAV_MENU_COMMAND_NEXT;
  if (just & GTAV_PAD_BTN_LEFT) return GTAV_MENU_COMMAND_LEFT;
  if (just & GTAV_PAD_BTN_RIGHT) return GTAV_MENU_COMMAND_RIGHT;
  // R3 (right-stick click) pins/unpins the selected row to the Quick menu (or toggles a browser
  // favorite). It is the ONLY shoulder/stick button the menu still claims: L1/R1 (formerly page +
  // double-tap HOME/END) and L3 (formerly letter-jump) are deliberately left UNbound here so they
  // pass straight through to gameplay while the menu is open -- the player keeps the handbrake,
  // weapon wheel, and sprint/duck with the panel up. R3 is hidden from the game (SUPPRESS_MASK) so
  // the click never also melees. Bare R1 reaches here only without DpadLeft (the R1+Left open chord
  // was handled above and returned early); with no R1 handler it now falls through to NONE.
  if (just & GTAV_PAD_BTN_R3) return GTAV_MENU_COMMAND_PIN;
  if (just & GTAV_PAD_BTN_CROSS) return GTAV_MENU_COMMAND_SELECT;
  // Circle: a tap is an immediate single-level BACK; holding it past the threshold also emits one
  // BACK_ROOT (collapse to the root menu) so backing out of a deep leaf is one gesture. The BACK on
  // press is harmless before the collapse -- BACK_ROOT is absolute. Circle is menu-owned (not on
  // the player's critical list), so claiming the hold steals nothing.
  if (buttons & GTAV_PAD_BTN_CIRCLE) {
    if (just & GTAV_PAD_BTN_CIRCLE) {
      state->circle_hold = 1u;
      state->back_root_fired = 0u;
      return GTAV_MENU_COMMAND_BACK;
    }
    if (state->circle_hold) ++state->circle_hold;
    if (!state->back_root_fired && state->circle_hold >= GTAV_MENU_PAD_CIRCLE_HOLD_POLLS) {
      state->back_root_fired = 1u;
      return GTAV_MENU_COMMAND_BACK_ROOT;
    }
    return GTAV_MENU_COMMAND_NONE;
  }
  state->circle_hold = 0u;
  state->back_root_fired = 0u;
  return GTAV_MENU_COMMAND_NONE;
}

#if GTAV_MENU_ENABLE_PAD_INPUT

// Full OrbisPadData layout. scePadReadState writes the entire structure, so the
// buffer we hand it must be this size even though we only read .buttons and
// .connected. Layout mirrors the OpenOrbis pad ABI used by the reference menus.
typedef struct {
  uint8_t x, y;
} GtavPadStick;
typedef struct {
  uint8_t l2, r2;
} GtavPadAnalog;
typedef struct {
  uint16_t x, y;
  uint8_t finger;
  uint8_t pad[3];
} GtavPadTouch;
typedef struct {
  uint8_t fingers;
  uint8_t pad1[3];
  uint32_t pad2;
  GtavPadTouch touch[2];
} GtavPadTouchData;
typedef struct {
  uint32_t buttons;
  GtavPadStick leftStick;
  GtavPadStick rightStick;
  GtavPadAnalog analogButtons;
  uint16_t padding;
  float quat[4];
  float vel[3];
  float accel[3];
  GtavPadTouchData touch;
  uint8_t connected;
  uint64_t timestamp;
  uint8_t ext[16];
  uint8_t count;
  uint8_t unknown[15];
} GtavOrbisPadData;

// Resolved at load time against the ScePad / SceUserService modules already
// loaded in the GTA process (same import mechanism as gtav_notify).
extern int scePadInit(void);
extern int scePadGetHandle(int userId, int type, int index);
extern int scePadOpen(int userId, int type, int index, void* param);
extern int scePadReadState(int handle, void* data);
extern int scePadRead(int handle, void* data, int count);
extern int sceUserServiceGetInitialUser(int* userId);

#define GTAV_PAD_PORT_TYPE_STANDARD 0

// The buttons the menu itself uses are hidden from the game while the menu is open: the d-pad
// (navigation), Cross (select), Circle (back), plus R3 (pin to Quick / favorite). Everything else
// stays with the player so they can keep playing with the panel up: L1/R1 (handbrake / weapon
// wheel), L3 (sprint/duck), Triangle (Enter/Exit Vehicle), Square (handbrake/reload), the sticks,
// the L2/R2 triggers, and Options all reach the game. L1/R1/L3 used to be menu nav (paging /
// HOME-END / letter-jump) but were unbound so gameplay keeps them; only R3 is claimed (it pins the
// selected row, so it must not also melee). While CLOSED nothing here is suppressed (see
// gtav_pad_game_clear_mask), so even these buttons keep their normal in-game function.
#define GTAV_PAD_SUPPRESS_MASK                                                    \
  (GTAV_PAD_BTN_UP | GTAV_PAD_BTN_DOWN | GTAV_PAD_BTN_LEFT | GTAV_PAD_BTN_RIGHT | \
   GTAV_PAD_BTN_CROSS | GTAV_PAD_BTN_CIRCLE | GTAV_PAD_BTN_R3)

static int g_pad_handle = -1;
static int g_pad_ready = 0;
static int g_pad_reported_fail = 0;
static int g_pad_reported_buttons = 0;
static int g_pad_reported_hook_live = 0;
static uint32_t g_pad_last_reported_buttons = 0;
static GtavPadMapState g_pad_state;

// --- scePad GOT-swap hooks (game input suppression) ------------------------
// GTA reads the controller through scePad too (scePadReadState and/or
// scePadRead), so navigation buttons also drive gameplay (DpadUp opens the
// phone). We repoint every GOT slot holding those function pointers at our
// hooks (atomic qword swap). The hooks reach the originals through *direct
// captured addresses* (never the GOT, so swapping every slot cannot recurse),
// publish the real pre-mask buttons for the menu, and clear the menu's own
// buttons from what the game sees while visible (plus the open chord's DpadLeft
// whenever the chord is held, even while closed -- see gtav_pad_game_clear_mask).
typedef int (*GtavScePadReadStateFn)(int, void*);
typedef int (*GtavScePadReadFn)(int, void*, int);
static GtavScePadReadStateFn g_real_readstate;
static GtavScePadReadFn g_real_read;
static volatile uint32_t g_pad_hook_buttons;
static volatile int g_pad_hook_suppress;
static volatile uint32_t g_pad_hook_calls;
// Latched first-finger touchpad state from the same GOT-hook read, for the optional touchpad nav
// lane. The game's own touchpad use is negligible, so we read these without masking them out.
static volatile uint32_t g_pad_hook_touch_x;
static volatile uint32_t g_pad_hook_touch_y;
static volatile uint32_t g_pad_hook_touch_fingers;
static GtavPadTouchMapState g_pad_touch_state;

// Bits to strip from what the game sees for one controller frame. While the menu
// is visible we hide the menu's own buttons (d-pad / Cross / Circle / R3). Independently --
// even while the menu is closed -- we hide DpadLeft whenever the open/close chord
// (R1+DpadLeft) is held, so opening the menu in a vehicle does not also skip the
// radio station. Computed from the live pre-mask buttons so the bit is removed
// from the very frame the game is about to consume (no one-frame leak). R1 itself is never
// suppressed now (it is not a menu button), so the handbrake works with the menu open or closed.
static inline uint32_t gtav_pad_game_clear_mask(uint32_t buttons) {
  uint32_t cleared = g_pad_hook_suppress ? (uint32_t)GTAV_PAD_SUPPRESS_MASK : 0u;
  if ((buttons & GTAV_PAD_OPEN_CHORD_MASK) == GTAV_PAD_OPEN_CHORD_MASK) {
    cleared |= GTAV_PAD_BTN_LEFT;
  }
  // When the optional touchpad nav lane is on, also hide the physical touchpad-click from the game
  // while the menu is open (the menu uses it to select). Only when enabled, so a click still
  // reaches the game normally otherwise.
  if (g_pad_hook_suppress && gtav_native_bridge_touchpad_enabled()) {
    cleared |= GTAV_PAD_BTN_TOUCH_PAD;
  }
  // Keep a user keybind combo from leaking to the game: while a bound combo is fully held
  // (in any menu state), strip its bits from what the game sees so the hotkey doesn't also
  // trigger a gameplay action. The driving triggers (R2/L2) are preserved so a combo built on
  // a shoulder button doesn't disable them.
  for (uint32_t i = 0; i < g_bind_count && i < GTAV_KEYBIND_SLOTS; ++i) {
    const uint32_t m = g_binds[i].combo_mask;
    if (m && (buttons & m) == m) cleared |= m;
  }
  // Always restore the L2/R2 triggers (never menu buttons) in case a keybind combo above cleared
  // them, so a combo built on a shoulder trigger doesn't disable driving. L1/R1/L3 are no longer
  // menu buttons (only R3 is, and it stays suppressed while open), so they are not in
  // SUPPRESS_MASK and reach the game on their own -- nothing to restore for them here.
  uint32_t restore = (uint32_t)GTAV_PAD_BTN_R2 | (uint32_t)GTAV_PAD_BTN_L2;
  cleared &= ~restore;
  return cleared;
}

int gtav_pad_scepad_readstate_hook(int handle, void* pData) {
  GtavScePadReadStateFn real = g_real_readstate;
  int ret = real ? real(handle, pData) : -1;
  g_pad_hook_calls++;
  if (ret == 0 && pData) {
    GtavOrbisPadData* d = (GtavOrbisPadData*)pData;
    if (d->connected) {
      g_pad_hook_buttons = d->buttons;  // real, pre-mask (menu reads this)
      g_pad_hook_touch_x = d->touch.touch[0].x;
      g_pad_hook_touch_y = d->touch.touch[0].y;
      g_pad_hook_touch_fingers = d->touch.fingers;
      d->buttons &= ~gtav_pad_game_clear_mask(d->buttons);
    }
  }
  return ret;
}

int gtav_pad_scepad_read_hook(int handle, void* pData, int count) {
  GtavScePadReadFn real = g_real_read;
  int n = real ? real(handle, pData, count) : -1;
  g_pad_hook_calls++;
  if (n > 0 && pData) {
    GtavOrbisPadData* arr = (GtavOrbisPadData*)pData;
    for (int i = 0; i < n && i < count; ++i) {
      if (arr[i].connected) {
        g_pad_hook_buttons = arr[i].buttons;
        g_pad_hook_touch_x = arr[i].touch.touch[0].x;
        g_pad_hook_touch_y = arr[i].touch.touch[0].y;
        g_pad_hook_touch_fingers = arr[i].touch.fingers;
        arr[i].buttons &= ~gtav_pad_game_clear_mask(arr[i].buttons);
      }
    }
  }
  return n;
}

void* gtav_pad_input_hook_fn(void) {
  return (void*)&gtav_pad_scepad_readstate_hook;
}

uint32_t gtav_pad_input_hook_calls(void) {
  return g_pad_hook_calls;
}

#ifndef GTAV_MENU_ENABLE_PAD_HOOK
#define GTAV_MENU_ENABLE_PAD_HOOK 0
#endif

// Park flag: when set, the worker has defensively stood down the live input path ahead of a
// suspected teardown/suspend (see teardown_watchdog.h, driven by gtav_quit_guard). While parked
// the GOT swap is restored (PAD_HOOK builds) and the live poll returns nothing, so the game never
// calls a hook that is about to unmap. gtav_pad_input_unpark() clears it and the next poll
// re-installs. Lives outside the PAD_HOOK gate so the stand-down still happens (poll returns NONE)
// even on the default no-GOT-swap build.
static int g_pad_parked;

#if GTAV_MENU_ENABLE_PAD_HOOK

// In-process discovery + swap of GTA's GOT slot(s) for scePadReadState. The
// payload does this itself (never the host) so it can record exactly what it
// changed and restore it on shutdown -- a dangling GOT entry after the payload
// unmaps would crash the game on the next controller read.
typedef struct {
  void* start;
  void* end;
  int64_t offset;
  int32_t protection;
  int32_t memoryType;
  uint32_t flags;
  char name[32];
} GtavVqInfo;

extern int sceKernelVirtualQuery(const void* addr, int flags, GtavVqInfo* info, size_t infoSize);
extern int mprotect(void* addr, size_t len, int prot);
extern long sysconf(int name);

// Flipping a RELRO GOT page writable in-process needs Sony's sceKernelMprotect: libc
// mprotect is a raw syscall the PS5 kernel rejects as "directly issued" (fatal
// SYSTEM_ILLEGAL_FUNCTION_CALL) unless a kernel patch allows it. sceKernelMprotect routes
// through libkernel (authorized origin), like sceKernelVirtualQuery above, so it RETURNS
// an error instead of trapping when denied -- letting swap_pad_slot skip the slot. (Set
// for the in-game worker build via PAD_HOOK=1; host/test builds fall back to libc.)
#ifndef GTAV_MENU_PAD_HOOK_SCE_MPROTECT
#define GTAV_MENU_PAD_HOOK_SCE_MPROTECT 0
#endif
#if GTAV_MENU_PAD_HOOK_SCE_MPROTECT
extern int sceKernelMprotect(const void* addr, size_t len, int prot);
#endif

#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC 0x4
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 47
#endif

// Make [pg, pg+page) read+write. Prefers the authorized sceKernelMprotect (in-game),
// falling back to libc mprotect on host/test builds. Returns 0 on success.
static int pad_make_page_writable(uintptr_t pg, size_t page) {
#if GTAV_MENU_PAD_HOOK_SCE_MPROTECT
  return sceKernelMprotect((const void*)pg, page, PROT_READ | PROT_WRITE) == 0 ? 0 : -1;
#else
  return mprotect((void*)pg, page, PROT_READ | PROT_WRITE) == 0 ? 0 : -1;
#endif
}

// Confirm a page is actually writable now (the protect call claimed success). Guards the
// case where a denied protect "succeeds" without changing the page -- so we NEVER write to
// read-only memory and fault the game. Returns 1 if writable.
static int pad_page_is_writable(uintptr_t pg) {
  GtavVqInfo info;
  if (sceKernelVirtualQuery((const void*)pg, 0, &info, sizeof(info)) != 0) {
    return 0;
  }
  return (info.protection & PROT_WRITE) != 0;
}

// A captured "original" scePad entry must be real executable code. If resolution ever returned
// something that is not in an executable mapping -- a stale pointer, freed memory, or (after an
// unclean prior session / reinject) our own hook -- swapping GTA's GOT to chain through it would
// jump into garbage and crash the game. Reliable and name-independent: a real libScePad export
// always lives in an executable region. Returns 1 if `fn` is sane to chain through.
static int pad_original_is_executable(uintptr_t fn) {
  GtavVqInfo info;
  if (fn == 0) {
    return 0;
  }
  if (sceKernelVirtualQuery((const void*)fn, 0, &info, sizeof(info)) != 0) {
    return 0;
  }
  return (info.protection & PROT_EXEC) != 0;
}

#define GTAV_VQ_FIND_NEXT 1
#define GTAV_PAD_HOOK_MAX_SLOTS 64u
// GTA's GOT is read-only after RELRO, so scan readable *data* regions (not just
// writable ones) and mprotect each match to writable before swapping. Skip
// executable code and huge regions (heaps, GPU memory) so the one-time scan
// stays bounded on the worker thread.
#define GTAV_PAD_HOOK_MAX_REGION (64u * 1024u * 1024u)
#define GTAV_PAD_HOOK_MAX_TOTAL (1536u * 1024u * 1024u)

typedef struct {
  volatile uintptr_t* slot;
  uintptr_t original;
} GtavPadHookSlot;

static GtavPadHookSlot g_pad_hook_slots[GTAV_PAD_HOOK_MAX_SLOTS];
static uint32_t g_pad_hook_slot_count;
static uint32_t g_pad_hook_skipped;  // matching slots skipped (page could not be made writable)
static int g_pad_hook_installed;

// Repoint one slot to `hook`, making its page writable first if needed. Records
// the original for restore. Returns 1 if swapped, 0 otherwise.
static int swap_pad_slot(volatile uintptr_t* slot, uintptr_t original, uintptr_t hook,
                         int writable) {
  if (g_pad_hook_slot_count >= GTAV_PAD_HOOK_MAX_SLOTS) {
    return 0;
  }
  if (!writable) {
    long pv = sysconf(_SC_PAGESIZE);
    size_t page = pv > 0 ? (size_t)pv : 0x4000u;
    uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)(page - 1);
    // Make the RELRO page writable via the authorized call, then CONFIRM it actually
    // took before writing. If either step fails -- e.g. the protect is denied because the
    // payload has lost jailbreak privileges -- skip this slot instead of writing to
    // still-read-only memory and faulting the game on inject (the prior crash mode).
    if (pad_make_page_writable(pg, page) != 0 || !pad_page_is_writable(pg)) {
      ++g_pad_hook_skipped;
      return 0;
    }
  }
  g_pad_hook_slots[g_pad_hook_slot_count].slot = slot;
  g_pad_hook_slots[g_pad_hook_slot_count].original = original;
  *slot = hook;  // atomic aligned qword store
  ++g_pad_hook_slot_count;
  return 1;
}

static int install_pad_hook(void) {
  if (g_pad_parked) {
    return -1;  // parked for teardown/suspend: do not (re)install until unparked
  }
  if (g_pad_hook_installed || !g_real_readstate) {
    return g_pad_hook_installed ? 0 : -1;
  }

  const uintptr_t target_state = (uintptr_t)g_real_readstate;
  const uintptr_t hook_state = (uintptr_t)&gtav_pad_scepad_readstate_hook;
  const uintptr_t target_read = (uintptr_t)g_real_read;
  const uintptr_t hook_read = (uintptr_t)&gtav_pad_scepad_read_hook;

  // Refuse to install if a captured "original" is not real executable code (a stale/garbage
  // pointer, or our own hook from an unclean prior session): chaining GTA's GOT through it would
  // jump into freed memory and crash the game. Fall back to native-bridge input. Latch installed
  // so this decision is made once, not re-attempted every poll.
  if (target_state == hook_state || !pad_original_is_executable(target_state) ||
      (target_read && (target_read == hook_read || !pad_original_is_executable(target_read)))) {
    g_pad_hook_installed = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                       "pad hook SKIPPED: original not executable state=0x%llx read=0x%llx "
                       "(native input fallback)",
                       (unsigned long long)target_state, (unsigned long long)target_read);
    return -1;
  }

  GtavVqInfo info;
  void* addr = 0;
  uint64_t scanned = 0;
  uint32_t scanned_regions = 0;

  while (g_pad_hook_slot_count < GTAV_PAD_HOOK_MAX_SLOTS &&
         sceKernelVirtualQuery(addr, GTAV_VQ_FIND_NEXT, &info, sizeof(info)) == 0) {
    const uintptr_t start = (uintptr_t)info.start;
    const uintptr_t end = (uintptr_t)info.end;
    if (end <= start) {
      break;
    }
    addr = (void*)end;

    // Readable, non-executable data regions only (GOT / pointer tables), of a
    // bounded size. Includes read-only RELRO GOT pages. Skip stacks: a stray
    // copy of the pointer value there must never be "swapped".
    const int readable = (info.protection & PROT_READ) != 0;
    const int executable = (info.protection & PROT_EXEC) != 0;
    const int writable = (info.protection & PROT_WRITE) != 0;
    const int is_stack = (info.flags & 0x4u) != 0;  // OrbisVirtualQueryInfo.isStack
    if (readable && !executable && !is_stack && (end - start) <= GTAV_PAD_HOOK_MAX_REGION) {
      for (uintptr_t p = start; p + sizeof(uintptr_t) <= end; p += sizeof(uintptr_t)) {
        const uintptr_t v = *(volatile uintptr_t*)p;
        if (v == target_state) {
          swap_pad_slot((volatile uintptr_t*)p, target_state, hook_state, writable);
        } else if (target_read && v == target_read) {
          swap_pad_slot((volatile uintptr_t*)p, target_read, hook_read, writable);
        }
        if (g_pad_hook_slot_count >= GTAV_PAD_HOOK_MAX_SLOTS) {
          break;
        }
      }
      ++scanned_regions;
    }

    scanned += (end - start);
    if (scanned > GTAV_PAD_HOOK_MAX_TOTAL) {
      break;
    }
  }

  g_pad_hook_installed = 1;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                     "pad hook installed slots=%u skipped=%u regions=%u state=0x%llx read=0x%llx",
                     g_pad_hook_slot_count, g_pad_hook_skipped, scanned_regions,
                     (unsigned long long)target_state, (unsigned long long)target_read);
  return g_pad_hook_slot_count > 0 ? 0 : -1;
}

static void restore_pad_hook(void) {
  if (!g_pad_hook_installed) {
    return;
  }
  for (uint32_t i = 0; i < g_pad_hook_slot_count; ++i) {
    if (g_pad_hook_slots[i].slot) {
      *g_pad_hook_slots[i].slot = g_pad_hook_slots[i].original;
    }
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad hook restored slots=%u",
                     g_pad_hook_slot_count);
  g_pad_hook_installed = 0;
  g_pad_hook_slot_count = 0;
  g_pad_hook_skipped = 0;
}

static int pad_hook_installed_flag(void) {
  return g_pad_hook_installed;
}
static uint32_t pad_hook_slot_count(void) {
  return g_pad_hook_slot_count;
}

#else  // GTAV_MENU_ENABLE_PAD_HOOK

static int install_pad_hook(void) {
  return -1;
}
static void restore_pad_hook(void) {}
static int pad_hook_installed_flag(void) {
  return 0;
}
static uint32_t pad_hook_slot_count(void) {
  return 0;
}

#endif  // GTAV_MENU_ENABLE_PAD_HOOK

int gtav_pad_input_init(void) {
  scePadInit();

  int user = -1;
  if (sceUserServiceGetInitialUser(&user) != 0 || user < 0) {
    user = 0xFF;  // ORBIS_USER_SERVICE_USER_ID_EVERYONE fallback
  }

  // Prefer the handle of the pad GTA already opened so we share its device and
  // do not fight the game for an exclusive open; fall back to opening our own.
  int handle = scePadGetHandle(user, GTAV_PAD_PORT_TYPE_STANDARD, 0);
  int via_open = 0;
  if (handle < 0) {
    handle = scePadOpen(user, GTAV_PAD_PORT_TYPE_STANDARD, 0, NULL);
    via_open = 1;
  }
  if (handle < 0) {
    g_pad_ready = 0;
    if (!g_pad_reported_fail) {
      g_pad_reported_fail = 1;
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad input unavailable user=0x%x handle=%d",
                         (unsigned)user, handle);
    }
    return -1;
  }

  g_pad_handle = handle;
  g_pad_ready = 1;
  memset(&g_pad_state, 0, sizeof(g_pad_state));
  // Capture the real scePad entries as direct callables BEFORE any GOT slot is
  // swapped, so the hooks can reach the originals without recursing.
  if (!g_real_readstate) {
    g_real_readstate = (GtavScePadReadStateFn)&scePadReadState;
  }
  if (!g_real_read) {
    g_real_read = (GtavScePadReadFn)&scePadRead;
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad input ready handle=%d user=0x%x via=%s",
                     handle, (unsigned)user, via_open ? "open" : "getHandle");
  // Report the live scePadReadState target (GOT-swap source value) and our hook
  // address (GOT-swap destination), so the host can repoint GTA's GOT slot(s).
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad fn scePadReadState=0x%llx hook=0x%llx",
                     (unsigned long long)(uintptr_t)&scePadReadState,
                     (unsigned long long)(uintptr_t)&gtav_pad_scepad_readstate_hook);
  return 0;
}

int gtav_pad_input_available(void) {
  return g_pad_ready;
}

uint32_t gtav_pad_input_poll_command(int visible) {
  if (!g_pad_ready) {
    return GTAV_MENU_COMMAND_NONE;
  }
  if (g_pad_parked) {
    // Parked for a suspected teardown/suspend: stand down completely (no scePad read, no GOT
    // re-install) so the worker never touches the controller while the process is being torn
    // down. gtav_pad_input_unpark() re-arms the live path.
    return GTAV_MENU_COMMAND_NONE;
  }

  // Install the suppression hook as soon as the pad is live -- not only once the
  // menu is first shown -- so the open/close chord (R1+DpadLeft) can be masked
  // from the game on the very first open and while the menu stays closed (no-op
  // unless GTAV_MENU_ENABLE_PAD_HOOK=1). Masking is then driven per tick: the
  // visibility flag hides the menu's nav buttons while it is up; the hook itself
  // hides the chord's DpadLeft whenever the chord is held (gtav_pad_game_clear_mask).
  install_pad_hook();
  g_pad_hook_suppress = visible ? 1 : 0;

  // Reliable, recurring install/fire telemetry: the status event ring buffer
  // rotates quickly, so re-emit the hook state often enough that any read sees
  // it. Reports whether the swap found slots and whether GTA is calling us.
  if (visible) {
    static uint32_t poll_count;
    if ((poll_count++ % 90u) == 0u) {
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                         "pad hook state installed=%d slots=%u calls=%u", pad_hook_installed_flag(),
                         pad_hook_slot_count(), g_pad_hook_calls);
    }
  }

  uint32_t buttons;
  uint32_t touch_x = 0u, touch_y = 0u, touch_fingers = 0u;
  if (g_pad_hook_calls) {
    // The GOT hook is live and already reading the controller every frame on
    // the game's thread; consume its captured pre-mask buttons instead of
    // issuing our own (now-hooked) read.
    buttons = g_pad_hook_buttons;
    touch_x = g_pad_hook_touch_x;
    touch_y = g_pad_hook_touch_y;
    touch_fingers = g_pad_hook_touch_fingers;
    // Observability for the GOT swap: confirm the hook is firing (and at what
    // rate) so a live install can be verified before trusting suppression.
    if (!g_pad_reported_hook_live) {
      g_pad_reported_hook_live = 1;
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                         "pad hook live first calls=%u buttons=0x%08x", g_pad_hook_calls, buttons);
    } else if ((g_pad_hook_calls % 600u) == 0u) {
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                         "pad hook calls=%u suppress=%d buttons=0x%08x", g_pad_hook_calls,
                         g_pad_hook_suppress, buttons);
    }
  } else {
    GtavOrbisPadData data;
    memset(&data, 0, sizeof(data));
    if (scePadReadState(g_pad_handle, &data) != 0 || !data.connected) {
      return GTAV_MENU_COMMAND_NONE;
    }
    buttons = data.buttons;
    touch_x = data.touch.touch[0].x;
    touch_y = data.touch.touch[0].y;
    touch_fingers = data.touch.fingers;
  }

  // One-time confirmation that live button bits are actually being read, so a
  // button-free injection can still prove the read path the moment any input
  // (stick, button, touchpad) occurs.
  if (buttons && !g_pad_reported_buttons) {
    g_pad_reported_buttons = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad buttons first=0x%08x", buttons);
  }
  if (buttons != g_pad_last_reported_buttons) {
    const uint32_t diff = buttons ^ g_pad_last_reported_buttons;
    g_pad_last_reported_buttons = buttons;
    if (buttons || diff) {
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad buttons=0x%08x diff=0x%08x", buttons,
                         diff);
    }
  }

  // Allow Left/Right auto-repeat only on a value-cycler row (Left is a one-shot Back
  // elsewhere, so repeating it would walk the user out of the menu).
  const int lr_repeat_ok = visible ? gtav_native_bridge_selected_is_cycler() : 0;
  uint32_t cmd = gtav_pad_input_map(&g_pad_state, buttons, visible, g_repeat_delay, g_repeat_rate,
                                    lr_repeat_ok);
  // Optional touchpad gesture lane (default off; the Menu Settings "Touchpad" toggle), layered on
  // top of the d-pad: only consulted when the button mapper produced nothing this poll, so the
  // d-pad always wins. fingers>0 = a finger is on the pad; the TOUCH_PAD bit is a physical click.
  if (cmd == GTAV_MENU_COMMAND_NONE && gtav_native_bridge_touchpad_enabled()) {
    cmd =
        gtav_pad_input_map_touch(&g_pad_touch_state, (int)touch_x, (int)touch_y, touch_fingers > 0u,
                                 (buttons & GTAV_PAD_BTN_TOUCH_PAD) != 0u, 1, visible);
  }
  return cmd;
}

void gtav_pad_input_wait_menu_buttons_released(uint32_t max_wait_usec) {
  if (!g_pad_ready || max_wait_usec == 0u) {
    return;
  }

  install_pad_hook();
  g_pad_hook_suppress = 1;

  uint32_t waited = 0;
  while (waited < max_wait_usec) {
    uint32_t buttons = 0;
    if (g_pad_hook_calls) {
      buttons = g_pad_hook_buttons;
    } else {
      GtavOrbisPadData data;
      memset(&data, 0, sizeof(data));
      if (scePadReadState(g_pad_handle, &data) == 0 && data.connected) {
        buttons = data.buttons;
      }
    }
    if ((buttons & GTAV_PAD_SUPPRESS_MASK) == 0u) {
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                         "pad shutdown release waited=%u buttons=0x%08x", waited, buttons);
      return;
    }
    usleep(8000);
    waited += 8000u;
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                     "pad shutdown release timeout=%u buttons=0x%08x", waited,
                     (unsigned)g_pad_hook_buttons);
}

void gtav_pad_input_shutdown(void) {
  // Restore GTA's GOT slot(s) before the payload's threads exit so the game
  // never calls into a hook that is about to be unmapped.
  restore_pad_hook();
}

void gtav_pad_input_park(void) {
  if (g_pad_parked) {
    return;
  }
  g_pad_parked = 1;
  // Restore the scePad GOT swap NOW (no-op on a non-GOT-swap build) so a teardown/suspend cannot
  // call a hook that is about to unmap. install_pad_hook() refuses to re-install while parked.
  restore_pad_hook();
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                     "pad input PARKED: GOT restored, live input stood down (teardown/suspend)");
}

void gtav_pad_input_unpark(void) {
  if (!g_pad_parked) {
    return;
  }
  g_pad_parked = 0;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "pad input UNPARKED: re-arming live input");
  // The next gtav_pad_input_poll_command() re-installs the GOT swap via install_pad_hook().
}

int gtav_pad_input_is_parked(void) {
  return g_pad_parked;
}

#else  // GTAV_MENU_ENABLE_PAD_INPUT

int gtav_pad_input_init(void) {
  return -1;
}

int gtav_pad_input_available(void) {
  return 0;
}

uint32_t gtav_pad_input_poll_command(int visible) {
  (void)visible;
  return GTAV_MENU_COMMAND_NONE;
}

int gtav_pad_scepad_readstate_hook(int handle, void* pData) {
  (void)handle;
  (void)pData;
  return -1;
}

void* gtav_pad_input_hook_fn(void) {
  return (void*)0;
}

uint32_t gtav_pad_input_hook_calls(void) {
  return 0;
}

void gtav_pad_input_wait_menu_buttons_released(uint32_t max_wait_usec) {
  (void)max_wait_usec;
}

void gtav_pad_input_shutdown(void) {}

void gtav_pad_input_park(void) {}

void gtav_pad_input_unpark(void) {}

int gtav_pad_input_is_parked(void) {
  return 0;
}

#endif  // GTAV_MENU_ENABLE_PAD_INPUT
