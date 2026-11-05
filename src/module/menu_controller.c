// The target-neutral half of the menu: command dispatch, the shell-action handler (settings
// cyclers, one-press confirm, toasts), visibility and the per-tick post-input step. Every platform
// shell drives it the same way -- the PS5 worker (menu.c) from its scePad/mailbox/command-file
// loop, the PS4 one-shot worker from the hooked game thread -- and supplies only the few effects
// that differ through GtavMenuPlatformOps.
#define GTAV_LOG_DEFAULT_CATEGORY GTAV_LOG_CAT_MENU

#include "gtavmenu/menu_controller.h"

#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/feature_catalog.h"
#include "gtavmenu/features.h"
#include "gtavmenu/localization.h"
#include "gtavmenu/menu_timing.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/status.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef GTAV_MENU_ENABLE_NATIVE_FEATURES
#define GTAV_MENU_ENABLE_NATIVE_FEATURES 0
#endif

static GtavMenuPlatformOps g_ops;
// Read by the platform's frame/render paths as well as its worker; written only from the
// thread that drives the controller.
static volatile int g_visible;

static void request_visible(int visible) {
  if (g_ops.set_visible) {
    g_ops.set_visible(visible);
  } else {
    gtav_menu_controller_apply_visible(visible);
  }
}

static void request_stop(const char* reason) {
  if (g_ops.request_stop) g_ops.request_stop(reason);
}

#if GTAV_MENU_ENABLE_NATIVE_FEATURES
static void push_list_values(void) {
  gtav_native_bridge_sync_feature_state();
}
#endif

#if GTAV_MENU_ENABLE_NATIVE_FEATURES
static void push_action_toast(void) {
  gtav_native_bridge_push_feature_toast();
}
#endif

/* The Worker Hz cycler actuates the platform's worker loop (PS5); a platform without a
   free-running worker (the PS4 one-shot hook) leaves set_worker_hz NULL. */
static void set_worker_hz(uint32_t hz) {
  if (g_ops.set_worker_hz) g_ops.set_worker_hz(hz);
}

/* Worker Hz + Toast Time cycler choices, hoisted to file scope so the cyclers (below) and the
   init-time restore (apply_view_settings_from_profile) share one source of truth. The selected
   index for each lives in the native bridge, so it round-trips through the feature profile (v11)
   instead of resetting to default on every re-inject. */
static const uint32_t kWorkerHzChoices[] = {60u, 72u, 90u, 120u};
static const char* const kWorkerHzLabels[] = {"60 Hz", "72 Hz", "90 Hz", "120 Hz"};
static const uint32_t kToastTicks[] = {GTAV_MENU_TICKS_60(120u), GTAV_MENU_TICKS_60(180u),
                                       GTAV_MENU_TICKS_60(270u), GTAV_MENU_TICKS_60(360u)};
static const char* const kToastLabels[] = {"2.0 s", "3.0 s", "4.5 s", "6.0 s"};
#define GTAV_MENU_WORKER_HZ_COUNT \
  ((uint32_t)(sizeof(kWorkerHzChoices) / sizeof(kWorkerHzChoices[0])))
#define GTAV_MENU_TOAST_COUNT ((uint32_t)(sizeof(kToastTicks) / sizeof(kToastTicks[0])))
_Static_assert(sizeof(kWorkerHzLabels) / sizeof(kWorkerHzLabels[0]) == GTAV_MENU_WORKER_HZ_COUNT,
               "one Worker Hz label per choice");
_Static_assert(sizeof(kToastLabels) / sizeof(kToastLabels[0]) == GTAV_MENU_TOAST_COUNT,
               "one Toast Time label per choice");

/* v13 feel/accessibility cyclers. Motion is a 2-way (Full/Reduced) flag held in the bridge (the
   draw path reads it). Nav Delay / Nav Speed tune the live D-pad auto-repeat cadence (poll ticks;
   smaller rate = faster); index 1 == the historical defaults (delay 14, rate 4), so a memset-0
   pre-v13 profile version-gates to those. Like Worker Hz, the selected index lives in the bridge so
   it round-trips through the profile; menu.c owns the tables and applies them to the pad mapper via
   gtav_pad_input_set_repeat. */
static const char* const kMotionLabels[] = {"Full", "Reduced"};
static const uint32_t kNavDelayChoices[] = {GTAV_MENU_TICKS_60(8u), GTAV_MENU_TICKS_60(14u),
                                            GTAV_MENU_TICKS_60(22u)};
