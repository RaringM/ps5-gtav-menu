// Tag this translation unit's GTAV_LOG*/INIT_TRACE lines as the menu subsystem. Must precede
// the first include, since features.h pulls in log.h (which else defaults the category).
#define GTAV_LOG_DEFAULT_CATEGORY GTAV_LOG_CAT_MENU

#include "gtavmenu/menu.h"
#include "gtavmenu/render_bank_probe.h"
#include "gtavmenu/render_cycle_probe.h"
#include "gtavmenu/render_diag.h"
#include "gtavmenu/render_path_probe.h"
#include "gtavmenu/render_phase_intercept.h"

#include "gtavmenu/abi.h"
#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/detour.h"
#include "gtavmenu/feature_catalog.h"
#include "gtavmenu/features.h"
#include "gtavmenu/frame_hook.h"
#include "gtavmenu/inject_lock.h"
#include "gtavmenu/log.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/notify.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/patch_broker.h"
#include "gtavmenu/quit_guard.h"
#include "gtavmenu/rootdir.h"
#include "gtavmenu/runtime_config.h"
#include "gtavmenu/spawn_module_loader.h"
#include "gtavmenu/status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef GTAV_MENU_LOG_TICK_INTERVAL
#define GTAV_MENU_LOG_TICK_INTERVAL 0
#endif

#ifndef GTAV_MENU_ENABLE_LIVE_HOOK
#define GTAV_MENU_ENABLE_LIVE_HOOK 0
#endif

#ifndef GTAV_MENU_ENABLE_HOOK_DRY_RUN
#define GTAV_MENU_ENABLE_HOOK_DRY_RUN 0
#endif

#ifndef GTAV_MENU_HOST_VALIDATED_HOOK_DRY_RUN
#define GTAV_MENU_HOST_VALIDATED_HOOK_DRY_RUN 0
#endif

#ifndef GTAV_MENU_HOST_VALIDATED_LIVE_HOOK
#define GTAV_MENU_HOST_VALIDATED_LIVE_HOOK 0
#endif

#ifndef GTAV_MENU_LIVE_HOOK_STAGE
#define GTAV_MENU_LIVE_HOOK_STAGE 0
#endif

#define GTAV_MENU_LIVE_HOOK_STAGE_INSTALL 0
#define GTAV_MENU_LIVE_HOOK_STAGE_GATEWAY 1
#define GTAV_MENU_LIVE_HOOK_STAGE_PROTECT 2

#ifndef GTAV_MENU_ENABLE_NATIVE_FEATURES
#define GTAV_MENU_ENABLE_NATIVE_FEATURES 0
#endif

#ifndef GTAV_MENU_HOOK_SKIP_ORIGINAL
#define GTAV_MENU_HOOK_SKIP_ORIGINAL 0
#endif

#ifndef GTAV_MENU_HOOK_NATIVE_HANDLER_SLOT
#define GTAV_MENU_HOOK_NATIVE_HANDLER_SLOT 0
#endif

#ifndef GTAV_MENU_HANDLER_SLOT_MIN_ADDR
#define GTAV_MENU_HANDLER_SLOT_MIN_ADDR 0
#endif

#ifndef GTAV_MENU_DISABLE_NOTIFICATIONS
#define GTAV_MENU_DISABLE_NOTIFICATIONS 0
#endif

#ifndef GTAV_MENU_SHUTDOWN_INPUT_RELEASE_USEC
#define GTAV_MENU_SHUTDOWN_INPUT_RELEASE_USEC 2000000u
#endif

typedef void (*GtavControlUpdateStepFn)(void);
typedef void (*GtavDrawRectNativeFn)(void* native_arg);

static GtavMenuInit g_init;
static GtavDetour g_frame_detour;
static GtavControlUpdateStepFn g_control_update_step_original;
static GtavDrawRectNativeFn g_draw_rect_native_original;
static uint64_t g_handler_slot_addr;
static uint64_t g_handler_slot_original;
static int g_handler_slot_installed;
static volatile unsigned long long g_ticks;
static volatile unsigned int g_hook_side_ticks;
#if GTAV_MENU_ENABLE_FRAME_HOOK
// Set once the worker has mirrored the first game-thread probe fire to the log ring.
static int g_frame_probe_logged;
#endif
static volatile int g_visible;
static volatile int g_initialized;
static volatile int g_stop_requested;

static void* selected_hook_body(void) {
  return (void*)gtav_menu_control_update_step_hook;
}

static void set_trampoline_original(void* trampoline_original) {
  g_control_update_step_original = (GtavControlUpdateStepFn)trampoline_original;
}

static void publish_trampoline_original(void* trampoline_original, void* context) {
  (void)context;
  set_trampoline_original(trampoline_original);
}

static int expected_handler_pointer(uint64_t* out) {
  uint64_t value = 0;

  if (!out || g_init.expected_len != sizeof(value)) {
    return -1;
  }

  memcpy(&value, g_init.expected, sizeof(value));
  if (!value) {
    return -1;
  }
  *out = value;
  return 0;
}

static int install_native_handler_slot(void* hook_body, int dry_run) {
  uint64_t expected = 0;
  uint64_t current = 0;
  uint64_t replacement = (uint64_t)(uintptr_t)hook_body;
  uintptr_t min_addr = (uintptr_t)GTAV_MENU_HANDLER_SLOT_MIN_ADDR;
  uint64_t* slot;

  if (!hook_body || expected_handler_pointer(&expected) != 0) {
    gtav_status_set_error(GTAV_MENU_ERROR_HOOK_MISSING_EXPECTED,
                          "handler slot expected pointer missing");
    gtav_logf("handler slot expected pointer missing");
    return -1;
  }
  if (!g_init.hook_addr || (g_init.hook_addr & 7u)) {
    gtav_status_set_error(GTAV_MENU_ERROR_HOOK_VALIDATION_FAILED,
                          "handler slot address is not aligned");
    gtav_logf("handler slot address invalid: 0x%llx", (unsigned long long)g_init.hook_addr);
    return -1;
  }
  if (min_addr && g_init.hook_addr < min_addr) {
    gtav_status_set_error(GTAV_MENU_ERROR_HOOK_VALIDATION_FAILED,
                          "handler slot address below minimum");
    gtav_logf("handler slot address below minimum: 0x%llx min=0x%llx",
              (unsigned long long)g_init.hook_addr, (unsigned long long)min_addr);
    return -1;
  }

  slot = (uint64_t*)(uintptr_t)g_init.hook_addr;
  current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (current != expected) {
    gtav_status_set_error(GTAV_MENU_ERROR_HOOK_VALIDATION_FAILED, "handler slot pointer mismatch");
    gtav_logf("handler slot validation failed at 0x%llx current=0x%llx expected=0x%llx",
              (unsigned long long)g_init.hook_addr, (unsigned long long)current,
              (unsigned long long)expected);
    return -1;
  }

  g_handler_slot_addr = g_init.hook_addr;
  g_handler_slot_original = expected;

  if (dry_run) {
    gtav_status_set_hook(GTAV_MENU_HOOK_DRY_RUN_PASSED, (uintptr_t)g_init.hook_addr,
                         sizeof(expected), (const uint8_t*)&expected, sizeof(expected));
    gtav_status_event(GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED, "native handler slot dry-run passed");
    return 0;
  }

  __atomic_store_n(slot, replacement, __ATOMIC_RELEASE);
  g_handler_slot_installed = 1;
  gtav_status_set_hook(GTAV_MENU_HOOK_INSTALLED, (uintptr_t)g_init.hook_addr, sizeof(expected),
                       (const uint8_t*)&expected, sizeof(expected));
  gtav_status_eventf(GTAV_MENU_EVENT_HOOK_INSTALLED,
                     "native handler slot installed replacement=0x%llx original=0x%llx",
                     (unsigned long long)replacement, (unsigned long long)expected);
  return 0;
}

static int restore_native_handler_slot(void) {
  uint64_t* slot;

  if (!g_handler_slot_installed || !g_handler_slot_addr) {
    return 0;
  }

  slot = (uint64_t*)(uintptr_t)g_handler_slot_addr;
  __atomic_store_n(slot, g_handler_slot_original, __ATOMIC_RELEASE);
  g_handler_slot_installed = 0;
  gtav_status_set_hook(GTAV_MENU_HOOK_RESTORED, (uintptr_t)g_handler_slot_addr,
                       sizeof(g_handler_slot_original), (const uint8_t*)&g_handler_slot_original,
                       sizeof(g_handler_slot_original));
  gtav_status_event(GTAV_MENU_EVENT_HOOK_INSTALLED, "native handler slot restored");
  return 0;
}

static int native_table_has_addresses(const GtavNativeAddressTable* table) {
  if (!table || table->abi_version != GTAV_NATIVE_BRIDGE_ABI_VERSION) return 0;
  return table->get_frame_count || table->is_control_pressed || table->is_control_just_pressed ||
         table->draw_rect || table->begin_text_command_display_text ||
         table->add_text_component_substring_player_name || table->end_text_command_display_text ||
         table->set_text_scale || table->set_text_colour || table->set_text_font ||
         table->set_text_centre || table->set_text_wrap || table->set_text_justification ||
         table->set_text_drop_shadow || table->set_text_dropshadow || table->set_text_outline ||
         table->is_disabled_control_pressed || table->is_disabled_control_just_pressed ||
         table->is_disabled_control_just_released || table->disable_control_action ||
         table->set_input_exclusive || table->get_game_timer || table->draw_sprite ||
         table->has_streamed_texture_dict_loaded || table->request_streamed_texture_dict ||
         table->player_id || table->player_ped_id || table->set_player_invincible ||
         table->set_entity_invincible || table->set_entity_health || table->set_ped_armour ||
         table->set_player_wanted_level || table->set_player_wanted_level_now;
}

