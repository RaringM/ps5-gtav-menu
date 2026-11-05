#pragma once

/* PS4 menu core: control block ABI and the internal entry points both deliveries share.
 *
 * The loader (the PS4 plugin runtime, or a debugger-side host for the development worker) fills
 * the HOST-owned control fields and installs a relative jump in PLAYER_PED_ID. The
 * hooked call runs the original first and then, at most once per game frame and only with a real
 * player ped, the whole menu on the game thread. There is no worker thread. Host commands arrive
 * through the shared command mailbox (gtav_menu_command_mailbox); physical pad input is sampled on
 * the game thread. Field layout: gtavmenu/ps4_control_fields.def. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PS4_CONTROL_MAGIC UINT64_C(0x43534c5053344754)
#define GTAV_PS4_CONTROL_ABI 2u
#define GTAV_PS4_CONTROL_SIZE 512u
#define GTAV_PS4_CONTROL_LOADER_SEEN UINT32_C(0x3154454c)
#define GTAV_PS4_STORAGE_PROBE_DONE UINT32_C(0x31425250)
/* Game frames the shutdown sequence may spend releasing owned entities before it gives up. */
#define GTAV_PS4_SHUTDOWN_MAX_FRAMES 90u
/* Game-thread jobs drained per frame (the PS5 frame hook drains up to four per fire). */
#define GTAV_PS4_JOBS_PER_FRAME 4u

enum {
  GTAV_PS4_STATE_EMPTY = 0,
  GTAV_PS4_STATE_LOADED = 1,
  GTAV_PS4_STATE_RUNNING = 3,
  GTAV_PS4_STATE_STOPPING = 4,
  GTAV_PS4_STATE_ERROR = 6,
};

/* host_flags: written only by the host. */
enum {
  GTAV_PS4_HOST_UI_ALLOWED = 1u << 0,
  GTAV_PS4_HOST_QUIESCE = 1u << 1,
  GTAV_PS4_HOST_DIAG_DRAW = 1u << 3,
  GTAV_PS4_HOST_HOOK_ENABLED = 1u << 4,
  GTAV_PS4_HOST_SHUTDOWN_REQUEST = 1u << 5,
  GTAV_PS4_HOST_STORAGE_READY = 1u << 6,
  /* An earlier instance in this process stopped without the cooperative shutdown: run Disable All
     once on the first active frame, before the profile import. */
  GTAV_PS4_HOST_RESET_FEATURES = 1u << 7,
};

/* worker_flags: written only by the worker. */
enum {
  /* The worker stopped all game work (after shutdown or a fatal readiness error); the hook only
     chains the original from here on. */
  GTAV_PS4_WORKER_SELF_QUIESCE = 1u << 0,
  /* The menu's Stop row or STOP command asked for the runtime to be stopped by the host. */
  GTAV_PS4_WORKER_MENU_STOP = 1u << 1,
  /* Features were turned off and queued jobs dropped on the game thread. */
  GTAV_PS4_WORKER_SHUTDOWN_DONE = 1u << 2,
  GTAV_PS4_WORKER_DIAG_DONE = 1u << 3,
  /* The reset GTAV_PS4_HOST_RESET_FEATURES asked for ran. */
  GTAV_PS4_WORKER_FEATURES_RESET = 1u << 4,
};

enum {
  GTAV_PS4_NATIVE_GETTERS = 1u << 0,
  GTAV_PS4_NATIVE_DRAW = 1u << 1,
  GTAV_PS4_NATIVE_TEXT = 1u << 2,
  GTAV_PS4_NATIVE_INPUT = 1u << 3,
  GTAV_PS4_NATIVE_ALL = 0xfu,
};

enum {
  GTAV_PS4_ERROR_NONE = 0,
  GTAV_PS4_ERROR_GATEWAY = 2,
  GTAV_PS4_ERROR_GETTER = 3,
  GTAV_PS4_ERROR_SHUTDOWN_TIMEOUT = 7,
};

typedef struct GtavPs4Control {
#define GTAV_PS4_FIELD(type, name, format, owner) type name;
#define GTAV_PS4_ARRAY(type, name, count, format, owner) type name[count];
#include "gtavmenu/ps4_control_fields.def"
#undef GTAV_PS4_FIELD
#undef GTAV_PS4_ARRAY
} GtavPs4Control;

#ifdef __cplusplus
static_assert(sizeof(GtavPs4Control) == GTAV_PS4_CONTROL_SIZE, "PS4 control size drift");
#else
_Static_assert(sizeof(GtavPs4Control) == GTAV_PS4_CONTROL_SIZE, "PS4 control size drift");
#endif

extern GtavPs4Control gtav_ps4_control;

/* game.cpp: native binding, the frame hook, and the per-frame game-thread pipeline. */
void gtav_ps4_game_init(void);
void gtav_ps4_frame_hook(void* native_context);

/* worker.c: the menu adapter over the shared controller (called from the game thread). */
void gtav_ps4_menu_init(void);
/* One menu tick per game frame: the host mailbox, then the pad command sampled this frame
   (GTAV_MENU_COMMAND_*, NONE = nothing), the post-input step, bridge and feature worker ticks. */
void gtav_ps4_menu_frame(uint32_t pad_command);
/* Hide the menu without host involvement (shutdown). */
void gtav_ps4_menu_hide(void);

/* Stat /app0, /data and /data/GTAVMenu into the control block. Read-only; no GTA natives. */
void gtav_ps4_storage_probe(GtavPs4Control* control);

/* control.c: the load step both deliveries run before any hook exists (the development worker's
   _main, the PS4 plugin runtime once Story Mode is up): the read-only storage probe, then
   gtav_ps4_game_init. No GTA native runs here. */
void gtav_ps4_runtime_load(void);

/* bridge.cpp: where gtav_worker_klog lines (the pack breadcrumbs) go. The development worker sets
   none and drops them; the PS4 plugin passes its log ring. The sink is called on whatever thread
   logs, the hooked game thread included, so it must queue without blocking. NULL drops again. */
typedef void (*GtavPs4KlogSink)(const char* message);
void gtav_ps4_set_klog_sink(GtavPs4KlogSink sink);

#ifdef __cplusplus
}
#endif