static const char* const kNavDelayLabels[] = {"Short", "Normal", "Long"};
static const uint32_t kNavSpeedChoices[] = {GTAV_MENU_TICKS_60(6u), GTAV_MENU_TICKS_60(4u),
                                            GTAV_MENU_TICKS_60(2u)};
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
  gtav_native_bridge_set_worker_hz_index(whz);
  gtav_native_bridge_set_toast_time_index(ti);
  set_worker_hz(kWorkerHzChoices[whz]);
  gtav_native_bridge_set_toast_ticks(kToastTicks[ti]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ, kWorkerHzLabels[whz]);
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

/* "<name>: <value>" toast for a setting step that asks for one (emit_toast): every Left/Right step
   of a SETTING_TOAST row such as Toast Time, and an explicit action+param mailbox command. Quiet
   SETTING steps (emit_toast=0) rely on the live "< value >" display. Works in every build:
   push_toast is a no-op when toasts are built out. */
static void push_setting_toast(int emit_toast, const char* name, const char* value) {
  if (!emit_toast) return;
  char msg[80];
  snprintf(msg, sizeof(msg), "%s: %s", gtav_locale_translate(name), gtav_locale_translate(value));
  gtav_native_bridge_push_toast(msg, GTAV_FEATURE_RESULT_OK);
}

/* Menu-settings cyclers (Settings > Menu / Runtime): pure bridge render-state, no game native, so
   they work in every build. All are live SETTING rows (list_kinds.def): param 2 = prev (Left),
   anything else = next (Right); Cross (param 0) never reaches them (setting_row_ignores_apply).
   A step sets the bridge index, refreshes the live "< value >", actuates the value where the bridge
   alone is not enough (worker period, toast dwell, pad repeat cadence), toasts "<name>: <value>"
   for the rows that name one, and autosaves so chrome settings stick across re-injects without a
   manual Runtime > Save Profile. Language is not here: it steps by locale id (handled inline). */
typedef struct {
  uint32_t action;
  uint32_t count;             /* fixed choice count; 0 = count_fn() */
  uint32_t (*count_fn)(void); /* bridge-owned choice count */
  uint32_t (*get)(void);
  void (*set)(uint32_t index);
  const char* const* labels;               /* fixed labels; NULL = label_fn(index) */
  const char* (*label_fn)(uint32_t index); /* bridge-owned label */
  void (*apply)(void);                     /* actuation after the display refresh, or NULL */
  const char* toast;                       /* toast name, or NULL = no toast */
} MenuSetting;

static const char* const kSpeedometerLayoutLabels[] = {"Classic", "Compact"};
static const char* const kOffOnLabels[] = {"Off", "On"};

static uint32_t reduce_motion_get(void) {
  return gtav_native_bridge_reduce_motion() ? 1u : 0u;
}
static void reduce_motion_set(uint32_t on) {
  gtav_native_bridge_set_reduce_motion((int)on);
}
static uint32_t touchpad_get(void) {
  return gtav_native_bridge_touchpad_enabled() ? 1u : 0u;
}
static void touchpad_set(uint32_t on) {
  gtav_native_bridge_set_touchpad_enabled((int)on);
}
static void apply_worker_hz(void) {
  set_worker_hz(kWorkerHzChoices[gtav_native_bridge_worker_hz_index() % GTAV_MENU_WORKER_HZ_COUNT]);
}
static void apply_toast_time(void) {
  gtav_native_bridge_set_toast_ticks(
      kToastTicks[gtav_native_bridge_toast_time_index() % GTAV_MENU_TOAST_COUNT]);
}

static const MenuSetting kMenuSettings[] = {
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_THEME, 0u, gtav_native_bridge_theme_count,
     gtav_native_bridge_theme, gtav_native_bridge_set_theme, NULL, gtav_native_bridge_theme_label,
     NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_REGION, 0u, gtav_native_bridge_region_count,
     gtav_native_bridge_region, gtav_native_bridge_set_region, NULL,
     gtav_native_bridge_region_label, NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_PANEL_WIDTH, 0u, gtav_native_bridge_panel_width_count,
     gtav_native_bridge_panel_width_index, gtav_native_bridge_set_panel_width_index, NULL,
     gtav_native_bridge_panel_width_label, NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_SPEEDOMETER_LAYOUT, 2u, NULL,
     gtav_native_bridge_speedometer_layout, gtav_native_bridge_set_speedometer_layout,
     kSpeedometerLayoutLabels, NULL, NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION, 2u, NULL, reduce_motion_get, reduce_motion_set,
     kMotionLabels, NULL, NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD, 2u, NULL, touchpad_get, touchpad_set, kOffOnLabels,
     NULL, NULL, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY, GTAV_MENU_NAV_DELAY_COUNT, NULL,
     gtav_native_bridge_nav_delay_index, gtav_native_bridge_set_nav_delay_index, kNavDelayLabels,
     NULL, apply_nav_repeat, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED, GTAV_MENU_NAV_SPEED_COUNT, NULL,
     gtav_native_bridge_nav_speed_index, gtav_native_bridge_set_nav_speed_index, kNavSpeedLabels,
     NULL, apply_nav_repeat, NULL},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ, GTAV_MENU_WORKER_HZ_COUNT, NULL,
     gtav_native_bridge_worker_hz_index, gtav_native_bridge_set_worker_hz_index, kWorkerHzLabels,
     NULL, apply_worker_hz, "Worker Hz"},
    {GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME, GTAV_MENU_TOAST_COUNT, NULL,
     gtav_native_bridge_toast_time_index, gtav_native_bridge_set_toast_time_index, kToastLabels,
     NULL, apply_toast_time, "Toast Time"},
};