static void menu_notify(const char* message) {
#if GTAV_MENU_DISABLE_NOTIFICATIONS
  (void)message;
#else
  gtav_notify(message);
#endif
}

static void request_stop(const char* reason) {
  g_stop_requested = 1;
  gtav_status_set_stop_requested(1);
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "stop requested: %s", reason ? reason : "unknown");
}

#if GTAV_MENU_ENABLE_NATIVE_FEATURES
/* Push the current value of each list-cycler into the menu so it can
 * render "< VALUE >". Mirrors the feature-toggle-mask push. Only built when native
 * features are on (its sole callers are too). */
static void push_list_values(void) {
  /* Single-source the list-row values off gtav_features_value_label(): it returns a non-empty
     label for exactly the cycler actions ("" otherwise), so pushing every action it labels
     keeps this in lockstep with the value computer -- no parallel list to drift. The
     category/theme/region cyclers are owned by native_bridge.cpp (not labelled here), so they
     are naturally skipped. */
  for (uint32_t action = 3; action < GTAV_NATIVE_SHELL_ACTION_COUNT; ++action) {
    const char* value = gtav_features_value_label(action);
    if (value && value[0]) {
      gtav_native_bridge_set_list_value(action, value);
    }
  }
}
#endif

#if GTAV_MENU_ENABLE_NATIVE_FEATURES
/* Surface the just-executed feature's result (set by features.cpp's
   ok()/unavailable()/failed() chokepoint) as an on-screen toast. No-op if the
   action produced no result (e.g. pure navigation). */
static void push_action_toast(void) {
  GtavFeatureState st;
  gtav_features_snapshot(&st);
  if (st.last_result != GTAV_FEATURE_RESULT_NONE) {
    gtav_native_bridge_push_toast(st.last_message, st.last_result);
  }
}
#endif

/* Forward decl: the Worker Hz cycler (handled below) applies via set_worker_hz, which is
   defined later in this file alongside the worker-loop period state. */
static void set_worker_hz(uint32_t hz);

/* Worker Hz + Toast Time cycler choices, hoisted to file scope so the cyclers (below) and the
   init-time restore (apply_view_settings_from_profile) share one source of truth. The selected
   index for each lives in the native bridge, so it round-trips through the feature profile (v11)
   instead of resetting to default on every re-inject. */
static const uint32_t kWorkerHzChoices[] = {60u, 72u, 90u, 120u};
static const uint32_t kToastTicks[] = {120u, 180u, 270u, 360u};
static const char* const kToastLabels[] = {"2.0 s", "3.0 s", "4.5 s", "6.0 s"};
#define GTAV_MENU_WORKER_HZ_COUNT \
  ((uint32_t)(sizeof(kWorkerHzChoices) / sizeof(kWorkerHzChoices[0])))
#define GTAV_MENU_TOAST_COUNT ((uint32_t)(sizeof(kToastTicks) / sizeof(kToastTicks[0])))

/* v13 feel/accessibility cyclers. Motion is a 2-way (Full/Reduced) flag held in the bridge (the
   draw path reads it). Nav Delay / Nav Speed tune the live D-pad auto-repeat cadence (poll ticks;
   smaller rate = faster); index 1 == the historical defaults (delay 14, rate 4), so a memset-0
   pre-v13 profile version-gates to those. Like Worker Hz, the selected index lives in the bridge so
   it round-trips through the profile; menu.c owns the tables and applies them to the pad mapper via
   gtav_pad_input_set_repeat. */
static const char* const kMotionLabels[] = {"Full", "Reduced"};
static const uint32_t kNavDelayChoices[] = {8u, 14u, 22u};
static const char* const kNavDelayLabels[] = {"Short", "Normal", "Long"};
static const uint32_t kNavSpeedChoices[] = {6u, 4u, 2u};
static const char* const kNavSpeedLabels[] = {"Slow", "Normal", "Fast"};
#define GTAV_MENU_NAV_DELAY_COUNT \
  ((uint32_t)(sizeof(kNavDelayChoices) / sizeof(kNavDelayChoices[0])))
#define GTAV_MENU_NAV_SPEED_COUNT \
  ((uint32_t)(sizeof(kNavSpeedChoices) / sizeof(kNavSpeedChoices[0])))

/* Map the persisted Nav Delay / Nav Speed indices to the live pad auto-repeat cadence. Called from
   init and whenever either cycler changes. Works in every build (a no-op on the cadence in builds
   without the live pad path, since the pure mapper takes its timing as parameters). */
static void apply_nav_repeat(void) {
  uint32_t di = gtav_native_bridge_nav_delay_index() % GTAV_MENU_NAV_DELAY_COUNT;
  uint32_t si = gtav_native_bridge_nav_speed_index() % GTAV_MENU_NAV_SPEED_COUNT;
  gtav_pad_input_set_repeat(kNavDelayChoices[di], kNavSpeedChoices[si]);
}

#if GTAV_MENU_ENABLE_NATIVE_FEATURES
/* Apply the Worker Hz / Toast Time selections restored from the profile, run once after
   gtav_features_init loads it. Clamps each bridge-held index to its choice table, actuates the
   value (worker period / toast dwell), and seeds the cycler's "< value >" display so the rows
   show the restored setting before the user touches them. */
static void apply_view_settings_from_profile(void) {
  uint32_t whz = gtav_native_bridge_worker_hz_index() % GTAV_MENU_WORKER_HZ_COUNT;
  uint32_t ti = gtav_native_bridge_toast_time_index() % GTAV_MENU_TOAST_COUNT;
  char label[16];
  gtav_native_bridge_set_worker_hz_index(whz);
  gtav_native_bridge_set_toast_time_index(ti);
  set_worker_hz(kWorkerHzChoices[whz]);
  gtav_native_bridge_set_toast_ticks(kToastTicks[ti]);
  snprintf(label, sizeof(label), "%u Hz", (unsigned)kWorkerHzChoices[whz]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ, label);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME, kToastLabels[ti]);
  // v13: Reduce Motion + Nav Delay/Speed. Clamp the restored indices, actuate the pad repeat
  // cadence, and seed the cycler displays. (Reduce Motion needs no actuation -- the draw path reads
  // the bridge flag directly.)
  uint32_t nd = gtav_native_bridge_nav_delay_index() % GTAV_MENU_NAV_DELAY_COUNT;
  uint32_t ns = gtav_native_bridge_nav_speed_index() % GTAV_MENU_NAV_SPEED_COUNT;
  gtav_native_bridge_set_nav_delay_index(nd);
  gtav_native_bridge_set_nav_speed_index(ns);
  apply_nav_repeat();
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION,
                                    kMotionLabels[gtav_native_bridge_reduce_motion() ? 1 : 0]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY, kNavDelayLabels[nd]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED, kNavSpeedLabels[ns]);
}
#endif

/* Setting cyclers for render cadence + toast dwell have no immediately visible effect (unlike
   Theme/Draw Side, which you SEE change), so a Cross "apply" press (emit_toast=1) confirms the
   new value with a toast -- the same acknowledgement every other row gives. Left/Right
   (emit_toast=0) stay quiet and rely on the live "< value >" display, matching the cycler
   convention. Works in every build: push_toast is a no-op when toasts are built out, and these
   cyclers are pure bridge state (no native-feature gate). */
static void push_setting_toast(int emit_toast, const char* name, const char* value) {
  if (!emit_toast) return;
  char msg[80];
  snprintf(msg, sizeof(msg), "%s: %s", name, value);
  gtav_native_bridge_push_toast(msg, GTAV_FEATURE_RESULT_OK);
}

/* Destructive one-shots that wipe state the user can't easily get back. Activating one of
   these from the controller arms a one-press confirm (see below) instead of firing immediately,
   so a stray Cross while scrolling can't nuke a built-up scene / kill the player. */
static int action_needs_confirm(uint32_t action) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL:
    case GTAV_NATIVE_SHELL_ACTION_KILL_SELF:
    case GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL:
      return 1;
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
    case GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT:
      /* Overwriting an occupied garage/outfit slot arms the confirm; saving to an empty slot
         fires immediately (nothing to lose). */
      return gtav_features_save_slot_occupied(action);
#endif
    default:
      return 0;
  }
}

/* Cyclers whose Left/Right step ACTUATES the feature immediately (not just stages a value that is
   applied later on Cross), so a value change deserves the same toast acknowledgement Cross gives --
   otherwise the user can't tell the change took effect (test note T1, e.g. "Fly: Off/On/Fast"
   silently switching mode). Pure-staging cyclers (LSC tiers, paint, spawn catalogs, cash) apply
   only on Cross and stay quiet on the step; menu-chrome cyclers (Theme/Draw Side/Menu Width, whose
   effect you SEE on screen) are likewise deliberately quiet. Keep this in sync when adding a cycler
   that actuates on the step (see the menu feature-add touch-points). */
