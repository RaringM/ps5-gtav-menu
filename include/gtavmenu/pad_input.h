#pragma once

#include <stdint.h>

#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/keybinds.h"

#ifdef __cplusplus
extern "C" {
#endif

// Direct DualSense input for the menu via the PS5 scePad HID library.
//
// This input source reads the controller at the OS layer (scePadReadState),
// completely independent of GTA V's scripted input natives. It exists so menu
// navigation does not have to wait on the unsolved IS_*CONTROL* native-identity
// problem: GTA already has the ScePad module loaded in-process, so an injected
// payload can resolve and call it the same way it already calls
// sceKernelSendNotificationRequest.
//
// The live scePad I/O is compiled only when GTAV_MENU_ENABLE_PAD_INPUT=1 (which
// also requires linking -lScePad -lSceUserService). With the macro off the file
// still compiles and exposes the pure button->command mapping below, so it can
// be built into any payload and unit-tested on the host with no PS5 libraries.

// Orbis/DualSense button bits as reported in OrbisPadData.buttons.
#define GTAV_PAD_BTN_L3 0x00000002u
#define GTAV_PAD_BTN_R3 0x00000004u
#define GTAV_PAD_BTN_OPTIONS 0x00000008u
#define GTAV_PAD_BTN_UP 0x00000010u
#define GTAV_PAD_BTN_RIGHT 0x00000020u
#define GTAV_PAD_BTN_DOWN 0x00000040u
#define GTAV_PAD_BTN_LEFT 0x00000080u
#define GTAV_PAD_BTN_L2 0x00000100u
#define GTAV_PAD_BTN_R2 0x00000200u
#define GTAV_PAD_BTN_L1 0x00000400u
#define GTAV_PAD_BTN_R1 0x00000800u
#define GTAV_PAD_BTN_TRIANGLE 0x00001000u
#define GTAV_PAD_BTN_CIRCLE 0x00002000u
#define GTAV_PAD_BTN_CROSS 0x00004000u
#define GTAV_PAD_BTN_SQUARE 0x00008000u
#define GTAV_PAD_BTN_TOUCH_PAD 0x00100000u

// The menu open/close chord: R1 + DpadLeft (either press order). Defined once as
// a mask so the button->command mapper and the scePad suppression hook agree on
// exactly which chord to treat specially.
#define GTAV_PAD_OPEN_CHORD_MASK (GTAV_PAD_BTN_R1 | GTAV_PAD_BTN_LEFT)

// Row capabilities passed to gtav_pad_input_map (bit flags, from the selected menu row).
//   GTAV_PAD_ROW_CYCLER    the row is a value cycler: Left/Right may auto-repeat.
#define GTAV_PAD_ROW_CYCLER 0x1

// Caller-owned mapping state: edge detection + dpad hold auto-repeat + a latch
// so the open chord fires exactly once per chord rather than every poll.
typedef struct GtavPadMapState {
  uint32_t prev_buttons;
  uint32_t repeat_bit;     // dpad bit currently auto-repeating (UP/DOWN), or 0
  uint32_t repeat_poll;    // polls elapsed since repeat_bit was first held
  uint32_t chord_latch;    // open chord currently held and already fired
  uint32_t hotkey_latch;   // bound combo currently held and already fired (mask), or 0
  uint32_t hotkey_action;  // action id of the last HOTKEY command (read by the caller)
  uint32_t poll_seq;       // increments once per map() call; never 0 after the first call
  // L1/R1 top/bottom jump while the menu is open: the shoulder being tracked (0 = none) and
  // whether another menu button spoiled it. A clean press fires on release, so R1 held for the
  // R1 + DpadLeft close chord never also jumps.
  uint32_t shoulder_bit;
  uint32_t shoulder_spoiled;
  // Circle hold-to-collapse: a tap is an immediate single BACK; holding past the threshold also
  // emits one BACK_ROOT (collapse to the root menu). The intermediate BACK is harmless -- the
  // collapse is absolute. circle_hold counts polls Circle has been held; back_root_fired latches
  // the one BACK_ROOT per hold.
  uint32_t circle_hold;
  uint32_t back_root_fired;
} GtavPadMapState;

// Caller-owned state for the touchpad gesture mapper (separate from the button mapper so the two
// input lanes stay independent and both stay pure/host-testable).
typedef struct GtavPadTouchMapState {
  uint32_t prev_finger;  // finger-down last call (contact-start / tap-release edge detection)
  uint32_t prev_click;   // physical touchpad-click held last call (edge detection)
  int active;            // a tracked contact is in progress
  int last_y;            // last contact y (for the incremental vertical delta)
  int accum_y;           // vertical travel not yet turned into a row step
  int travel;            // total absolute travel this contact (tap vs swipe classification)
} GtavPadTouchMapState;

// Pure touchpad gesture mapper: turns a single-finger contact into the EXISTING menu command
// vocabulary so it layers on top of the d-pad without replacing it (vertical swipe -> PREV/NEXT,
// fast flick -> PAGE_PREV/PAGE_NEXT, a tap or a physical touchpad-click -> SELECT). enabled gates
// the whole lane (the Menu Settings "Touchpad" toggle); visible gates it to an open menu. y grows
// downward, so swiping the finger down moves the cursor down. Side-effect-free apart from *state.
// NOTE: the touch coordinate range + finger encoding are PS5-hardware specific; the px thresholds
// are provisional and must be calibrated on hardware (see GTAV_MENU_TOUCH_* in pad_input.c).
uint32_t gtav_pad_input_map_touch(GtavPadTouchMapState* state, int x, int y, int finger_down,
                                  int clicked, int enabled, int visible);

// Pure, side-effect-free (apart from *state) mapping from a controller button
// snapshot to a single GTAV_MENU_COMMAND_*:
//   - R1 + DpadLeft (either press order) -> TOGGLE, once per chord.
//   - While visible: DpadUp -> PREV, DpadDown -> NEXT (with auto-repeat),
//     Cross -> SELECT, Circle -> BACK, L1 -> HOME, R1 -> END (top / bottom selectable row).
//   - While NOT visible: a bound keybind combo (installed via gtav_pad_input_set_binds)
//     held in full -> HOTKEY, once per press; the fired action is left in
//     state->hotkey_action. Open chord wins (checked first), and hotkeys only match while
//     closed so they never steal navigation.
// repeat_delay / repeat_rate are in poll ticks; repeat_rate 0 means repeat
// every poll once the delay elapses; repeat_delay 0 disables auto-repeat.
// row_caps (GTAV_PAD_ROW_* bits) describes the selected row. GTAV_PAD_ROW_CYCLER gates
// Left/Right auto-repeat (on other rows Left/Right are one-shots); Up/Down auto-repeat (scrolling)
// is unaffected. L1/R1 belong to the menu on every row while it is visible: a press with no other
// menu button joining it emits HOME / END on RELEASE (no repeat), so the R1 + DpadLeft close chord
// never also jumps. While hidden L1/R1 map to NONE (they stay with the game).
// Reads the installed bind table (file-local; set via gtav_pad_input_set_binds) but is
// otherwise side-effect-free apart from *state, so host tests stay deterministic.
uint32_t gtav_pad_input_map(GtavPadMapState* state, uint32_t buttons, int visible,
                            uint32_t repeat_delay, uint32_t repeat_rate, int row_caps);

// Override the live D-pad auto-repeat cadence (poll ticks; rate 0 = repeat every poll once the
// delay elapses, delay 0 = no auto-repeat). Set by the Menu Settings Nav Delay / Nav Speed cyclers
// so the scroll feel is tunable + persisted. Affects only the live poll path (gtav_pad_input_poll_
// command); the pure gtav_pad_input_map still takes delay/rate as explicit parameters.
void gtav_pad_input_set_repeat(uint32_t delay_ticks, uint32_t rate_ticks);

// Install the active keybind table (combo -> action). Copied into file-local storage that
// both gtav_pad_input_map (detection while closed) and the game-input suppression hook read.
// count is clamped to GTAV_KEYBIND_SLOTS; a NULL/empty table disables all hotkeys.
void gtav_pad_input_set_binds(const GtavKeybind* binds, uint32_t count);

// Action id of the most recent GTAV_MENU_COMMAND_HOTKEY produced by the live poll path.
uint32_t gtav_pad_input_last_hotkey_action(void);

// Live path (no-ops returning failure unless GTAV_MENU_ENABLE_PAD_INPUT=1).
// gtav_pad_input_init acquires the active controller handle; returns 0 on
// success. gtav_pad_input_available reports whether a handle is held.
int gtav_pad_input_init(void);
int gtav_pad_input_available(void);

// Read the live controller and return one menu command (NONE if nothing/idle).
uint32_t gtav_pad_input_poll_command(int visible);

// scePadReadState GOT-swap hook for game-input suppression (live path only).
// gtav_pad_input_hook_fn returns the address GTA's GOT slot(s) should point at;
// gtav_pad_input_hook_calls reports how many times the hook has run (0 until
// the GOT swap is installed). Both return 0 unless GTAV_MENU_ENABLE_PAD_INPUT=1.
int gtav_pad_scepad_readstate_hook(int handle, void* pData);
void* gtav_pad_input_hook_fn(void);
uint32_t gtav_pad_input_hook_calls(void);

// Keep menu buttons suppressed until they are released, or until max_wait_usec
// expires. Used during Stop Runtime so the final Cross/D-pad press does not
// leak into GTA after the menu tears down.
void gtav_pad_input_wait_menu_buttons_released(uint32_t max_wait_usec);

// Restore any installed scePadReadState GOT swap. MUST be called on shutdown
// before the payload unmaps, or the game will call into freed memory. No-op if
// no hook was installed.
void gtav_pad_input_shutdown(void);

// Teardown/suspend guard. gtav_pad_input_park() defensively restores the scePadReadState GOT swap
// and stands the live input path down (poll returns NONE, the hook is not re-installed), so a
// force-close or rest-mode suspend cannot call a hook that is about to unmap.
// gtav_pad_input_unpark() re-arms it (the next poll re-installs the swap). Driven by the worker's
// teardown watchdog / app-state listener (see teardown_watchdog.h, gtav_quit_guard). Idempotent;
// all no-ops unless GTAV_MENU_ENABLE_PAD_INPUT=1.
void gtav_pad_input_park(void);
void gtav_pad_input_unpark(void);
int gtav_pad_input_is_parked(void);

#ifdef __cplusplus
}
#endif