static const MenuSetting* find_menu_setting(uint32_t action) {
  for (size_t i = 0; i < sizeof(kMenuSettings) / sizeof(kMenuSettings[0]); ++i) {
    if (kMenuSettings[i].action == action) return &kMenuSettings[i];
  }
  return NULL;
}

static void step_menu_setting(const MenuSetting* s, uint32_t param, int emit_toast) {
  const uint32_t count = s->count ? s->count : s->count_fn();
  const uint32_t cur = s->get() % count;
  const uint32_t next = (param == 2u) ? (cur + count - 1u) % count : (cur + 1u) % count;
  const char* label;
  s->set(next);
  label = s->labels ? s->labels[next] : s->label_fn(next);
  gtav_native_bridge_set_list_value(s->action, label);
  if (s->apply) s->apply();
  if (s->toast) push_setting_toast(emit_toast, s->toast, label);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  gtav_features_profile_save_default();
#endif
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
    /* Custom Packs "Uninstall < id >": Cross applies the picked pack (its files stay). */
    case GTAV_NATIVE_SHELL_ACTION_UNINSTALL_PACK:
    /* Custom Packs "Revert overrides": puts every overridden stock asset back for this process. */
    case GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES:
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

/* Whether a Left/Right step should toast like Cross does: only live SETTING_TOAST rows, whose step
   actuates something off the menu (Wind, Camera Shake, autopilot tuning, ...). Pick-then-apply
   CHOICE rows only stage on the step and toast on Cross; quiet SETTING rows (menu chrome, browser
   filters, spawn parameters) show their effect on screen. Single-sourced from list_kinds.def. */
static int list_step_toasts(uint32_t action) {
  return gtav_native_bridge_list_kind(action) == GTAV_LIST_KIND_SETTING_TOAST;
}

/* D-pad Left/Right: a list step toasts per list_step_toasts; a toggle row's Off/On (Left = OFF,
   Right = ON) flips the feature like Cross does, so it confirms with the same toast. */
static int left_right_toasts(uint32_t action) {
  if (action != GTAV_NATIVE_SHELL_ACTION_NONE && gtav_native_bridge_selected_is_toggle()) return 1;
  return list_step_toasts(action);
}

/* A live SETTING row already applied its value on Left/Right, so Cross (param 0) does nothing --
   the bridge never dispatches it from the pad; this also covers the mailbox lane. A SETTING_APPLY
   row's Cross applies the shown value (Wardrobe Style/Texture), so it passes. Unlisted actions
   read as CHOICE, so only real list rows are affected. */
static int setting_row_ignores_apply(uint32_t action, uint32_t param) {
  if (param != 0u || action == GTAV_NATIVE_SHELL_ACTION_NONE) return 0;
  const uint32_t kind = gtav_native_bridge_list_kind(action);
  return kind == GTAV_LIST_KIND_SETTING || kind == GTAV_LIST_KIND_SETTING_TOAST;
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
      /* Name the row the user is on ("Uninstall: gtavmenu-col-v1"), not the internal action name
         (a hardware run showed "uninstall__pack"); the action name stays the fallback. */
      GtavNativeShellSnapshot snap;
      memset(&snap, 0, sizeof(snap));
      gtav_native_bridge_snapshot(&snap);
      const char* what = snap.selected_label[0]
                             ? snap.selected_label
                             : gtav_locale_translate(gtav_native_bridge_action_name(action));
      char msg[96];
      snprintf(msg, sizeof(msg), gtav_locale_text("toast.confirm"), what);
      gtav_native_bridge_push_toast(msg, GTAV_FEATURE_RESULT_UNAVAILABLE);
      return;
    }
    g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE; /* confirmed -> fall through + run */
  } else if (emit_toast && param == 0u) {
    g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE; /* any other activation disarms */
  } else if (param != 0u && action == g_pending_confirm_action) {
    /* A Left/Right step on an armed list row (Uninstall < id >) picks another value: the next
       Cross must confirm that one afresh. */
    g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE;
  }
  if (setting_row_ignores_apply(action, param)) return;
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_HIDE:
      request_visible(0);
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
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_LANGUAGE: {
      /* Not a kMenuSettings row: the locale list steps by id and a change relabels every row. */
      uint32_t count = gtav_locale_count();
      if (count <= 1u) return;
      uint32_t cur = gtav_locale_id();
      uint32_t next = gtav_locale_step_id(cur, param == 2u ? -1 : 1);
      gtav_native_bridge_set_language(next);
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
      push_list_values();
      gtav_features_profile_save_default();
#else
      gtav_native_bridge_set_list_value(action, gtav_locale_display_name(gtav_locale_id()));
#endif
      return;
    }
    default: {
      const MenuSetting* setting = find_menu_setting(action);
      if (setting) {
        step_menu_setting(setting, param, emit_toast);
        return;
      }
      break;
    }
  }

    /* Everything else is a native gameplay feature (god mode, vehicle spawn,
       weapons, movement). Gated behind the feature build so default builds make
       no native calls. */
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  /* Game-thread-sensitive actions (vehicle spawn, weapons, skin) execute through
     gtav_features_activate_param; their game-thread work is queued as a job and drained
     by the frame hook in valid script context. gtav_features_action_is_gated() is the
     UI lock predicate that keeps such a row selectable only once the hook is live. */
  const uint32_t result = gtav_features_activate_param(action, param);
  if (g_ops.action_result) g_ops.action_result(action, param, result);
  gtav_native_bridge_sync_feature_state();
  if (action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_PRESERVE_SPEED ||
      action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_REPLACE_PREVIOUS ||
      action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_AIRCRAFT_IN_FLIGHT ||
      action == GTAV_NATIVE_SHELL_ACTION_CYCLE_POPULATION_DENSITY) {
    gtav_features_profile_save_default();
  }
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

