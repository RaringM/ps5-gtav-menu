// PS4 game-thread pipeline: the PLAYER_PED_ID frame hook and everything it runs once per frame.
//
// The host patches PLAYER_PED_ID with a relative jump to gtav_ps4_frame_hook. Every caller of that
// native (script threads, engine threads such as Hang Detect) arrives here; the original runs first
// through the relocated gateway, and only a call that returned a real player ped, on a new game
// frame, outside a nested call, runs the menu. All GTA natives the menu issues therefore run on the
// script thread that is currently executing PLAYER_PED_ID.

#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/features.h"
#include "gtavmenu/menu_controller.h"
#include "gtavmenu/menu_draw_list.h"
#include "gtavmenu/menu_timing.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/native_invoke.hpp"
#include "gtavmenu/native_job_queue.h"
#include "gtavmenu/native_table_from_header.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/ps4_pad_hook.h"
#include "gtavmenu/ps4_slice.h"
#include GTAV_PS4_TARGET_HEADER

#include <string.h>

GtavNativeJobQueue gtav_ps4_native_jobs;
static GtavPadMapState g_pad_state;
#if GTAV_PS4_ENABLE_TOUCHPAD
static GtavPadTouchMapState g_touch_state;
#endif
static uint32_t g_draining;
static uint32_t g_shutdown_started;
static uint32_t g_features_reset;

using gtavmenu::invoke_return;
using gtavmenu::invoke_void;

static uint32_t host_flags(void) {
  return __atomic_load_n(&gtav_ps4_control.host_flags, __ATOMIC_ACQUIRE);
}

static uint32_t worker_flags(void) {
  return __atomic_load_n(&gtav_ps4_control.worker_flags, __ATOMIC_ACQUIRE);
}

static void set_worker_flags(uint32_t bits) {
  __atomic_or_fetch(&gtav_ps4_control.worker_flags, bits, __ATOMIC_ACQ_REL);
}

// Load time (debugger loader thread): bind addresses and reset state. No native runs here.
void gtav_ps4_game_init(void) {
  gtav_job_queue_init(&gtav_ps4_native_jobs);
  memset(&g_pad_state, 0, sizeof(g_pad_state));
  gtav_pad_input_set_binds(NULL, 0);
#if GTAV_PS4_ENABLE_TOUCHPAD
  memset(&g_touch_state, 0, sizeof(g_touch_state));
  // Published so the host can put the import back itself if a stop never reaches the game thread.
  gtav_ps4_control.pad_hook_address = (uint64_t)(uintptr_t)&gtav_ps4_pad_read_state_hook;
#endif
  g_draining = 0;
  g_shutdown_started = 0;
  g_features_reset = 0;
  GtavNativeAddressTable table;
  gtav_native_table_from_header(&table);
  // The hooked native itself: features calling PLAYER_PED_ID re-enter the hook, which runs the
  // original and skips the nested menu pass (g_draining).
  table.player_ped_id = GTAV_PS4_HOOK_ADDR;
  // The getters bind only after validate_getters proves them on the game thread: bridge init
  // refreshes its snapshot through GET_FRAME_COUNT, and this runs on the debugger's loader thread.
  table.get_frame_count = 0;
  table.get_game_timer = 0;
  gtav_native_bridge_init(&table);
  gtav_features_init(&table);
  gtav_ps4_menu_init();
  gtav_ps4_control.tick_hz = GTAV_MENU_TICK_HZ;
}

static void note_native(uint64_t address) {
  gtav_ps4_control.last_native_address = address;
}

// A readiness failure stops all game work for the rest of the session; the hook keeps chaining
// the original until the host restores it.
static void native_error(uint32_t error) {
  gtav_ps4_control.last_error = error;
  __atomic_store_n(&gtav_ps4_control.native_ready, 0u, __ATOMIC_RELEASE);
  set_worker_flags(GTAV_PS4_WORKER_SELF_QUIESCE);
}

static uint32_t direct_frame_count(void) {
  return *(volatile uint32_t*)(uintptr_t)GTAV_PS4_FRAME_COUNT;
}

static uint32_t direct_game_timer(void) {
  return *(volatile uint32_t*)(uintptr_t)GTAV_PS4_GAME_TIMER;
}

static int text_flags_ready(void) {
  return *(volatile uint32_t*)(uintptr_t)GTAV_PS4_TEXT_FLAGS != 0;
}

