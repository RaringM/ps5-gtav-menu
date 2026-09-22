#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_COMMAND_MAILBOX_MAGIC 0x584F424D56415447ull
#define GTAV_MENU_COMMAND_MAILBOX_ABI_VERSION 1u
#define GTAV_MENU_COMMAND_MAILBOX_LAST_COMMAND_LEN 32u
#define GTAV_MENU_COMMAND_MAILBOX_RESULT_LEN 80u
#define GTAV_MENU_COMMAND_MAILBOX_SIZE 160u

enum {
  GTAV_MENU_COMMAND_NONE = 0,
  GTAV_MENU_COMMAND_TOGGLE = 1,
  GTAV_MENU_COMMAND_NEXT = 2,
  GTAV_MENU_COMMAND_PREV = 3,
  GTAV_MENU_COMMAND_SELECT = 4,
  GTAV_MENU_COMMAND_BACK = 5,
  GTAV_MENU_COMMAND_STOP = 6,
  GTAV_MENU_COMMAND_TELEMETRY = 7,
  GTAV_MENU_COMMAND_GOD = 8,
  GTAV_MENU_COMMAND_HEAL_ARMOR = 9,
  GTAV_MENU_COMMAND_CLEAR_WANTED = 10,
  GTAV_MENU_COMMAND_SHOW = 11,
  GTAV_MENU_COMMAND_HIDE = 12,
  // Runtime tuning: set the worker text-render interval (ticks between renders).
  // argument = interval (1 = every tick/~60Hz .. larger = slower/less off-thread
  // contention). Lets us sweep the smooth-vs-flicker trade-off live without a rebuild.
  GTAV_MENU_COMMAND_SET_RENDER_INTERVAL = 13,
  // Runtime tuning: set worker loop rate in Hz (argument = Hz). Over-rendering above
  // the game frame rate removes phase-beat flicker. Pairs with SET_RENDER_INTERVAL.
  GTAV_MENU_COMMAND_SET_WORKER_HZ = 14,
  // Test/dev: spawn a vehicle by catalog index (argument = index). The menu
  // translates index -> model hash before queueing the game-thread spawn job; it
  // never falls back to worker-thread CREATE_VEHICLE by default.
  GTAV_MENU_COMMAND_SPAWN_VEHICLE = 15,
  // D-pad Left/Right: adjust the selected row's value (e.g. cycle weather/time)
  // when it is a list option; no-op on other rows. Suppressed from the game while
  // the menu is visible, so claiming them carries no fall-through risk.
  GTAV_MENU_COMMAND_LEFT = 16,
  GTAV_MENU_COMMAND_RIGHT = 17,
  // Legacy diagnostic: try sceKernelLoadStartModule from the already-injected
  // menu's context and report handle/res via status events. ps5debug-only module
  // authorization has returned EACCES on the current target; the supported spawn
  // lane is the etaHEN-loaded game module plus handler-slot queue consumer.
  GTAV_MENU_COMMAND_LOAD_MODULE = 18,
  // Test/dev: activate any feature by its GTAV_NATIVE_SHELL_ACTION_* id (argument =
  // action id), routed through gtav_features_activate_param(action, 0). Lets a host
  // script validate each feature on-console one at a time: send activate-action,
  // then confirm the worker is still ticking (a crash stalls it), giving automatic
  // per-feature crash isolation without walking the menu by hand.
  GTAV_MENU_COMMAND_ACTIVATE_ACTION = 19,
  // Runtime tuning: set the on-screen toast/confirmation display duration in worker
  // ticks (argument = ticks, clamped 30..1800). Default ~270 (~3s at 90Hz). Lets the
  // dwell time be tuned live without a rebuild, mirroring SET_RENDER_INTERVAL.
  GTAV_MENU_COMMAND_SET_TOAST_TICKS = 20,
  // A user keybind combo fired while the menu was closed (see gtav_pad_input_map). The
  // bound action id is read via gtav_pad_input_last_hotkey_action(); the menu dispatches it
  // like a normal shell action. Pad-only (never produced by the mailbox/file paths).
  GTAV_MENU_COMMAND_HOTKEY = 21,
  // Page the cursor up/down by one visible window (L1/R1). On a value cycler these instead
  // fast-step the value by several steps. Suppressed from the game while the menu is visible.
  GTAV_MENU_COMMAND_PAGE_PREV = 22,
  GTAV_MENU_COMMAND_PAGE_NEXT = 23,
  // Jump the cursor to the first / last row (Triangle / Square).
  GTAV_MENU_COMMAND_HOME = 24,
  GTAV_MENU_COMMAND_END = 25,
  // Seek the cursor to the previous / next first-letter boundary in the list (L3 / R3). Turns a
  // 1000-item alphabetical catalog scroll into a handful of presses. Suppressed from the game.
  GTAV_MENU_COMMAND_LETTER_PREV = 26,
  GTAV_MENU_COMMAND_LETTER_NEXT = 27,
  // Collapse straight to the root menu (Circle held past the hold threshold). A plain Circle tap
  // still pops one level (BACK); this jumps out of a deep leaf in one gesture.
  GTAV_MENU_COMMAND_BACK_ROOT = 28,
  // Pin/unpin the selected row to the global Quick menu, or toggle a browser favorite on a
  // spawn/skin row (R3 / right-stick click). Suppressed from the game while the menu is visible.
  GTAV_MENU_COMMAND_PIN = 29,
  // Default-off rendering diagnostics. Mode argument = mode | (alpha << 8).
  GTAV_MENU_COMMAND_RENDER_DIAG_MODE = 30,
  // Capture argument: 1 = start once, 2 = stop (no recycling/re-arm in this process).
  GTAV_MENU_COMMAND_RENDER_DIAG_CAPTURE = 31,
  GTAV_MENU_COMMAND_RENDER_CYCLE_PROBE = 32,
  GTAV_MENU_COMMAND_RENDER_PATH_LOW = 33,
  GTAV_MENU_COMMAND_RENDER_PATH_HIGH = 34,
  GTAV_MENU_COMMAND_RENDER_PATH_CONTROL = 35,
  GTAV_MENU_COMMAND_RENDER_BANK_CONTROL = 36,
  GTAV_MENU_COMMAND_RENDER_PHASE_SLOT = 37,
  GTAV_MENU_COMMAND_RENDER_PHASE_CONTROL = 38,
};