static int action_applies_on_change(uint32_t action) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA:
      return 1;
    default:
      return 0;
  }
}

/* The action awaiting its confirming second press (0 = none). Armed on the first interactive
   activation of a destructive row and consumed by the second; any other interactive activation
   clears it, so it never lingers across navigation to an unrelated row. */
static uint32_t g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE;

/* emit_toast distinguishes an apply/activate (Cross, direct action -> confirm with
   a toast) from a left/right cycler adjustment (just stages/scrolls a value -> stay
   quiet; the displayed "< value >" still refreshes via push_list_values). */
static void handle_shell_action_param(uint32_t action, uint32_t param, int emit_toast) {
  /* One-press confirm for destructive rows, controller-side only. Left/right cycler steps
     (param != 0) are never gated, and the mailbox lane pre-confirms its action. */
  if (emit_toast && param == 0u && action_needs_confirm(action)) {
    if (g_pending_confirm_action != action) {
      g_pending_confirm_action = action;
      char msg[80];
      snprintf(msg, sizeof(msg), "Press again to confirm: %s",
               gtav_native_bridge_action_name(action));
      gtav_native_bridge_push_toast(msg, GTAV_FEATURE_RESULT_UNAVAILABLE);
      return;
    }
    g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE; /* confirmed -> fall through + run */
  } else if (emit_toast && param == 0u) {
    g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE; /* any other activation disarms */
  }
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_HIDE:
      gtav_menu_set_visible(0);
      return;
    case GTAV_NATIVE_SHELL_ACTION_STOP:
      request_stop("shell action");
      return;
    case GTAV_NATIVE_SHELL_ACTION_NONE:
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      /* Submenu entry, BACK navigation, and non-cycler Left/Right all dispatch NONE through here.
         Re-pull the live cycler values so a submenu that mirrors engine state (e.g. the LSC
         Performance / Bodywork mod-tier rows, read via GET_VEHICLE_MOD) shows the vehicle's actual
         configuration the moment it becomes visible -- previously the rows stayed stale until the
         user nudged one (the only other push_list_values site is the feature-dispatch path). Reads
         worker-safe getters only, fires no toast/dispatch, and runs only on discrete button input.
       */
      push_list_values();
#endif
      return;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY:
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_activate_param(action, param);
      if (emit_toast) push_action_toast();
#else
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell action=%s acknowledged",
                         gtav_native_bridge_action_name(action));
