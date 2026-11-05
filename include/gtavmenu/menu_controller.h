#pragma once

/* Target-neutral menu controller (src/module/menu_controller.c).
 *
 * Owns menu visibility, command dispatch (GTAV_MENU_COMMAND_*), the shell-action handler (menu
 * settings cyclers, Language, one-press confirm of destructive rows, toasts) and the per-tick
 * post-input step. A platform shell (the PS5 worker in menu.c, the PS4 one-shot worker) feeds it
 * input and supplies the effects that differ per platform through GtavMenuPlatformOps. */

#include <stdint.h>
#include <string.h>

#include "gtavmenu/command_mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavMenuPlatformOps {
  /* Platform visibility entry point. It must call gtav_menu_controller_apply_visible and may add
     its own effects (status, notifications). NULL applies the shared change only. */
  void (*set_visible)(int visible);
  /* The Stop row and STOP command; the platform owns its stop sequence. */
  void (*request_stop)(const char* reason);
  /* Worker loop rate. NULL on a platform without a free-running worker (PS4 one-shot hook). */
  void (*set_worker_hz)(uint32_t hz);
  /* Current worker period for status events; NULL reports 0. */
  unsigned (*worker_period_us)(void);
  /* Platform-only commands, offered before the shared switch; NULL result = not handled. */
  const char* (*dispatch_extra)(uint32_t command, uint64_t argument, const char* source);
  /* Each gameplay feature activation and its GTAV_FEATURE_RESULT_* (telemetry); may be NULL. */
  void (*action_result)(uint32_t action, uint32_t param, uint32_t result);
} GtavMenuPlatformOps;

/* Reset controller state and install the platform hooks (copied). */
void gtav_menu_controller_init(const GtavMenuPlatformOps* ops);
/* After the feature profile loaded: apply the persisted menu settings and seed cycler values. */
void gtav_menu_controller_apply_profile_settings(void);
/* Seed every settings row's displayed value from the bridge (all builds). */
void gtav_menu_controller_seed_labels(void);

/* Dispatch one GTAV_MENU_COMMAND_*; returns a short result name ("unknown" when unhandled). */
const char* gtav_menu_controller_dispatch(uint32_t command, uint64_t argument, const char* source);
/* Activate a shell action. emit_toast marks an interactive apply (Cross / host command), which also
   arms the confirm of destructive rows; Left/Right cycler steps pass their own toast rule. */
void gtav_menu_controller_activate(uint32_t action, uint32_t param, int emit_toast);
/* Command-file token -> GTAV_MENU_COMMAND_* (NONE when unknown). */
uint32_t gtav_menu_controller_parse_command(const char* text);
/* Once per tick after all input: feature visibility requests, job refusal toasts, staged-pick
   reversion. */
void gtav_menu_controller_post_input(void);
/* Re-pull every live cycler value from the feature layer. */
void gtav_menu_controller_refresh_values(void);

int gtav_menu_controller_visible(void);
/* Shared visibility change: bridge, game-input suppression, list refresh on open. */
void gtav_menu_controller_apply_visible(int visible);
/* Record visibility without side effects (isolated render diagnostics only). */
void gtav_menu_controller_store_visible(int visible);

/* The shells' mailbox step: dispatch one pending host command and complete it with its result
   (UNKNOWN when nothing handled it). Returns 1 when a command was consumed. Inline, so each shell
   keeps the step in its own loop. */
static inline int gtav_menu_controller_consume_mailbox(void) {
  GtavMenuCommandRequest request;
  const char* result;
  uint32_t status;

  if (!gtav_command_mailbox_consume(&request)) return 0;

  result = gtav_menu_controller_dispatch(request.command, request.argument, "mailbox");
  status =
      strcmp(result, "unknown") ? GTAV_MENU_COMMAND_STATUS_ACKED : GTAV_MENU_COMMAND_STATUS_UNKNOWN;
  gtav_command_mailbox_complete(request.sequence, status, result);
  return 1;
}

#ifdef __cplusplus
}
#endif