/* A pick-then-apply row whose staged value was not applied reverts once the cursor leaves it
   (another row or menu, Back, menu closed): replay the opposite single steps through the feature
   layer, which walks its staged index back to the applied value, then refresh the shown values.
   Staging a CHOICE row has no side effect, so the replay is pure index math. Runs once per worker
   tick after all input (pad, mailbox, command file, object-move hide). */
static void revert_unapplied_pick(void) {
  uint32_t action = GTAV_NATIVE_SHELL_ACTION_NONE;
  uint32_t param = 0u;
  uint32_t steps = 0u;
  if (!gtav_native_bridge_take_staged_revert(&action, &param, &steps)) return;
  if (action == g_pending_confirm_action) g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE;
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  for (uint32_t i = 0; i < steps; ++i) gtav_features_activate_param(action, param);
  push_list_values();
#else
  (void)param;
  (void)steps;
#endif
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "shell pick reverted action=%s steps=%u",
                     gtav_native_bridge_action_name(action), steps);
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

// Canonical list of every menu command: the switch below routes each
// GTAV_MENU_COMMAND_* (navigation, toggles, parameterized commands) to its
// handler. New commands are added here and named in command_mailbox.c. Platform-only commands
// (render probes, the spawn-module loader) are offered to the platform first.
static const char* dispatch_menu_command(uint32_t command, uint64_t argument, const char* source) {
  if (g_ops.dispatch_extra) {
    const char* handled = g_ops.dispatch_extra(command, argument, source);
    if (handled) return handled;
  }
  const char* name = gtav_command_mailbox_command_name(command);

  if (!source) source = "unknown";
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s command %s", source, name);

  switch (command) {
    case GTAV_MENU_COMMAND_TOGGLE:
      request_visible(!g_visible);
      return "toggle";
    case GTAV_MENU_COMMAND_SHOW:
      request_visible(1);
      return "show";
    case GTAV_MENU_COMMAND_HIDE:
      request_visible(0);
      return "hide";
    case GTAV_MENU_COMMAND_SET_RENDER_INTERVAL:
      gtav_native_bridge_set_render_interval((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s render_interval=%u", source,
                         gtav_native_bridge_render_interval());
      return "render_interval";
    case GTAV_MENU_COMMAND_SET_WORKER_HZ:
      set_worker_hz((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s worker_period_us=%u", source,
                         g_ops.worker_period_us ? g_ops.worker_period_us() : 0u);
      return "worker_hz";
    case GTAV_MENU_COMMAND_SET_TOAST_TICKS:
      gtav_native_bridge_set_toast_ticks((uint32_t)argument);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s toast_ticks=%u", source,
                         gtav_native_bridge_toast_ticks());
      return "toast_ticks";
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
    case GTAV_MENU_COMMAND_ACTIVATE_ACTION_PARAM: {
      const uint32_t action = (uint32_t)argument;
      const uint32_t param = (uint32_t)(argument >> 32);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s activate action=%u param=0x%x (pre)", source,
                         action, param);
      g_pending_confirm_action = action;
      handle_shell_action_param(action, param, 1);
      gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s activate action=%u param=0x%x (post)", source,
                         action, param);
      return "activate_action_param";
    }
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
      /* Live SETTING_TOAST rows toast on the step (the change actuates now), and so does a toggle
         row's Off/On (it flips the feature like Cross); the rest stay quiet and rely on the live
         "< value >" display. */
      handle_shell_action_param(action, gtav_native_bridge_last_action_param(),
                                left_right_toasts(action));
      return "left";
    }
    case GTAV_MENU_COMMAND_RIGHT: {
      uint32_t action = gtav_native_bridge_right();
      handle_shell_action_param(action, gtav_native_bridge_last_action_param(),
                                left_right_toasts(action));
      return "right";
    }
    case GTAV_MENU_COMMAND_PAGE_NEXT:
      // Page the cursor (touchpad flick, mailbox), rolling into the next sibling submenu once the
      // list is already at its bottom edge.
      gtav_native_bridge_page_or_sibling(1);
      return "page_next";
    case GTAV_MENU_COMMAND_PAGE_PREV:
      gtav_native_bridge_page_or_sibling(-1);
      return "page_prev";
    case GTAV_MENU_COMMAND_HOME:
      // L1 / R1 (and the mailbox home / end): the first / last selectable row of the page.
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