#endif
      return;
    /* Menu customisation cyclers: pure bridge render-state, no game native, so they work
       in every build (independent of GTAV_MENU_ENABLE_NATIVE_FEATURES). Cycler param: 2 =
       prev (Left), 0/1 = next (Cross/Right). The live "< value >" updates via set_list_value;
       no toast (matches the quiet left/right cycle convention). */
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_THEME: {
      uint32_t count = gtav_native_bridge_theme_count();
      uint32_t cur = gtav_native_bridge_theme();
      uint32_t next = (param == 2u) ? (cur + count - 1u) % count : (cur + 1u) % count;
      gtav_native_bridge_set_theme(next);
      gtav_native_bridge_set_list_value(action, gtav_native_bridge_theme_label(next));
      /* Autosave on change so chrome settings stick like favorites/Quick pins do, instead of
         only persisting on a manual Runtime > Save Profile. */
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_RESET_CUSTOM_THEME:
      /* Re-seed the Custom theme to the default palette (the safety net for an unreadable custom
         colour). Pure bridge render-state; if the Custom slot is active the recolour previews
         live. Persisted via the Theme Editor's exit-save, but save here too for the explicit
         action. */
      gtav_native_bridge_reset_custom_theme();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      push_setting_toast(emit_toast, "Custom Theme", "Reset");
      return;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_REGION: {
      uint32_t count = gtav_native_bridge_region_count();
      uint32_t cur = gtav_native_bridge_region();
      uint32_t next = (param == 2u) ? (cur + count - 1u) % count : (cur + 1u) % count;
      gtav_native_bridge_set_region(next);
      gtav_native_bridge_set_list_value(action, gtav_native_bridge_region_label(next));
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // autosave on change (see CYCLE_THEME)
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PANEL_WIDTH: {
      /* Menu panel width: immediate-effect render-state cycler (like CYCLE_REGION). Left/Right
         (param 2/1) and Cross (0) all step it; the live "< value >" updates and the change is
         autosaved so the width persists across re-injects (profile v16). */
      uint32_t count = gtav_native_bridge_panel_width_count();
      uint32_t cur = gtav_native_bridge_panel_width_index();
      uint32_t next = (param == 2u) ? (cur + count - 1u) % count : (cur + 1u) % count;
      gtav_native_bridge_set_panel_width_index(next);
      gtav_native_bridge_set_list_value(action, gtav_native_bridge_panel_width_label(next));
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // autosave on change (see CYCLE_THEME)
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ: {
      /* Off-thread render over-sample rate: raising it past the game frame rate hides the
         phase-beat flicker. STAGE/APPLY like the gameplay sliders (world.inc): Left/Right
         (param 2/1) only pick the value; Cross (param 0) applies it via set_worker_hz. The
         selection index lives in the bridge so it persists across re-injects (profile v11). */
      const uint32_t count = GTAV_MENU_WORKER_HZ_COUNT;
      uint32_t idx = gtav_native_bridge_worker_hz_index() % count;
      char label[16];
      if (param == 1u || param == 2u) {  // stage only -- do not apply yet
        idx = (param == 2u) ? (idx + count - 1u) % count : (idx + 1u) % count;
        gtav_native_bridge_set_worker_hz_index(idx);
        snprintf(label, sizeof(label), "%u Hz", (unsigned)kWorkerHzChoices[idx]);
        gtav_native_bridge_set_list_value(action, label);
        return;
      }
      set_worker_hz(kWorkerHzChoices[idx]);  // apply the staged value
      snprintf(label, sizeof(label), "%u Hz", (unsigned)kWorkerHzChoices[idx]);
      push_setting_toast(emit_toast, "Worker Hz", label);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // autosave on apply so the tuned rate survives a
                                             // re-inject
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME: {
      /* Toast dwell, in worker ticks (labelled at the 60 Hz default; the dwell scales with
         worker_hz). Default index 2 == 270 ticks, matching the g_toast_ticks default. The
         selection index lives in the bridge so it persists across re-injects (profile v11). */
      const uint32_t count = GTAV_MENU_TOAST_COUNT;
      uint32_t idx = gtav_native_bridge_toast_time_index() % count;
      if (param == 1u || param == 2u) {  // stage only -- do not apply yet
        idx = (param == 2u) ? (idx + count - 1u) % count : (idx + 1u) % count;
        gtav_native_bridge_set_toast_time_index(idx);
        gtav_native_bridge_set_list_value(action, kToastLabels[idx]);
        return;
      }
      gtav_native_bridge_set_toast_ticks(kToastTicks[idx]);  // apply the staged value
      push_setting_toast(emit_toast, "Toast Time", kToastLabels[idx]);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();  // autosave on apply
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION: {
      /* Reduce Motion: a 2-way Full/Reduced flag held in the bridge (read by the draw path). The
         effect is immediately visible, so cycle on any of Left/Right/Cross like Theme/Region (no
         stage/apply). Autosave on change. */
      int on = !gtav_native_bridge_reduce_motion();
      gtav_native_bridge_set_reduce_motion(on);
      gtav_native_bridge_set_list_value(action, kMotionLabels[on ? 1 : 0]);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD: {
      /* Optional touchpad gesture input (Off/On). Bridge flag read by the live pad poll path;
         default off until calibrated on hardware. 2-way like Motion. Autosave on change. */
      int on = !gtav_native_bridge_touchpad_enabled();
      gtav_native_bridge_set_touchpad_enabled(on);
      gtav_native_bridge_set_list_value(action, on ? "On" : "Off");
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY: {
      /* D-pad auto-repeat start delay. Cycler param: 2 = prev (Left), 0/1 = next (Cross/Right).
         Applies immediately to the pad mapper + autosaves; the index persists (profile v13). */
      const uint32_t count = GTAV_MENU_NAV_DELAY_COUNT;
      uint32_t idx = gtav_native_bridge_nav_delay_index() % count;
      idx = (param == 2u) ? (idx + count - 1u) % count : (idx + 1u) % count;
      gtav_native_bridge_set_nav_delay_index(idx);
      gtav_native_bridge_set_list_value(action, kNavDelayLabels[idx]);
      apply_nav_repeat();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      return;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED: {
      /* D-pad auto-repeat speed (smaller rate = faster). See CYCLE_NAV_DELAY. */
      const uint32_t count = GTAV_MENU_NAV_SPEED_COUNT;
      uint32_t idx = gtav_native_bridge_nav_speed_index() % count;
      idx = (param == 2u) ? (idx + count - 1u) % count : (idx + 1u) % count;
      gtav_native_bridge_set_nav_speed_index(idx);
      gtav_native_bridge_set_list_value(action, kNavSpeedLabels[idx]);
      apply_nav_repeat();
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      gtav_features_profile_save_default();
#endif
      return;
    }
    default:
      break;
  }

    /* Everything else is a native gameplay feature (god mode, vehicle spawn,
       weapons, movement). Gated behind the feature build so default builds make
       no native calls. */
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  /* Game-thread-sensitive actions (vehicle spawn, weapons, skin) execute through
     gtav_features_activate_param; their game-thread work is queued as a job and drained
     by the frame hook in valid script context. gtav_features_action_is_gated() is the
     UI lock predicate that keeps such a row selectable only once the hook is live. */
  gtav_features_activate_param(action, param);
  gtav_native_bridge_set_feature_toggles(gtav_features_toggle_mask());
  push_list_values();
  if (emit_toast) push_action_toast();
#else
  (void)param;
  (void)emit_toast;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "feature unavailable action=%s reason=native feature build disabled",
                     gtav_native_bridge_action_name(action));
#endif
}

static void handle_shell_action(uint32_t action) {
  handle_shell_action_param(action, 0, 1);
}

// Coarse-step a value cycler several values in one L1/R1 press (paint/livery lists are long).
// Reuses the fully-tested single-step adjust path so each value still stages/applies through
// the feature layer. dir<0 = left/prev, dir>=0 = right/next.
#define GTAV_MENU_CYCLER_FAST_STEP 5
static void cycler_fast_step(int dir) {
  for (int i = 0; i < GTAV_MENU_CYCLER_FAST_STEP; ++i) {
    uint32_t action = (dir < 0) ? gtav_native_bridge_left() : gtav_native_bridge_right();
    handle_shell_action_param(action, gtav_native_bridge_last_action_param(), 0);
  }
}

static int vehicle_model_hash_from_index(uint32_t index, uint32_t* model_hash) {
  if (!model_hash) return 0;
  if (index >= gtav_vehicle_catalog_count) return 0;
  *model_hash = gtav_vehicle_catalog[index].model_hash;
  return *model_hash != 0u;
}

// Command-file / debug token names mapped to menu commands. Names are matched
// exactly (the caller trims the trailing newline first), so each token resolves
// to one command and lengths can never drift from the literals. Aliases are
// listed as their own rows.
static const struct {
  char name[16];  // inline (no relocation; see feature_catalog.h)
  uint32_t command;
} kCommandNames[] = {
    {"toggle", GTAV_MENU_COMMAND_TOGGLE},
    {"stop", GTAV_MENU_COMMAND_STOP},
    {"next", GTAV_MENU_COMMAND_NEXT},
    {"down", GTAV_MENU_COMMAND_NEXT},
    {"prev", GTAV_MENU_COMMAND_PREV},
    {"up", GTAV_MENU_COMMAND_PREV},
    {"select", GTAV_MENU_COMMAND_SELECT},
    {"left", GTAV_MENU_COMMAND_LEFT},
    {"right", GTAV_MENU_COMMAND_RIGHT},
    {"page_prev", GTAV_MENU_COMMAND_PAGE_PREV},
    {"page_next", GTAV_MENU_COMMAND_PAGE_NEXT},
    {"home", GTAV_MENU_COMMAND_HOME},
    {"end", GTAV_MENU_COMMAND_END},
    {"letter_prev", GTAV_MENU_COMMAND_LETTER_PREV},
    {"letter_next", GTAV_MENU_COMMAND_LETTER_NEXT},
    {"pin", GTAV_MENU_COMMAND_PIN},
    {"telemetry", GTAV_MENU_COMMAND_TELEMETRY},
    {"god", GTAV_MENU_COMMAND_GOD},
    {"heal", GTAV_MENU_COMMAND_HEAL_ARMOR},
    {"wanted", GTAV_MENU_COMMAND_CLEAR_WANTED},
    {"back", GTAV_MENU_COMMAND_BACK},
    {"back_root", GTAV_MENU_COMMAND_BACK_ROOT},
    {"refresh", GTAV_MENU_COMMAND_SHOW},
    {"redraw", GTAV_MENU_COMMAND_SHOW},
    {"show", GTAV_MENU_COMMAND_SHOW},
    {"hide", GTAV_MENU_COMMAND_HIDE},
};

static uint32_t parse_command_text(const char* command) {
  if (!command) return GTAV_MENU_COMMAND_NONE;
  for (size_t i = 0; i < sizeof(kCommandNames) / sizeof(kCommandNames[0]); ++i) {
    if (!strcmp(command, kCommandNames[i].name)) return kCommandNames[i].command;
  }
  return GTAV_MENU_COMMAND_NONE;
}

// Runtime worker loop period (us). Default 60 Hz (16666 us). Read by main.c's loop;
// tunable live via the SET_WORKER_HZ mailbox command. Note: over-rendering above the
// game's frame rate (e.g. 90 Hz) samples a fresh menu text submission every presented
// frame and eliminates the phase-beat flicker off-thread rendering at/below the frame
// rate can produce (confirmed on-target 2026-06-14); raise this back toward 90 Hz if
// flicker reappears.
static volatile unsigned g_worker_period_us = 16666u;
unsigned gtav_menu_worker_period_us(void) {
  return g_worker_period_us;
}
static void set_worker_hz(uint32_t hz) {
  if (hz < 10u) hz = 10u;
  if (hz > 240u) hz = 240u;
  g_worker_period_us = 1000000u / hz;
}

// Canonical list of every menu command: the switch below routes each
// GTAV_MENU_COMMAND_* (navigation, toggles, parameterized commands) to its
// handler. New commands are added here and named in command_mailbox.c.
static const char* dispatch_menu_command(uint32_t command, uint64_t argument, const char* source) {
#if GTAV_RENDER_BANK_PROBE
  if (command == GTAV_MENU_COMMAND_RENDER_BANK_CONTROL)
    return gtav_render_bank_control(argument) == 0 ? "bank observation ack" : "unknown";
#endif
#if GTAV_RENDER_CYCLE_PROBE
  if (command == GTAV_MENU_COMMAND_RENDER_CYCLE_PROBE)
    return gtav_render_cycle_arm((uint32_t)argument) == 0 ? "cycle observation ack" : "unknown";
#endif
#if GTAV_RENDER_PATH_PROBE
  if (command >= GTAV_MENU_COMMAND_RENDER_PATH_LOW &&
      command <= GTAV_MENU_COMMAND_RENDER_PATH_CONTROL)
    return gtav_render_path_control(command - GTAV_MENU_COMMAND_RENDER_PATH_LOW, argument) == 0
               ? "path observation ack"
               : "unknown";
#endif
#if GTAV_RENDER_PHASE_INTERCEPT
  if (command >= GTAV_MENU_COMMAND_RENDER_PHASE_SLOT &&
      command <= GTAV_MENU_COMMAND_RENDER_PHASE_CONTROL)
    return gtav_render_phase_control(command - GTAV_MENU_COMMAND_RENDER_PHASE_SLOT, argument) == 0
               ? "phase interception ack"
               : "unknown";
#endif
#if GTAV_MENU_RENDER_DIAGNOSTICS
  if (command == GTAV_MENU_COMMAND_RENDER_DIAG_MODE)
    return gtav_render_diag_select((uint32_t)(argument & 255u), (uint32_t)(argument >> 8)) == 0
               ? "diag transition ack; warmup 2s"
               : "unknown";
  if (command == GTAV_MENU_COMMAND_RENDER_DIAG_CAPTURE)
    return gtav_render_diag_capture((uint32_t)argument) == 0 ? "diag capture ack" : "unknown";
#if GTAV_RENDER_DIAG_ISOLATED
  // No input/actions/profile/resource work can enter the isolated producer through the mailbox.
  if (command != GTAV_MENU_COMMAND_STOP && command != GTAV_MENU_COMMAND_SHOW &&
      command != GTAV_MENU_COMMAND_HIDE && command != GTAV_MENU_COMMAND_TOGGLE &&
      command != GTAV_MENU_COMMAND_SET_WORKER_HZ &&
      command != GTAV_MENU_COMMAND_SET_RENDER_INTERVAL)
    return "unknown";
#endif
#endif
  const char* name = gtav_command_mailbox_command_name(command);

  if (!source) source = "unknown";
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s command %s", source, name);

  switch (command) {
    case GTAV_MENU_COMMAND_TOGGLE:
      gtav_menu_toggle();
      return "toggle";
    case GTAV_MENU_COMMAND_SHOW:
      gtav_menu_set_visible(1);
      return "show";
    case GTAV_MENU_COMMAND_HIDE:
      gtav_menu_set_visible(0);
      return "hide";
    case GTAV_MENU_COMMAND_SET_RENDER_INTERVAL:
      gtav_native_bridge_set_render_interval((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s render_interval=%u", source,
                         gtav_native_bridge_render_interval());
      return "render_interval";
    case GTAV_MENU_COMMAND_SET_WORKER_HZ:
      set_worker_hz((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s worker_period_us=%u", source,
                         g_worker_period_us);
      return "worker_hz";
    case GTAV_MENU_COMMAND_SET_TOAST_TICKS:
      gtav_native_bridge_set_toast_ticks((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s toast_ticks=%u", source,
                         gtav_native_bridge_toast_ticks());
      return "toast_ticks";
    case GTAV_MENU_COMMAND_LOAD_MODULE:
      return gtav_menu_load_spawn_module(source);
    case GTAV_MENU_COMMAND_SPAWN_VEHICLE: {
      uint32_t model_hash = 0;
      uint32_t index = (uint32_t)argument;
      if (!vehicle_model_hash_from_index(index, &model_hash)) {
        gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s spawn index=%u invalid", source, index);
        return "spawn";
      }
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s spawn index=%u hash=0x%x (pre)", source,
                         index, model_hash);
      handle_shell_action_param(GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE, model_hash, 1);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s spawn index=%u hash=0x%x (post)", source,
                         index, model_hash);
      return "spawn";
    }
    case GTAV_MENU_COMMAND_ACTIVATE_ACTION:
      // Bracket the call with pre/post events so a host validator can see the last
      // action attempted if a feature crashes the game (only "(pre)" would appear).
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s activate action=%u (pre)", source,
                         (uint32_t)argument);
      // A host command is already explicit: pre-confirm it so destructive rows run on the first
      // request instead of arming the controller's "press again" prompt.
      g_pending_confirm_action = (uint32_t)argument;
      handle_shell_action_param((uint32_t)argument, 0, 1);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s activate action=%u (post)", source,
                         (uint32_t)argument);
      return "activate_action";
    case GTAV_MENU_COMMAND_STOP:
      request_stop(source);
      return "stop";
    case GTAV_MENU_COMMAND_NEXT:
      gtav_native_bridge_next();
      return "next";
    case GTAV_MENU_COMMAND_PREV:
      gtav_native_bridge_prev();
      return "prev";
    case GTAV_MENU_COMMAND_SELECT: {
      uint32_t action = gtav_native_bridge_activate();
      handle_shell_action_param(action, gtav_native_bridge_last_action_param(), 1);
      return "select";
    }
    case GTAV_MENU_COMMAND_LEFT: {
      uint32_t action = gtav_native_bridge_left();
      /* Apply-on-change cyclers toast on the step (the change actuates now); staging cyclers stay
         quiet and rely on the live "< value >" display. */
      handle_shell_action_param(action, gtav_native_bridge_last_action_param(),
                                action_applies_on_change(action));
      return "left";
    }
    case GTAV_MENU_COMMAND_RIGHT: {
      uint32_t action = gtav_native_bridge_right();
      handle_shell_action_param(action, gtav_native_bridge_last_action_param(),
                                action_applies_on_change(action));
      return "right";
    }
    case GTAV_MENU_COMMAND_PAGE_NEXT:
      // On a value cycler, L1/R1 fast-step the value; elsewhere they page the cursor, rolling into
      // the next sibling submenu once the list is already at its bottom edge.
      if (gtav_native_bridge_selected_faststeppable()) {
        cycler_fast_step(1);
      } else {
        gtav_native_bridge_page_or_sibling(1);
      }
      return "page_next";
    case GTAV_MENU_COMMAND_PAGE_PREV:
      if (gtav_native_bridge_selected_faststeppable()) {
        cycler_fast_step(-1);
      } else {
        gtav_native_bridge_page_or_sibling(-1);
      }
      return "page_prev";
    case GTAV_MENU_COMMAND_HOME:
      gtav_native_bridge_home();
      return "home";
    case GTAV_MENU_COMMAND_END:
      gtav_native_bridge_end();
      return "end";
    case GTAV_MENU_COMMAND_LETTER_NEXT:
      gtav_native_bridge_letter_jump(1);
      return "letter_next";
    case GTAV_MENU_COMMAND_LETTER_PREV:
      gtav_native_bridge_letter_jump(-1);
      return "letter_prev";
    case GTAV_MENU_COMMAND_PIN:
      gtav_native_bridge_pin_selected();
      return "pin";
    case GTAV_MENU_COMMAND_BACK:
      handle_shell_action(gtav_native_bridge_back_action());
      return "back";
    case GTAV_MENU_COMMAND_BACK_ROOT:
      handle_shell_action(gtav_native_bridge_back_to_root());
      return "back_root";
    case GTAV_MENU_COMMAND_TELEMETRY:
      handle_shell_action(gtav_native_bridge_toggle_telemetry());
      return "telemetry";
    case GTAV_MENU_COMMAND_GOD:
      handle_shell_action(GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE);
      return "god";
    case GTAV_MENU_COMMAND_HEAL_ARMOR:
      handle_shell_action(GTAV_NATIVE_SHELL_ACTION_HEAL_ARMOR);
      return "heal";
    case GTAV_MENU_COMMAND_CLEAR_WANTED:
      handle_shell_action(GTAV_NATIVE_SHELL_ACTION_CLEAR_WANTED);
      return "wanted";
    case GTAV_MENU_COMMAND_NONE:
    default:
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s command unknown id=%u", source, command);
      return "unknown";
  }
}

static void consume_command_mailbox(void) {
  GtavMenuCommandRequest request;
  const char* result;
  uint32_t status;

  if (!gtav_command_mailbox_consume(&request)) return;

  result = dispatch_menu_command(request.command, request.argument, "mailbox");
  status =
      strcmp(result, "unknown") ? GTAV_MENU_COMMAND_STATUS_ACKED : GTAV_MENU_COMMAND_STATUS_UNKNOWN;
  gtav_command_mailbox_complete(request.sequence, status, result);
}

static void consume_command_file(void) {
  GtavRootdirGuard rootdir;
  FILE* fp;
  char command[64];
  uint32_t parsed;
  int rooted;
  int got_line;

  rooted = gtav_rootdir_enter(&rootdir) == 0;
  fp = fopen("/data/GTAVMenu/command.txt", "r");
  if (!fp) {
    if (rooted) gtav_rootdir_leave(&rootdir);
    return;
  }
  got_line = fgets(command, sizeof(command), fp) != NULL;
  fclose(fp);
  remove("/data/GTAVMenu/command.txt");
  if (rooted) gtav_rootdir_leave(&rootdir);

  if (!got_line) return;
  // fgets keeps the trailing newline (and any stray whitespace); strip it so the
  // token matches the command table exactly.
  command[strcspn(command, "\r\n")] = 0;
  {
    size_t len = strlen(command);
    while (len > 0 && (command[len - 1] == ' ' || command[len - 1] == '\t')) command[--len] = 0;
  }
  parsed = parse_command_text(command);
  if (parsed) {
    dispatch_menu_command(parsed, 0, "command file");
  } else {
    gtav_status_event(GTAV_MENU_EVENT_COMMAND, "command file unknown");
  }
}

static void consume_controller_input(unsigned long long tick) {
  uint32_t command = gtav_native_bridge_poll_input_command((uint64_t)tick, g_visible);

#if GTAV_MENU_ENABLE_PAD_INPUT
  // Direct DualSense fallback (scePad), independent of GTA input natives. Only
  // consulted when the native bridge produced nothing, so an accepted native
  // input path always wins. Init is retried each tick until the controller
  // handle is acquired, then left alone.
  if (command == GTAV_MENU_COMMAND_NONE) {
    if (!gtav_pad_input_available()) {
      gtav_pad_input_init();
    }
    command = gtav_pad_input_poll_command(g_visible);
  }
#endif

#if GTAV_MENU_ENABLE_PAD_INPUT
  // A keybind combo fired while the menu was closed: dispatch its bound action directly
  // (with a confirming toast) rather than through the navigation command path.
  if (command == GTAV_MENU_COMMAND_HOTKEY) {
    uint32_t action = gtav_pad_input_last_hotkey_action();
    if (action != GTAV_NATIVE_SHELL_ACTION_NONE) {
      handle_shell_action_param(action, 0, 1);
    }
    return;
  }
#endif

  if (command != GTAV_MENU_COMMAND_NONE) {
    dispatch_menu_command(command, 0, "input");
  }
}

// Init-progress breadcrumb at INFO: one-time, low-volume, and exactly what helps diagnose a
// failed/hung injection -- so it stays visible by default (init runs before Telemetry can be
// toggled). Reaches the loader log file and, in the worker, the retrievable log ring.
#define GTAV_MENU_INIT_TRACE(...) GTAV_LOGI(__VA_ARGS__)

// Install (or dry-run / preflight) the per-frame hook described by g_init, per the build's
// hook lane. Defined after gtav_menu_init and forward-declared here; see the definition
// below for the decision tree across the orthogonal install dimensions.
static void install_frame_hook(void);

int gtav_menu_init(const GtavMenuInit* init) {
  const GtavNativeAddressTable* native_table;

  GTAV_MENU_INIT_TRACE("mi: enter");
  gtav_status_reset(init);
  GTAV_MENU_INIT_TRACE("mi: status_reset done");

  if (!init || init->abi_version != GTAV_MENU_ABI_VERSION) {
    gtav_log_open(GTAV_MENU_DEFAULT_LOG);
    gtav_logf("invalid init block");
    gtav_status_set_init_result(-1);
    gtav_status_set_error(GTAV_MENU_ERROR_INVALID_INIT, "invalid init block");
    return -1;
  }

  memcpy(&g_init, init, sizeof(g_init));
  gtav_status_set_hook(GTAV_MENU_HOOK_DISABLED, (uintptr_t)g_init.hook_addr, g_init.hook_length,
                       NULL, 0);
  gtav_status_set_log_open_result(
      gtav_log_open(g_init.log_path[0] ? g_init.log_path : GTAV_MENU_DEFAULT_LOG));
  gtav_logf("gtav_menu_init target=%s flags=0x%08x base=0x%llx hook=0x%llx", g_init.target_id,
            g_init.flags, (unsigned long long)g_init.game_base,
            (unsigned long long)g_init.hook_addr);
  gtav_status_eventf(GTAV_MENU_EVENT_INIT, "init target=%s flags=0x%08x", g_init.target_id,
                     g_init.flags);

  g_ticks = 0;
  g_hook_side_ticks = 0;
  g_control_update_step_original = NULL;
  g_draw_rect_native_original = NULL;
  g_handler_slot_addr = 0;
  g_handler_slot_original = 0;
  g_handler_slot_installed = 0;
  g_visible = 0;
  g_stop_requested = 0;
  g_initialized = 1;
  gtav_command_mailbox_reset();
  gtav_patch_broker_reset();
  gtav_quit_guard_reset();  // start the teardown/suspend watchdog clean (matters on re-inject)
  native_table = native_table_has_addresses(&g_init.native_table) ? &g_init.native_table : NULL;
  GTAV_MENU_INIT_TRACE("mi: pre native_bridge_init table=%p", (const void*)native_table);
  gtav_native_bridge_init(native_table);
#if GTAV_RENDER_DIAG_ISOLATED
  gtav_features_init_frame_hook();  // probe + original chaining only; callback/jobs bypassed
  gtav_status_set_initialized(1);
  gtav_status_set_init_result(0);
  gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
  return 0;
#endif
  GTAV_MENU_INIT_TRACE("mi: post native_bridge_init");
  gtav_native_bridge_set_canary(g_init.native_canary_flags, g_init.native_canary_interval,
                                g_init.native_canary_max_frames);
  /* Seed the toast dwell time from config (0 leaves the bridge's compile-time
     default; the setter clamps to a sane range). */
  gtav_native_bridge_set_toast_ticks(g_init.toast_ticks);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  gtav_features_init(native_table);
  GTAV_MENU_INIT_TRACE("mi: post features_init");
  /* Restore + apply the persisted Worker Hz / Toast Time selections (profile v11) now that
      gtav_features_init has loaded the profile; otherwise they silently revert to defaults on
      every re-inject. Overrides the config-seeded toast dwell above (profile wins). */
  apply_view_settings_from_profile();
  /* Seed the menu's list-cycler display values (weather/time) so they show before
      the user touches them. */
  push_list_values();
  GTAV_MENU_INIT_TRACE("mi: post push_list_values");
#else
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native features build disabled");
#endif
  /* Arm the main-thread frame hook independent of gameplay natives so the context probe
     (and any probe/calibration lane) runs on the game thread even when native features are
     compiled off. In feature builds this also installs the job-drain consumer. */
  gtav_features_init_frame_hook();
  GTAV_MENU_INIT_TRACE("mi: post init_frame_hook");
  /* Seed the menu customisation cycler labels (theme/draw side). These are bridge render
     state, not native features, so they are seeded in every build. */
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_THEME,
                                    gtav_native_bridge_theme_label(gtav_native_bridge_theme()));
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_REGION,
                                    gtav_native_bridge_region_label(gtav_native_bridge_region()));
  gtav_native_bridge_set_list_value(
      GTAV_NATIVE_SHELL_ACTION_CYCLE_PANEL_WIDTH,
      gtav_native_bridge_panel_width_label(gtav_native_bridge_panel_width_index()));
  /* Render-cadence + toast-dwell + feel cyclers: seed the display from the live bridge index
     (derived, like Theme/Draw Side above) so a persisted non-default value shows correctly -- a
     plain hardcoded "60 Hz" here would clobber the value apply_view_settings_from_profile just
     restored. In a build without the profile the bridge holds its defaults, so this still reads
     60 Hz / 4.5 s / Full / Normal / Normal. */
  {
    uint32_t whz = gtav_native_bridge_worker_hz_index() % GTAV_MENU_WORKER_HZ_COUNT;
    uint32_t tti = gtav_native_bridge_toast_time_index() % GTAV_MENU_TOAST_COUNT;
    uint32_t nd = gtav_native_bridge_nav_delay_index() % GTAV_MENU_NAV_DELAY_COUNT;
    uint32_t ns = gtav_native_bridge_nav_speed_index() % GTAV_MENU_NAV_SPEED_COUNT;
    char hzlabel[16];
    snprintf(hzlabel, sizeof(hzlabel), "%u Hz", (unsigned)kWorkerHzChoices[whz]);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ, hzlabel);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME, kToastLabels[tti]);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION,
                                      kMotionLabels[gtav_native_bridge_reduce_motion() ? 1 : 0]);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY,
                                      kNavDelayLabels[nd]);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED,
                                      kNavSpeedLabels[ns]);
    gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD,
                                      gtav_native_bridge_touchpad_enabled() ? "On" : "Off");
    apply_nav_repeat();
  }
  gtav_status_set_visible(0);
  gtav_status_set_stop_requested(0);
  gtav_status_set_initialized(1);
  gtav_status_tick(0);
  gtav_status_hook_tick(0);
  gtav_status_set_init_result(0);
  gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
  GTAV_MENU_INIT_TRACE("mi: status set running; pre notify");

  menu_notify("GTAVMenu injected proof loaded");
  gtav_status_event(GTAV_MENU_EVENT_NOTIFY, GTAV_MENU_DISABLE_NOTIFICATIONS
                                                ? "notification disabled"
                                                : "notification sent");
  install_frame_hook();

  GTAV_MENU_INIT_TRACE("mi: returning 0");
  return 0;
}