// First game frame: the getter natives must agree with the engine counters read directly, which
// proves the address table and the calling convention before anything else runs.
static void validate_getters(void) {
  note_native(GTAV_NATIVE_ADDR_GET_FRAME_COUNT);
  const uint32_t frame = invoke_return<uint32_t>(GTAV_NATIVE_ADDR_GET_FRAME_COUNT);
  note_native(GTAV_NATIVE_ADDR_GET_GAME_TIMER);
  const uint32_t timer = invoke_return<uint32_t>(GTAV_NATIVE_ADDR_GET_GAME_TIMER);
  gtav_ps4_control.getter_frame = frame;
  gtav_ps4_control.getter_timer = timer;
  gtav_ps4_control.direct_frame = direct_frame_count();
  gtav_ps4_control.direct_timer = direct_game_timer();
  if (frame != gtav_ps4_control.direct_frame || timer != gtav_ps4_control.direct_timer) {
    native_error(GTAV_PS4_ERROR_GETTER);
    return;
  }
  gtav_native_bridge_bind_frame_getters(GTAV_NATIVE_ADDR_GET_FRAME_COUNT,
                                        GTAV_NATIVE_ADDR_GET_GAME_TIMER);
  uint32_t ready = GTAV_PS4_NATIVE_GETTERS | GTAV_PS4_NATIVE_DRAW | GTAV_PS4_NATIVE_INPUT;
  if (text_flags_ready()) ready |= GTAV_PS4_NATIVE_TEXT;
  __atomic_or_fetch(&gtav_ps4_control.native_ready, ready, __ATOMIC_ACQ_REL);
}

// Controller sampling through the frontend control group. IS_DISABLED_CONTROL_PRESSED reads the
// physical state even while the menu suppresses these controls from the game.
static uint32_t sample_pad_buttons(void) {
  static const struct {
    int control;
    uint32_t button;
  } kBindings[] = {
      {188, GTAV_PAD_BTN_UP},       {187, GTAV_PAD_BTN_DOWN},  {189, GTAV_PAD_BTN_LEFT},
      {190, GTAV_PAD_BTN_RIGHT},    {201, GTAV_PAD_BTN_CROSS}, {202, GTAV_PAD_BTN_CIRCLE},
      {205, GTAV_PAD_BTN_L1},       {206, GTAV_PAD_BTN_R1},    {203, GTAV_PAD_BTN_SQUARE},
      {204, GTAV_PAD_BTN_TRIANGLE},
  };
  uint32_t buttons = 0;
  for (unsigned i = 0; i < sizeof(kBindings) / sizeof(kBindings[0]); ++i) {
    note_native(GTAV_NATIVE_ADDR_IS_DISABLED_CONTROL_PRESSED);
    if (invoke_return<int>(GTAV_NATIVE_ADDR_IS_DISABLED_CONTROL_PRESSED, 2, kBindings[i].control)) {
      buttons |= kBindings[i].button;
    }
  }
  return buttons;
}

// Optional Touchpad lane (Menu Settings > Touchpad): the scePadReadState swap goes in while the
// setting is on and comes out when it is turned off. Gestures only act while the menu is open, and
// the D-pad always wins.
static uint32_t sample_touch_command(int visible) {
#if GTAV_PS4_ENABLE_TOUCHPAD
  const int enabled = gtav_native_bridge_touchpad_enabled();
  const uint32_t state = gtav_ps4_pad_hook_sync(enabled);
  if (enabled && (state == GTAV_PS4_PAD_HOOK_UNAVAILABLE || state == GTAV_PS4_PAD_HOOK_MISMATCH)) {
    gtav_native_bridge_set_touchpad_enabled(0);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD, "Off");
    gtav_native_bridge_push_toast("touchpad unavailable", GTAV_FEATURE_RESULT_UNAVAILABLE);
    (void)gtav_ps4_pad_hook_sync(0);
    return GTAV_MENU_COMMAND_NONE;
  }
  gtav_ps4_pad_hook_set_hide(enabled && visible);
  if (!enabled || !gtav_ps4_pad_hook_live()) return GTAV_MENU_COMMAND_NONE;
  GtavPs4TouchSample touch;
  gtav_ps4_pad_hook_sample(&touch);
  const uint32_t command =
      gtav_pad_input_map_touch(&g_touch_state, (int)touch.x, (int)touch.y, touch.fingers > 0u,
                               touch.clicked != 0u, 1, visible);
  if (command != GTAV_MENU_COMMAND_NONE) gtav_ps4_control.touch_commands++;
  return command;
#else
  (void)visible;
  return GTAV_MENU_COMMAND_NONE;
#endif
}

static uint32_t sample_pad_command(void) {
  const uint32_t buttons = sample_pad_buttons();
  const int visible = gtav_menu_controller_visible();
  // While the menu is open the shared feature tick suppresses every navigation control. While it
  // is closed, swallow D-pad Left during the R1+Left open chord so the game does not act on it.
  if (!visible && (buttons & GTAV_PAD_OPEN_CHORD_MASK) == GTAV_PAD_OPEN_CHORD_MASK) {
    note_native(GTAV_NATIVE_ADDR_DISABLE_CONTROL_ACTION);
    invoke_void(GTAV_NATIVE_ADDR_DISABLE_CONTROL_ACTION, 2, 189, 1);
    invoke_void(GTAV_NATIVE_ADDR_DISABLE_CONTROL_ACTION, 0, 189, 1);
  }
  const uint32_t command = gtav_pad_input_map_live(
      &g_pad_state, buttons, visible, visible && gtav_native_bridge_selected_is_cycler());
  return command != GTAV_MENU_COMMAND_NONE ? command : sample_touch_command(visible);
}

