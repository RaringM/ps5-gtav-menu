/* PS4 menu adapter: drives the shared menu controller from the hooked game thread.
 *
 * Everything menu-related -- dispatch, settings cyclers, confirm, toasts, visibility -- is the
 * shared controller (menu_controller.c) and render bridge. This file only supplies the PS4 platform
 * hooks (visibility mirror, Stop row, action telemetry) and feeds it host mailbox commands and pad
 * commands once per game frame. */

#include "gtavmenu/command_mailbox.h"
#include "gtavmenu/features.h"
#include "gtavmenu/menu_controller.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/ps4_slice.h"
#include "gtavmenu/ps4_stdio.h"

#include <stddef.h>
#include <string.h>

#ifndef GTAV_PS4_ENABLE_STORAGE
#define GTAV_PS4_ENABLE_STORAGE 0
#endif

enum {
  GTAV_PS4_STORAGE_OFF = 0,
  GTAV_PS4_STORAGE_ENABLED = 1,
  GTAV_PS4_STORAGE_PROFILE_LOADED = 2,
  GTAV_PS4_STORAGE_PROFILE_MISSING = 3,
};

static uint64_t g_menu_ticks;
static int g_seeded;

static void ps4_set_visible(int visible) {
  gtav_menu_controller_apply_visible(visible);
  __atomic_store_n(&gtav_ps4_control.visible, (uint32_t)gtav_menu_controller_visible(),
                   __ATOMIC_RELEASE);
}

/* The Stop row / STOP command: the game thread runs the feature shutdown on its next frame and the
   host's stop restores the hook (worker_flags tells it why). */
static void ps4_request_stop(const char* reason) {
  (void)reason;
  __atomic_or_fetch(&gtav_ps4_control.worker_flags, GTAV_PS4_WORKER_MENU_STOP, __ATOMIC_ACQ_REL);
}

static void ps4_action_result(uint32_t action, uint32_t param, uint32_t result) {
  gtav_ps4_control.last_feature_action = action;
  gtav_ps4_control.last_feature_param = param;
  gtav_ps4_control.feature_calls++;
  if (result == GTAV_FEATURE_RESULT_OK) gtav_ps4_control.feature_ok++;
}

static const GtavMenuPlatformOps kPs4MenuOps = {
    .set_visible = ps4_set_visible,
    .request_stop = ps4_request_stop,
    .set_worker_hz = NULL, /* the menu ticks once per game frame; there is no worker loop */
    .worker_period_us = NULL,
    .dispatch_extra = NULL,
    .action_result = ps4_action_result,
};

/* Runs from _main on the debugger's loader thread: no natives, state only. */
void gtav_ps4_menu_init(void) {
  gtav_menu_controller_init(&kPs4MenuOps);
  gtav_command_mailbox_reset();
  g_menu_ticks = 0;
  g_seeded = 0;
}

/* Storage builds: once the host grants /data (GTAV_PS4_HOST_STORAGE_READY), open the layer,
   create the menu's folders and load the saved profile. Runs on the game thread because importing a
   profile re-applies features through their natives. Returns 1 when a profile was applied. */
static int open_storage(void) {
#if GTAV_PS4_ENABLE_STORAGE
  if (gtav_ps4_stdio_enabled()) return 0;
  const uint32_t host = __atomic_load_n(&gtav_ps4_control.host_flags, __ATOMIC_ACQUIRE);
  if ((host & GTAV_PS4_HOST_STORAGE_READY) == 0) return 0;
  gtav_ps4_stdio_set_enabled(1);
  (void)gtav_ps4_mkdir("/data/GTAVMenu", 0777);
  (void)gtav_ps4_mkdir("/data/GTAVMenu/state", 0777);
  const int loaded = gtav_features_profile_load_default() == 0;
  gtav_ps4_control.storage_state =
      loaded ? GTAV_PS4_STORAGE_PROFILE_LOADED : GTAV_PS4_STORAGE_PROFILE_MISSING;
  return loaded;
#else
  return 0;
#endif
}

/* First UI frame on the game thread: seeding the cycler values reads feature state, which may
   call natives, so it cannot happen at load time. A profile that arrives later re-applies the
   menu settings it carries. */
static void seed_once(void) {
  const int profile_applied = open_storage();
  if (g_seeded && !profile_applied) return;
  g_seeded = 1;
  gtav_menu_controller_apply_profile_settings();
  gtav_menu_controller_seed_labels();
}

static void consume_mailbox(void) {
  if (gtav_menu_controller_consume_mailbox()) gtav_ps4_control.mailbox_commands++;
}

static void consume_pad(uint32_t command) {
  if (command == GTAV_MENU_COMMAND_NONE) return;
  gtav_ps4_control.pad_commands++;
  if (command == GTAV_MENU_COMMAND_HOTKEY) {
    /* A keybind combo fired while the menu was closed: run its bound action with a toast. */
    const uint32_t action = gtav_pad_input_last_hotkey_action();
    if (action != GTAV_NATIVE_SHELL_ACTION_NONE) gtav_menu_controller_activate(action, 0, 1);
    return;
  }
  (void)gtav_menu_controller_dispatch(command, 0, "input");
}

void gtav_ps4_menu_frame(uint32_t pad_command) {
  seed_once();
  consume_mailbox();
  consume_pad(pad_command);
  gtav_menu_controller_post_input();
  const int visible = gtav_menu_controller_visible();
  gtav_native_bridge_worker_tick(++g_menu_ticks, visible);
  gtav_features_worker_tick(visible);
  GtavNativeShellSnapshot snapshot;
  gtav_native_bridge_snapshot(&snapshot);
  gtav_ps4_control.selection = snapshot.selected_index;
  gtav_ps4_control.current_menu = gtav_native_bridge_current_menu();
  gtav_ps4_control.selected_action = gtav_native_bridge_selected_action();
  __atomic_store_n(&gtav_ps4_control.visible, (uint32_t)visible, __ATOMIC_RELEASE);
}

void gtav_ps4_menu_hide(void) {
  if (gtav_menu_controller_visible()) ps4_set_visible(0);
}