// The detour's saved-original length, clamped to the status struct's fixed
// expected[]/original[] buffers. Every arm that reports g_frame_detour bytes
// shares this clamp.
static uint32_t frame_detour_original_len(void) {
  return g_frame_detour.length <= GTAV_MENU_MAX_EXPECTED_BYTES ? (uint32_t)g_frame_detour.length
                                                               : 0;
}

// Report a failed hook install and fall back to worker-only ticking: the
// status_set_hook(INSTALL_FAILED) + set_error + log + set_state(RUNNING) that
// every failing arm of install_frame_hook shares.
static void hook_install_failed(uint32_t error_code, const char* error_message,
                                const char* log_line, const uint8_t* original,
                                uint32_t original_len) {
  gtav_status_set_hook(GTAV_MENU_HOOK_INSTALL_FAILED, (uintptr_t)g_init.hook_addr,
                       g_init.hook_length, original, original_len);
  gtav_status_set_error(error_code, error_message);
  gtav_logf("%s", log_line);
  gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
}

// install_frame_hook (forward-declared above). One decision tree across orthogonal
// dimensions: dry_run vs live (GTAV_MENU_FLAG_DRY_RUN); host-prevalidated bytes vs
// in-process validate (GTAV_MENU_HOST_VALIDATED_*); install method (native handler-slot
// swap vs prologue detour/trampoline); and live-hook staging (GATEWAY / PROTECT preflight
// vs full install). Every arm is fail-safe: on any problem it records hook status + error
// and leaves the worker RUNNING (the menu degrades to worker-tick-only, never crashes).
// Operates entirely on g_init / g_frame_detour and the file-static helpers above.
static void install_frame_hook(void) {
  GTAV_MENU_INIT_TRACE("mi: pre hook block allow_hook=%d hook_addr=0x%llx",
                       !!(g_init.flags & GTAV_MENU_FLAG_ALLOW_HOOK),
                       (unsigned long long)g_init.hook_addr);

  if ((g_init.flags & GTAV_MENU_FLAG_ALLOW_HOOK) && g_init.hook_addr) {
    int dry_run = !!(g_init.flags & GTAV_MENU_FLAG_DRY_RUN);
    void* hook_body = selected_hook_body();
    void* trampoline_original = NULL;
    if (!dry_run && GTAV_MENU_HOST_VALIDATED_LIVE_HOOK) {
      gtav_status_set_hook(GTAV_MENU_HOOK_VALIDATING, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, g_init.expected, g_init.expected_len);
      gtav_status_event(GTAV_MENU_EVENT_INIT, "host-validated live hook install starting");
    } else {
      gtav_status_set_hook(GTAV_MENU_HOOK_VALIDATING, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, NULL, 0);
    }
    if (!g_init.expected_len) {
      gtav_status_set_hook(GTAV_MENU_HOOK_VALIDATION_FAILED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, NULL, 0);
      gtav_status_set_error(GTAV_MENU_ERROR_HOOK_MISSING_EXPECTED, "hook expected bytes missing");
      gtav_logf("hook expected bytes missing; continuing worker tick only");
      gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
    } else if (dry_run && GTAV_MENU_HOST_VALIDATED_HOOK_DRY_RUN) {
      gtav_status_set_hook(GTAV_MENU_HOOK_DRY_RUN_PASSED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, g_init.expected, g_init.expected_len);
      gtav_status_event(GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED, "host-validated hook dry-run passed");
    } else if (GTAV_MENU_HOOK_NATIVE_HANDLER_SLOT && !dry_run && !GTAV_MENU_ENABLE_LIVE_HOOK) {
      hook_install_failed(GTAV_MENU_ERROR_LIVE_HOOK_DISABLED, "live handler slot build disabled",
                          "live handler slot build disabled; rebuild only after slot review", NULL,
                          0);
    } else if (GTAV_MENU_HOOK_NATIVE_HANDLER_SLOT &&
               install_native_handler_slot(hook_body, dry_run) != 0) {
      gtav_status_set_hook(GTAV_MENU_HOOK_INSTALL_FAILED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, g_init.expected, g_init.expected_len);
      gtav_logf("native handler slot install failed; continuing worker tick only");
      gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
    } else if (GTAV_MENU_HOOK_NATIVE_HANDLER_SLOT) {
      gtav_logf("native handler slot path selected");
    } else if (!(!dry_run && GTAV_MENU_HOST_VALIDATED_LIVE_HOOK) &&
               gtav_detour_validate((uintptr_t)g_init.hook_addr, g_init.expected,
                                    g_init.expected_len) != 0) {
      gtav_status_set_hook(GTAV_MENU_HOOK_VALIDATION_FAILED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, NULL, 0);
      gtav_status_set_error(GTAV_MENU_ERROR_HOOK_VALIDATION_FAILED, "hook validation failed");
      gtav_logf("hook validation failed; continuing worker tick only");
      gtav_status_set_state(GTAV_MENU_STATE_RUNNING);
    } else if (dry_run && !GTAV_MENU_ENABLE_HOOK_DRY_RUN) {
      hook_install_failed(
          GTAV_MENU_ERROR_LIVE_HOOK_DISABLED, "hook dry-run build disabled",
          "hook dry-run build disabled; use read-only live sweep for default validation", NULL, 0);
    } else if (dry_run && gtav_detour_install_abs_jump(
                              &g_frame_detour, (uintptr_t)g_init.hook_addr, hook_body,
                              g_init.hook_length, g_init.expected, g_init.expected_len, 1) != 0) {
      hook_install_failed(GTAV_MENU_ERROR_HOOK_INSTALL_FAILED, "hook install failed",
                          "hook install failed; continuing worker tick only",
                          g_frame_detour.original, frame_detour_original_len());
    } else if (dry_run) {
      gtav_status_set_hook(GTAV_MENU_HOOK_DRY_RUN_PASSED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, g_frame_detour.original,
                           frame_detour_original_len());
      gtav_status_event(GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED, "hook dry-run passed");
    } else if (!GTAV_MENU_ENABLE_LIVE_HOOK) {
      hook_install_failed(GTAV_MENU_ERROR_LIVE_HOOK_DISABLED, "live hook build disabled",
                          "live hook build disabled; rebuild only after ABI review", NULL, 0);
    } else if (!dry_run && GTAV_MENU_HOST_VALIDATED_LIVE_HOOK &&
               GTAV_MENU_LIVE_HOOK_STAGE == GTAV_MENU_LIVE_HOOK_STAGE_GATEWAY) {
      if (gtav_detour_prepare_trampoline_prevalidated(
              &g_frame_detour, (uintptr_t)g_init.hook_addr, g_init.hook_length, g_init.expected,
              g_init.expected_len, &trampoline_original) != 0) {
        hook_install_failed(GTAV_MENU_ERROR_HOOK_INSTALL_FAILED, "gateway preflight failed",
                            "host-validated gateway preflight failed; continuing worker tick only",
                            g_frame_detour.original, frame_detour_original_len());
      } else {
        gtav_status_set_hook(GTAV_MENU_HOOK_DRY_RUN_PASSED, (uintptr_t)g_init.hook_addr,
                             g_init.hook_length, g_frame_detour.original,
                             frame_detour_original_len());
        gtav_status_event(GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED,
                          "host-validated live gateway preflight passed");
      }
    } else if (!dry_run && GTAV_MENU_HOST_VALIDATED_LIVE_HOOK &&
               GTAV_MENU_LIVE_HOOK_STAGE == GTAV_MENU_LIVE_HOOK_STAGE_PROTECT) {
      int preflight_rc = gtav_detour_prepare_trampoline_prevalidated(
          &g_frame_detour, (uintptr_t)g_init.hook_addr, g_init.hook_length, g_init.expected,
          g_init.expected_len, &trampoline_original);
      if (preflight_rc == 0) {
        preflight_rc =
            gtav_detour_probe_patch_protection((uintptr_t)g_init.hook_addr, g_init.hook_length);
      }
      if (preflight_rc != 0) {
        gtav_detour_restore(&g_frame_detour);
        hook_install_failed(
            GTAV_MENU_ERROR_HOOK_INSTALL_FAILED, "protection preflight failed",
            "host-validated protection preflight failed; continuing worker tick only",
            g_frame_detour.original, frame_detour_original_len());
      } else {
        gtav_status_set_hook(GTAV_MENU_HOOK_DRY_RUN_PASSED, (uintptr_t)g_init.hook_addr,
                             g_init.hook_length, g_frame_detour.original,
                             frame_detour_original_len());
        gtav_status_event(GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED,
                          "host-validated live protection preflight passed");
      }
    } else if ((!GTAV_MENU_HOST_VALIDATED_LIVE_HOOK &&
                gtav_detour_install_trampoline(&g_frame_detour, (uintptr_t)g_init.hook_addr,
                                               hook_body, g_init.hook_length, g_init.expected,
                                               g_init.expected_len, &trampoline_original) != 0) ||
               (GTAV_MENU_HOST_VALIDATED_LIVE_HOOK &&
                gtav_detour_install_trampoline_prevalidated_publish(
                    &g_frame_detour, (uintptr_t)g_init.hook_addr, hook_body, g_init.hook_length,
                    g_init.expected, g_init.expected_len, publish_trampoline_original, NULL,
                    &trampoline_original) != 0)) {
      hook_install_failed(GTAV_MENU_ERROR_HOOK_INSTALL_FAILED, "trampoline install failed",
                          "trampoline hook install failed; continuing worker tick only",
                          g_frame_detour.original, frame_detour_original_len());
    } else {
      if (!GTAV_MENU_HOST_VALIDATED_LIVE_HOOK) {
        set_trampoline_original(trampoline_original);
      }
      gtav_status_set_hook(GTAV_MENU_HOOK_INSTALLED, (uintptr_t)g_init.hook_addr,
                           g_init.hook_length, g_frame_detour.original,
                           frame_detour_original_len());
      gtav_status_event(GTAV_MENU_EVENT_HOOK_INSTALLED, "hook installed");
    }
  } else {
    gtav_logf("hook install disabled; using worker tick proof");
    gtav_status_set_hook(GTAV_MENU_HOOK_DISABLED, (uintptr_t)g_init.hook_addr, g_init.hook_length,
                         NULL, 0);
    gtav_status_event(GTAV_MENU_EVENT_HOOK_DISABLED, "hook disabled");
  }
}

