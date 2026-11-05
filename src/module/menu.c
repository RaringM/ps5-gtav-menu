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
#include "gtavmenu/localization.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu_controller.h"
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
static uint64_t g_handler_slot_replacement;
static int g_handler_slot_installed;
static volatile unsigned long long g_ticks;
static volatile unsigned int g_hook_side_ticks;
#if GTAV_MENU_ENABLE_FRAME_HOOK
// Set once the worker has mirrored the first game-thread probe fire to the log ring.
static int g_frame_probe_logged;
#endif
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
  g_handler_slot_replacement = replacement;

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
  uint64_t current;

  if (!g_handler_slot_installed || !g_handler_slot_addr) {
    return 0;
  }

  slot = (uint64_t*)(uintptr_t)g_handler_slot_addr;
  current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (current == g_handler_slot_replacement) {
    __atomic_store_n(slot, g_handler_slot_original, __ATOMIC_RELEASE);
    current = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  }
  if (current != g_handler_slot_original) {
    gtav_logf("handler slot restore failed current=0x%llx original=0x%llx replacement=0x%llx",
              (unsigned long long)current, (unsigned long long)g_handler_slot_original,
              (unsigned long long)g_handler_slot_replacement);
    return -1;
  }
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

void gtav_menu_request_stop(const char* reason) {
  g_stop_requested = 1;
  gtav_status_set_stop_requested(1);
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "stop requested: %s", reason ? reason : "unknown");
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

// PS5-only commands the shared controller offers here first: render probes, the isolated
// diagnostic filter and the privileged spawn-module loader. NULL lets the controller dispatch.
static const char* ps5_dispatch_extra(uint32_t command, uint64_t argument, const char* source) {
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
  if (command == GTAV_MENU_COMMAND_LOAD_MODULE) {
    if (!source) source = "unknown";
    gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "%s command %s", source,
                       gtav_command_mailbox_command_name(command));
    return gtav_menu_load_spawn_module(source);
  }
  return NULL;
}

static const GtavMenuPlatformOps kPs5MenuOps = {
    .set_visible = gtav_menu_set_visible,
    .request_stop = gtav_menu_request_stop,
    .set_worker_hz = set_worker_hz,
    .worker_period_us = gtav_menu_worker_period_us,
    .dispatch_extra = ps5_dispatch_extra,
};

static void consume_command_mailbox(void) {
  (void)gtav_menu_controller_consume_mailbox();
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
  parsed = gtav_menu_controller_parse_command(command);
  if (parsed) {
    gtav_menu_controller_dispatch(parsed, 0, "command file");
  } else {
    gtav_status_event(GTAV_MENU_EVENT_COMMAND, "command file unknown");
  }
}