static void mirror_draw_counts(void) {
  uint64_t rects = 0, texts = 0, sprites = 0, lists = 0;
  gtav_native_bridge_phase_draw_counts(&rects, &texts, &sprites, &lists);
  gtav_ps4_control.draw_rects = rects;
  gtav_ps4_control.draw_texts = texts;
  gtav_ps4_control.draw_sprites = sprites;
  gtav_ps4_control.draw_lists = lists;
}

// Every frame, menu open or not: the published list also carries toasts and overlays.
static void draw_frame(void) {
  if (gtav_ps4_control.native_ready != GTAV_PS4_NATIVE_ALL) return;
  (void)gtav_native_bridge_phase_tick(gtav_ps4_control.game_frames, gtav_menu_controller_visible());
  mirror_draw_counts();
}

// Bring-up mode (the dev lane's draw-only mode): one rectangle and one line of text for 120 frames,
// through the same draw-list consumer as the menu.
static void diagnostic_frame(void) {
  if (worker_flags() & GTAV_PS4_WORKER_DIAG_DONE) return;
  if ((gtav_ps4_control.native_ready & (GTAV_PS4_NATIVE_DRAW | GTAV_PS4_NATIVE_TEXT)) !=
      (GTAV_PS4_NATIVE_DRAW | GTAV_PS4_NATIVE_TEXT)) {
    return;
  }
  GtavMenuDrawList* list = gtav_menu_draw_list_begin(gtav_ps4_control.game_frames);
  if (!list) return;
  (void)gtav_menu_draw_list_add_rect(list, 0.50f, 0.45f, 0.20f, 0.10f, 20, 120, 220, 200);
  (void)gtav_menu_draw_list_add_text(list, "PS4 DRAW OK", 0.42f, 0.425f, 0.35f, 0.35f, 0, 255, 255,
                                     255, 255, 0, 0.12f, 0.88f, 1, 0, 0);
  (void)gtav_menu_draw_list_publish(list);
  (void)gtav_native_bridge_phase_tick(gtav_ps4_control.game_frames, 0);
  mirror_draw_counts();
  if (++gtav_ps4_control.diag_frames >= 120u) {
    GtavMenuDrawList* empty = gtav_menu_draw_list_begin(gtav_ps4_control.game_frames + 1u);
    if (empty) (void)gtav_menu_draw_list_publish(empty);
    set_worker_flags(GTAV_PS4_WORKER_DIAG_DONE);
  }
}

static void run_job(uint32_t action, uint32_t param, void* context) {
  gtav_features_run_job(action, param, context);
  gtav_ps4_control.jobs_run++;
}

static void drain_jobs(void) {
  (void)gtav_job_queue_drain(&gtav_ps4_native_jobs, GTAV_PS4_JOBS_PER_FRAME, run_job, NULL);
}

// Stop sequence on the game thread (host SHUTDOWN_REQUEST or the menu's Stop row): hide the menu,
// turn every feature off, keep ticking until owned entities are released (bounded), drop queued
// jobs, publish an empty list, then stop all game work. The host restores the hook afterwards.
static void shutdown_frame(void) {
  if (!g_shutdown_started) {
    g_shutdown_started = 1;
    gtav_ps4_menu_hide();
    gtav_features_shutdown_begin();
    gtav_ps4_control.shutdown_frames = 0;
#if GTAV_PS4_ENABLE_TOUCHPAD
    // The import goes back first; the stop completes only once no read is inside the hook.
    (void)gtav_ps4_pad_hook_sync(0);
#endif
  }
  gtav_ps4_control.shutdown_frames++;
  gtav_features_game_thread_tick();
  drain_jobs();
  const int busy = gtav_features_shutdown_busy() || !gtav_ps4_pad_hook_idle();
  if (busy && gtav_ps4_control.shutdown_frames < GTAV_PS4_SHUTDOWN_MAX_FRAMES) return;
  if (busy) gtav_ps4_control.last_error = GTAV_PS4_ERROR_SHUTDOWN_TIMEOUT;
  gtav_features_shutdown_finish();
  GtavMenuDrawList* empty = gtav_menu_draw_list_begin(gtav_ps4_control.game_frames + 1u);
  if (empty) (void)gtav_menu_draw_list_publish(empty);
  set_worker_flags(GTAV_PS4_WORKER_SHUTDOWN_DONE | GTAV_PS4_WORKER_SELF_QUIESCE);
}