void gtav_menu_worker_tick(void) {
  if (!g_initialized) return;

  unsigned long long tick = ++g_ticks;
  gtav_status_tick(tick);
  consume_command_mailbox();
#if GTAV_RENDER_DIAG_ISOLATED
  gtav_quit_guard_tick();
  gtav_native_bridge_worker_tick(tick, g_visible && !g_stop_requested);
  return;
#endif
  if ((tick % 30u) == 0) {
    consume_command_file();
  }
  // Heartbeat the loader's double-inject lock as liveness evidence for diagnostics/reconciliation.
  // Age is deliberately NOT sufficient to reclaim a lock for this same (or unknown) live game
  // instance: a crashed/partial mapping cannot yet be safely unmapped, so re-inject requires a
  // relaunch or a definite instance-token mismatch. Cadence is wall-clock-bounded (~3s) rather than
  // a fixed tick count so the evidence remains meaningful as worker_hz changes. No-op when the
  // guard left no lock.
  {
    unsigned period_us = gtav_menu_worker_period_us();
    unsigned refresh_ticks = period_us ? (3000000u / period_us) : 180u;
    if (refresh_ticks < 1u) refresh_ticks = 1u;
    if (refresh_ticks > 600u) refresh_ticks = 600u;
    if ((tick % refresh_ticks) == 0u) {
      gtav_inject_lock_refresh((int)getpid());
    }
  }
  // Teardown/suspend guard: if the game thread stops firing the frame hook (force-close in
  // progress, deep hang) or the app reports backgrounded/suspended, defensively un-hook the
  // scePad input path BEFORE the process is torn down -- the case gtav_menu_shutdown() never sees.
  // Runs before the controller poll so a parked tick issues no scePad read. No-op unless the
  // production quit guard is compiled in. See quit_guard.c / teardown_watchdog.h.
  gtav_quit_guard_tick();
  consume_controller_input(tick);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Apply a menu-visibility request from the game-thread interactive object-move driver (it
  // cannot call gtav_menu_set_visible itself -- that does worker-thread-only logging/status).
  // 1 = hide (entering move mode), 2 = show (committed/cancelled/aborted).
  {
    int vis_req = gtav_features_take_menu_visibility_request();
    if (vis_req == 1) {
      gtav_menu_set_visible(0);
    } else if (vis_req == 2) {
      gtav_menu_set_visible(1);
    }
  }
#endif
  gtav_native_bridge_worker_tick(tick, g_visible);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Skip per-tick feature work once the quit-guard has parked the renderer for a teardown/suspend:
  // gtav_native_bridge_worker_tick (above) already stood its natives down, and we want the worker
  // wholly GTA-inert so the app can suspend cleanly. No-op gate unless the quit-guard is built in.
  if (!gtav_native_bridge_is_parked()) {
    gtav_features_worker_tick(g_visible);
  }
#endif

  if (GTAV_MENU_LOG_TICK_INTERVAL && (tick % GTAV_MENU_LOG_TICK_INTERVAL) == 0) {
    // Recurring -- DEBUG so it only fills the worker log ring when Telemetry is on.
    GTAV_LOGD("worker tick=%llu", tick);
    gtav_status_eventf(GTAV_MENU_EVENT_TICK, "worker tick=%llu", tick);
  }

#if GTAV_MENU_ENABLE_FRAME_HOOK
  // One-shot persistent breadcrumb: mirror the game-thread TLS probe once it has fired. The
  // status event ring scrolls every ~1s under worker-tick events, so the calibrate event alone
  // is not a reliable capture channel; the worker log ring (64 slots, quiet) carries it instead.
  if (!g_frame_probe_logged) {
    GtavFrameHookProbeSnapshot probe;
    gtav_frame_hook_probe_snapshot(&probe);
    if (probe.calls > 0) {
      g_frame_probe_logged = 1;
      // Split into short lines: the worker log ring caps a message at
      // GTAV_MENU_LOG_RING_MSG_LEN (128) and truncates silently, which previously cut a
      // pointer field mid-hex and made a valid TLS base read as a small bogus integer
      // (see tick_frame_hook_telemetry in features.cpp).
      GTAV_LOGI("frame probe first fire calls=%llu ctx_hits=%llu", (unsigned long long)probe.calls,
                (unsigned long long)probe.calls_with_context);
      GTAV_LOGI("frame probe fsbase=0x%llx realfs=0x%llx", (unsigned long long)probe.last_fsbase,
                (unsigned long long)probe.last_real_fsbase);
      GTAV_LOGI("frame probe ctx=0x%llx tls_ctx_off=0x%llx",
                (unsigned long long)probe.last_script_context,
                (unsigned long long)probe.tls_ctx_off);
      GTAV_LOGI("frame probe nt=0x%llx ntv=0x%llx", (unsigned long long)probe.last_native_thread,
                (unsigned long long)probe.last_native_thread_vtable);
    }
  }
#endif

  if (g_visible && (tick % 120u) == 0) {
    GTAV_LOGD("menu visible tick=%llu", tick);
  }
}

void gtav_menu_tick(void) {
  gtav_menu_worker_tick();
}

void gtav_menu_frame_tick(void) {
  if (!g_initialized) return;

  unsigned int frame_tick = ++g_hook_side_ticks;
  gtav_status_hook_tick(frame_tick);
  gtav_native_bridge_frame_tick(frame_tick, g_visible);
}

int gtav_menu_render_phase_tick(unsigned long long epoch) {
#if GTAV_RENDER_PHASE_INTERCEPT
  if (!g_initialized || g_stop_requested) return 0;
  return gtav_native_bridge_phase_tick((uint64_t)epoch, g_visible);
#else
  (void)epoch;
  return 0;
#endif
}

void gtav_menu_set_visible(int visible) {
#if GTAV_RENDER_DIAG_ISOLATED
  g_visible = visible ? 1 : 0;
  gtav_status_set_visible(g_visible);
  return;
#endif
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  const int was_visible = g_visible; /* for the closed->open list-value refresh below */
#endif
  g_visible = visible ? 1 : 0;
  gtav_native_bridge_set_visible(g_visible);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Flip the game-thread input-suppression predicate on the SAME transition as visibility.
  // The features worker tick also mirrors it, but that runs after input is consumed, so the
  // open/close frame would otherwise leak D-pad/Cross/radio into gameplay for one frame.
  gtav_features_set_menu_open(g_visible);
  // Re-pull live cycler values on the closed->open edge so reopening the menu while parked in an
  // LSC submenu shows the vehicle's current mod tiers (not the values cached when it last closed).
  if (g_visible && !was_visible) push_list_values();
#endif
  gtav_status_set_visible(g_visible);
  gtav_status_eventf(GTAV_MENU_EVENT_TOGGLE, "visible=%d", g_visible);
  gtav_logf("menu visibility changed: %s", g_visible ? "visible" : "hidden");
  menu_notify(g_visible ? "GTAVMenu shell visible" : "GTAVMenu shell hidden");
}

void gtav_menu_toggle(void) {
  gtav_menu_set_visible(!g_visible);
}

// Remove the loader's per-pid double-inject lock only when this worker can fully restore its own
// hook state. EXTERNAL_INSTALL cannot remove the loader-written eboot jump (shutdown leaves its
// passthrough gateway mapped), so advertising a same-process re-inject would stack a second image
// and then fail broker prologue validation. Retain that lock until game relaunch/token change.
static void remove_inject_lock(void) {
#if GTAV_FRAME_HOOK_EXTERNAL_INSTALL
  gtav_logf("retaining inject lock: external frame-hook image requires game relaunch");
#else
  char path[GTAV_INJECT_LOCK_PATH_MAX];
  gtav_inject_lock_path(path, sizeof(path), (int)getpid());
  if (unlink(path) == 0) {
    gtav_logf("removed inject lock %s", path);
  }
#endif
}

void gtav_menu_shutdown(void) {
  int hook_was_installed = g_frame_detour.installed;
  int handler_slot_was_installed = g_handler_slot_installed;

  gtav_status_set_state(GTAV_MENU_STATE_STOPPING);
  gtav_status_eventf(GTAV_MENU_EVENT_SHUTDOWN, "shutdown ticks=%llu", (unsigned long long)g_ticks);
  gtav_logf("gtav_menu_shutdown ticks=%llu", (unsigned long long)g_ticks);
  // Teardown breadcrumb / "last words": a single line carrying the state that matters for
  // diagnosing a teardown crash (frame-hook liveness + queue health + whether the input path was
  // parked). It rides the production worker->kernel-log sink, so it
  // survives the GTA process dying -- read it back with `./menu-ctl.sh logs --kernel`.
  unsigned fh_calls = 0u, fh_jobs = 0u, fh_dropped = 0u;
  int fh_active = 0;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  fh_calls = gtav_frame_hook_call_count();
  fh_jobs = gtav_frame_hook_jobs_run();
  fh_dropped = gtav_frame_hook_jobs_dropped();
  fh_active = gtav_frame_hook_is_active();
#endif
  gtav_logf(
      "shutdown last-words: ticks=%llu hook_calls=%u jobs_run=%u jobs_dropped=%u "
      "frame_active=%d pad_parked=%d frame_detour=%d handler_slot=%d",
      (unsigned long long)g_ticks, fh_calls, fh_jobs, fh_dropped, fh_active,
      gtav_pad_input_is_parked(), hook_was_installed, handler_slot_was_installed);
  // Stand the renderer down before we touch hooks: the frame hook stays active until it is
  // restored below, so a late game-thread fire would otherwise still run the draw canary. Parking
  // here also matches the force-close path (quit_guard), keeping teardown render-inert either way.
  gtav_native_bridge_park();
  gtav_menu_set_visible(0);
#if GTAV_RENDER_PHASE_INTERCEPT
  if (gtav_render_phase_restore() != 0)
    gtav_logf("shutdown: phase callback restoration failed; process must be retired");
#endif
#if GTAV_MENU_ENABLE_NATIVE_FEATURES && !GTAV_RENDER_DIAG_ISOLATED
  gtav_features_shutdown();
#endif
  gtav_pad_input_wait_menu_buttons_released(GTAV_MENU_SHUTDOWN_INPUT_RELEASE_USEC);

  // Restore the scePadReadState GOT swap before the worker exits so GTA never
  // calls into payload code after Stop Runtime completes. The release grace
  // above keeps the final Stop button press suppressed before this restore.
  gtav_logf("shutdown: restoring scePad GOT swap");
  gtav_pad_input_shutdown();

  if (restore_native_handler_slot() == 0 && handler_slot_was_installed) {
    hook_was_installed = 0;
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  gtav_logf("shutdown: restoring frame hook (external-install .text jump stays a passthrough)");
  gtav_frame_hook_restore();
#endif
  if (gtav_detour_restore(&g_frame_detour) == 0 && hook_was_installed) {
    gtav_status_set_hook(GTAV_MENU_HOOK_RESTORED, g_frame_detour.address,
                         (uint32_t)g_frame_detour.length, g_frame_detour.original,
                         frame_detour_original_len());
  }
  g_control_update_step_original = NULL;
  g_draw_rect_native_original = NULL;
  g_handler_slot_addr = 0;
  g_handler_slot_original = 0;
  gtav_native_bridge_set_draw_rect_call_override(0);
  g_initialized = 0;
  gtav_status_set_initialized(0);
  gtav_status_set_state(GTAV_MENU_STATE_STOPPED);
  remove_inject_lock();
  gtav_logf("gtav_menu_shutdown complete (state=STOPPED)");
  gtav_log_close();
}

int gtav_menu_cam_exists_i32(int cam) {
  gtav_menu_frame_tick();
  return cam;
}

void gtav_menu_control_update_step_hook(void) {
#if !GTAV_MENU_HOOK_SKIP_ORIGINAL
  GtavControlUpdateStepFn original = g_control_update_step_original;
  if (original) {
    original();
  }
#endif
  gtav_menu_frame_tick();
}

void gtav_menu_draw_rect_native_hook(void* native_arg) {
#if GTAV_MENU_HOOK_SKIP_ORIGINAL
  (void)native_arg;
#else
  GtavDrawRectNativeFn original = g_draw_rect_native_original;
  if (original) {
    original(native_arg);
  }
#endif
  gtav_menu_frame_tick();
}

int gtav_menu_is_visible(void) {
  return g_visible;
}

unsigned long long gtav_menu_tick_count(void) {
  return g_ticks;
}

int gtav_menu_stop_requested(void) {
  return g_stop_requested;
}