enum {
  GTAV_MENU_COMMAND_STATUS_IDLE = 0,
  GTAV_MENU_COMMAND_STATUS_PENDING = 1,
  GTAV_MENU_COMMAND_STATUS_ACKED = 2,
  GTAV_MENU_COMMAND_STATUS_UNKNOWN = 3,
};

typedef struct GtavMenuCommandMailbox {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t request_sequence;
  uint64_t ack_sequence;
  uint32_t command;
  uint32_t status;
  uint64_t argument;
  char last_command[GTAV_MENU_COMMAND_MAILBOX_LAST_COMMAND_LEN];
  char last_result[GTAV_MENU_COMMAND_MAILBOX_RESULT_LEN];
} GtavMenuCommandMailbox;

typedef struct GtavMenuCommandRequest {
  uint64_t sequence;
  uint32_t command;
  uint64_t argument;
} GtavMenuCommandRequest;

extern GtavMenuCommandMailbox gtav_menu_command_mailbox;
extern const char gtav_menu_command_mailbox_marker[];

void gtav_command_mailbox_reset(void);
int gtav_command_mailbox_consume(GtavMenuCommandRequest* request);
void gtav_command_mailbox_complete(uint64_t sequence, uint32_t status, const char* result);
const char* gtav_command_mailbox_command_name(uint32_t command);

#ifdef __cplusplus
}
#endif