// No menu pass ran a feature yet: a stop needs no feature shutdown, only the empty list.
static void acknowledge_stop(void) {
  GtavMenuDrawList* empty = gtav_menu_draw_list_begin(gtav_ps4_control.game_frames + 1u);
  if (empty) (void)gtav_menu_draw_list_publish(empty);
  set_worker_flags(GTAV_PS4_WORKER_SHUTDOWN_DONE | GTAV_PS4_WORKER_SELF_QUIESCE);
}

static void game_frame(void) {
  gtav_ps4_control.game_frames++;
  if ((gtav_ps4_control.native_ready & GTAV_PS4_NATIVE_GETTERS) == 0) {
    validate_getters();
    return;
  }
  if (text_flags_ready()) {
    __atomic_or_fetch(&gtav_ps4_control.native_ready, GTAV_PS4_NATIVE_TEXT, __ATOMIC_ACQ_REL);
  }
  const uint32_t host = host_flags();
  if (host & GTAV_PS4_HOST_DIAG_DRAW) diagnostic_frame();
  const int stop =
      (host & GTAV_PS4_HOST_SHUTDOWN_REQUEST) || (worker_flags() & GTAV_PS4_WORKER_MENU_STOP);
  if ((host & GTAV_PS4_HOST_UI_ALLOWED) == 0) {
    // Bring-up modes (getters-only, draw-only) never activated a feature: acknowledge a stop at
    // once.
    if (host & GTAV_PS4_HOST_SHUTDOWN_REQUEST) acknowledge_stop();
    return;
  }
  if (g_shutdown_started) {
    shutdown_frame();
    return;
  }
  // Nothing of the menu, its features, its jobs or the pad runs before every native family proved
  // itself (the activation gate): no feature can be on yet, so a stop is only acknowledged.
  if (gtav_ps4_control.native_ready != GTAV_PS4_NATIVE_ALL) {
    if (stop) acknowledge_stop();
    return;
  }
  if (stop) {
    shutdown_frame();
    return;
  }
  // An earlier instance in this process stopped without its cooperative shutdown: turn every
  // feature off once, before this instance imports the profile.
  if ((host & GTAV_PS4_HOST_RESET_FEATURES) && !g_features_reset) {
    g_features_reset = 1;
    (void)gtav_features_activate(GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL);
    set_worker_flags(GTAV_PS4_WORKER_FEATURES_RESET);
  }
  const uint32_t pad = sample_pad_command();
  gtav_ps4_menu_frame(pad);
#if GTAV_PS4_ENABLE_TOUCHPAD
  // The menu may have opened or closed this frame: the game's next pad read sees the new state.
  gtav_ps4_pad_hook_set_hide(gtav_native_bridge_touchpad_enabled() &&
                             gtav_menu_controller_visible());
#endif
  draw_frame();
  gtav_features_game_thread_tick();
  drain_jobs();
}

void gtav_ps4_frame_hook(void* native_context) {
  __atomic_add_fetch(&gtav_ps4_control.inflight, 1u, __ATOMIC_ACQ_REL);
  __atomic_add_fetch(&gtav_ps4_control.hook_calls, 1u, __ATOMIC_RELAXED);
  const int can_run = (host_flags() & GTAV_PS4_HOST_QUIESCE) == 0 &&
                      (worker_flags() & GTAV_PS4_WORKER_SELF_QUIESCE) == 0;
  __atomic_store_n(&gtav_ps4_control.quiesced, can_run ? 0u : 1u, __ATOMIC_RELEASE);

  // Original first. PLAYER_PED_ID is also called by engine threads (Hang Detect among them) that
  // have no player/script context; the handle it returns is the context gate, so nothing below
  // runs unless the original produced a real ped.
  uint32_t player_ped = 0;
  void* gateway = (void*)(uintptr_t)gtav_ps4_control.gateway_address;
  if (gateway) {
    ((void (*)(void*))gateway)(native_context);
    gtavmenu::NativeArg* args = (gtavmenu::NativeArg*)native_context;
    if (args && args->return_value) player_ped = *(uint32_t*)args->return_value;
    if (player_ped) gtav_ps4_control.captured_ped = player_ped;
  } else {
    native_error(GTAV_PS4_ERROR_GATEWAY);
  }
  if (can_run && player_ped && __atomic_exchange_n(&g_draining, 1u, __ATOMIC_ACQUIRE) == 0) {
    const uint32_t frame = direct_frame_count();
    if (frame != gtav_ps4_control.last_frame_count) {
      gtav_ps4_control.last_frame_count = frame;
      game_frame();
    }
    __atomic_store_n(&g_draining, 0u, __ATOMIC_RELEASE);
  }
  __atomic_sub_fetch(&gtav_ps4_control.inflight, 1u, __ATOMIC_ACQ_REL);
}