static void consume_controller_input(unsigned long long tick) {
  uint32_t command =
      gtav_native_bridge_poll_input_command((uint64_t)tick, gtav_menu_controller_visible());

#if GTAV_MENU_ENABLE_PAD_INPUT
  // Direct DualSense fallback (scePad), independent of GTA input natives. Only
  // consulted when the native bridge produced nothing, so an accepted native
  // input path always wins. Init is retried each tick until the controller
  // handle is acquired, then left alone.
  if (command == GTAV_MENU_COMMAND_NONE) {
    if (!gtav_pad_input_available()) {
      gtav_pad_input_init();
    }
    command = gtav_pad_input_poll_command(gtav_menu_controller_visible());
  }
#endif

#if GTAV_MENU_ENABLE_PAD_INPUT
  // A keybind combo fired while the menu was closed: dispatch its bound action directly
  // (with a confirming toast) rather than through the navigation command path.
  if (command == GTAV_MENU_COMMAND_HOTKEY) {
    uint32_t action = gtav_pad_input_last_hotkey_action();
    if (action != GTAV_NATIVE_SHELL_ACTION_NONE) {
      gtav_menu_controller_activate(action, 0, 1);
    }
    return;
  }
#endif

  if (command != GTAV_MENU_COMMAND_NONE) {
    gtav_menu_controller_dispatch(command, 0, "input");
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
  g_handler_slot_replacement = 0;
  g_handler_slot_installed = 0;
  gtav_menu_controller_init(&kPs5MenuOps);
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
  /* Apply the persisted menu settings now that gtav_features_init loaded the profile, and seed
     the list-cycler display values (weather/time) before the user touches them. */
  gtav_menu_controller_apply_profile_settings();
  GTAV_MENU_INIT_TRACE("mi: post push_list_values");
#else
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_BRIDGE, "native features build disabled");
#endif
  /* Arm the main-thread frame hook independent of gameplay natives so the context probe
     (and any probe/calibration lane) runs on the game thread even when native features are
     compiled off. In feature builds this also installs the job-drain consumer. */
  gtav_features_init_frame_hook();
  GTAV_MENU_INIT_TRACE("mi: post init_frame_hook");
  gtav_menu_controller_seed_labels();
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
  gtav_native_bridge_worker_tick(tick, gtav_menu_controller_visible() && !g_stop_requested);
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
  gtav_menu_controller_post_input();
  gtav_native_bridge_worker_tick(tick, gtav_menu_controller_visible());
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Skip per-tick feature work once the quit-guard has parked the renderer for a teardown/suspend:
  // gtav_native_bridge_worker_tick (above) already stood its natives down, and we want the worker
  // wholly GTA-inert so the app can suspend cleanly. No-op gate unless the quit-guard is built in.
  if (!gtav_native_bridge_is_parked()) {
    gtav_features_worker_tick(gtav_menu_controller_visible());
    // Once a second at 60 Hz: copy new "GTAVMenu pack ..." klog lines to the console (klog.c),
    // so a run's pack results survive klog delivery gaps and the status ring.
    if ((tick % 60u) == 0u) (void)gtav_pack_notes_flush();
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

  if (gtav_menu_controller_visible() && (tick % 120u) == 0) {
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
  gtav_native_bridge_frame_tick(frame_tick, gtav_menu_controller_visible());
}

int gtav_menu_render_phase_tick(unsigned long long epoch) {
#if GTAV_RENDER_PHASE_INTERCEPT
  if (!g_initialized || g_stop_requested) return 0;
  return gtav_native_bridge_phase_tick((uint64_t)epoch, gtav_menu_controller_visible());
#else
  (void)epoch;
  return 0;
#endif
}

void gtav_menu_set_visible(int visible) {
#if GTAV_RENDER_DIAG_ISOLATED
  gtav_menu_controller_store_visible(visible);
  gtav_status_set_visible(gtav_menu_controller_visible());
  return;
#endif
  gtav_menu_controller_apply_visible(visible);
  const int now = gtav_menu_controller_visible();
  gtav_status_set_visible(now);
  gtav_status_eventf(GTAV_MENU_EVENT_TOGGLE, "visible=%d", now);
  gtav_logf("menu visibility changed: %s", now ? "visible" : "hidden");
  menu_notify(now ? "GTAVMenu shell visible" : "GTAVMenu shell hidden");
}

void gtav_menu_toggle(void) {
  gtav_menu_set_visible(!gtav_menu_controller_visible());
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

int gtav_menu_shutdown(void) {
  int hook_was_installed = g_frame_detour.installed;
  int handler_slot_was_installed = g_handler_slot_installed;
  int cleanup_failed = 0;

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
  if (gtav_render_phase_restore() != 0) {
    gtav_logf("shutdown: phase callback restoration failed; process must be retired");
    cleanup_failed = 1;
  }
#endif
#if GTAV_MENU_ENABLE_NATIVE_FEATURES && !GTAV_RENDER_DIAG_ISOLATED
  gtav_features_shutdown();
#endif
  gtav_pad_input_wait_menu_buttons_released(GTAV_MENU_SHUTDOWN_INPUT_RELEASE_USEC);

  // Restore the scePadReadState GOT swap before the worker exits so GTA never
  // calls into payload code after Stop Runtime completes. The release grace
  // above keeps the final Stop button press suppressed before this restore.
  gtav_logf("shutdown: restoring scePad GOT swap");
  if (gtav_pad_input_shutdown() != 0) {
    gtav_logf("shutdown: scePad GOT restoration failed; module must remain mapped");
    cleanup_failed = 1;
  }

  if (handler_slot_was_installed) {
    if (restore_native_handler_slot() == 0) {
      hook_was_installed = 0;
    } else {
      cleanup_failed = 1;
    }
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  gtav_logf("shutdown: restoring frame hook (external-install .text jump stays a passthrough)");
  if (gtav_frame_hook_restore() != 0) cleanup_failed = 1;
#endif
  if (hook_was_installed) {
    if (gtav_detour_restore(&g_frame_detour) == 0) {
      gtav_status_set_hook(GTAV_MENU_HOOK_RESTORED, g_frame_detour.address,
                           (uint32_t)g_frame_detour.length, g_frame_detour.original,
                           frame_detour_original_len());
    } else {
      cleanup_failed = 1;
    }
  }
  if (cleanup_failed) {
    gtav_status_set_error(GTAV_MENU_ERROR_SHUTDOWN_FAILED,
                          "shutdown restoration incomplete; module retained");
    gtav_status_set_state(GTAV_MENU_STATE_ERROR);
    gtav_logf("gtav_menu_shutdown incomplete (state=ERROR; module must remain mapped)");
    return -1;
  }

  g_control_update_step_original = NULL;
  g_draw_rect_native_original = NULL;
  g_handler_slot_addr = 0;
  g_handler_slot_original = 0;
  g_handler_slot_replacement = 0;
  gtav_native_bridge_set_draw_rect_call_override(0);
  g_initialized = 0;
  gtav_status_set_initialized(0);
  gtav_status_set_state(GTAV_MENU_STATE_STOPPED);
  remove_inject_lock();
  gtav_logf("gtav_menu_shutdown complete (state=STOPPED)");
  gtav_log_close();
  return 0;
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
  return gtav_menu_controller_visible();
}

unsigned long long gtav_menu_tick_count(void) {
  return g_ticks;
}

int gtav_menu_stop_requested(void) {
  return g_stop_requested;
}