void gtav_menu_controller_init(const GtavMenuPlatformOps* ops) {
  memset(&g_ops, 0, sizeof(g_ops));
  if (ops) g_ops = *ops;
  g_visible = 0;
  g_pending_confirm_action = GTAV_NATIVE_SHELL_ACTION_NONE;
}

void gtav_menu_controller_apply_profile_settings(void) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  /* Restore + apply the persisted Worker Hz / Toast Time selections (profile v11) now that the
     profile is loaded; otherwise they silently revert to defaults on every re-inject. Overrides
     the config-seeded toast dwell (profile wins). */
  apply_view_settings_from_profile();
  /* Seed the menu's list-cycler display values (weather/time) so they show before the user
     touches them. */
  push_list_values();
#endif
}

void gtav_menu_controller_seed_labels(void) {
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
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY, kNavDelayLabels[nd]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED, kNavSpeedLabels[ns]);
  gtav_native_bridge_set_list_value(GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD,
                                    gtav_native_bridge_touchpad_enabled() ? "On" : "Off");
  apply_nav_repeat();
}

const char* gtav_menu_controller_dispatch(uint32_t command, uint64_t argument, const char* source) {
  return dispatch_menu_command(command, argument, source);
}

void gtav_menu_controller_activate(uint32_t action, uint32_t param, int emit_toast) {
  handle_shell_action_param(action, param, emit_toast);
}

uint32_t gtav_menu_controller_parse_command(const char* text) {
  return parse_command_text(text);
}

void gtav_menu_controller_post_input(void) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Apply a menu-visibility request from the game-thread interactive object-move driver (it
  // cannot change visibility itself -- the platform side effects belong to the menu's thread).
  // 1 = hide (entering move mode), 2 = show (committed/cancelled/aborted).
  {
    int vis_req = gtav_features_take_menu_visibility_request();
    if (vis_req == 1) {
      request_visible(0);
    } else if (vis_req == 2) {
      request_visible(1);
    }
  }
  // A queued game-thread job that refused or failed: its reason replaces the "queued" toast.
  {
    char why[GTAV_FEATURE_MESSAGE_LEN];
    uint32_t result = GTAV_FEATURE_RESULT_NONE;
    if (gtav_features_take_job_refusal(why, sizeof(why), &result))
      gtav_native_bridge_push_toast(gtav_locale_translate(why), result);
  }
#endif
  revert_unapplied_pick();
}

int gtav_menu_controller_visible(void) {
  return g_visible;
}

void gtav_menu_controller_store_visible(int visible) {
  g_visible = visible ? 1 : 0;
}

void gtav_menu_controller_apply_visible(int visible) {
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
}

void gtav_menu_controller_refresh_values(void) {
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  push_list_values();
#endif
}
