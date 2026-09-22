#pragma once

#include "gtavmenu/native_bridge.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_MENU_ABI_VERSION 1u
#define GTAV_MENU_MAX_EXPECTED_BYTES 32u
#define GTAV_MENU_TARGET_ID_LEN 64u
#define GTAV_MENU_PATH_LEN 128u
#define GTAV_MENU_STATUS_MAGIC 0x5441545356415447ull
#define GTAV_MENU_STATUS_ABI_VERSION 1u
#define GTAV_MENU_STATUS_EVENT_COUNT 16u
#define GTAV_MENU_STATUS_EVENT_LEN 80u
#define GTAV_MENU_STATUS_EVENT_SIZE 96u
#define GTAV_MENU_STATUS_SIZE 1888u

enum {
  /* Bit 0 remains reserved so ABI v1 flag values do not move. */
  GTAV_MENU_FLAG_ALLOW_HOOK = 1u << 1,
  GTAV_MENU_FLAG_DRY_RUN = 1u << 2,
  GTAV_MENU_FLAG_NATIVE_DRAW_CANARY = 1u << 3,
};

enum {
  GTAV_MENU_STATE_ZERO = 0,
  GTAV_MENU_STATE_STARTING = 1,
  GTAV_MENU_STATE_RUNNING = 2,
  GTAV_MENU_STATE_STOPPING = 3,
  GTAV_MENU_STATE_STOPPED = 4,
  GTAV_MENU_STATE_ERROR = 5,
};

enum {
  GTAV_MENU_HOOK_UNKNOWN = 0,
  GTAV_MENU_HOOK_DISABLED = 1,
  GTAV_MENU_HOOK_VALIDATING = 2,
  GTAV_MENU_HOOK_DRY_RUN_PASSED = 3,
  GTAV_MENU_HOOK_INSTALLED = 4,
  GTAV_MENU_HOOK_VALIDATION_FAILED = 5,
  GTAV_MENU_HOOK_INSTALL_FAILED = 6,
  GTAV_MENU_HOOK_RESTORED = 7,
};

enum {
  GTAV_MENU_ERROR_NONE = 0,
  GTAV_MENU_ERROR_INVALID_INIT = 1,
  GTAV_MENU_ERROR_HOOK_MISSING_EXPECTED = 2,
  GTAV_MENU_ERROR_HOOK_VALIDATION_FAILED = 3,
  GTAV_MENU_ERROR_HOOK_INSTALL_FAILED = 4,
  GTAV_MENU_ERROR_THREAD_CREATE_FAILED = 5,
  GTAV_MENU_ERROR_LIVE_HOOK_DISABLED = 6,
};

enum {
  GTAV_MENU_EVENT_NONE = 0,
  GTAV_MENU_EVENT_START = 1,
  GTAV_MENU_EVENT_INIT = 2,
  GTAV_MENU_EVENT_LOG_OPEN = 3,
  GTAV_MENU_EVENT_NOTIFY = 4,
  GTAV_MENU_EVENT_HOOK_DISABLED = 5,
  GTAV_MENU_EVENT_HOOK_DRY_RUN_PASSED = 6,
  GTAV_MENU_EVENT_HOOK_INSTALLED = 7,
  GTAV_MENU_EVENT_HOOK_VALIDATION_FAILED = 8,
  GTAV_MENU_EVENT_HOOK_INSTALL_FAILED = 9,
  GTAV_MENU_EVENT_TICK = 10,
  GTAV_MENU_EVENT_TOGGLE = 11,
  GTAV_MENU_EVENT_COMMAND = 12,
  GTAV_MENU_EVENT_SHUTDOWN = 13,
  GTAV_MENU_EVENT_THREAD_FAILED = 14,
  GTAV_MENU_EVENT_CONFIG_LOADED = 15,
  GTAV_MENU_EVENT_CONFIG_DEFAULT = 16,
  GTAV_MENU_EVENT_NATIVE_BRIDGE = 17,
  GTAV_MENU_EVENT_NATIVE_SHELL = 18,
};

typedef struct GtavMenuInit {
  uint32_t abi_version;
  uint32_t flags;
  uint64_t game_base;
  uint64_t text_start;
  uint64_t text_end;
  uint64_t data_start;
  uint64_t data_end;
  uint64_t hook_addr;
  uint32_t hook_length;
  uint32_t expected_len;
  uint8_t expected[GTAV_MENU_MAX_EXPECTED_BYTES];
  char target_id[GTAV_MENU_TARGET_ID_LEN];
  char log_path[GTAV_MENU_PATH_LEN];
  GtavNativeAddressTable native_table;
  uint32_t native_canary_flags;
  uint32_t native_canary_interval;
  uint32_t native_canary_max_frames;
  /* On-screen toast/confirmation dwell time in worker ticks (0 = leave the
     compile-time default; applied via gtav_native_bridge_set_toast_ticks at
     init, which clamps to 30..1800). Reuses the former native_reserved slot. */
  uint32_t toast_ticks;
} GtavMenuInit;

typedef struct GtavMenuStatusEvent {
  uint64_t tick;
  uint32_t code;
  uint32_t level;  // GtavLogLevel (log.h): severity of this event; 0=error..2=info..4=trace
  char message[GTAV_MENU_STATUS_EVENT_LEN];
} GtavMenuStatusEvent;

typedef struct GtavMenuStatus {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t state;
  uint32_t flags;
  uint64_t ticks;
  uint32_t initialized;
  uint32_t visible;
  uint32_t stop_requested;
  int32_t init_result;
  uint32_t last_error;
  uint32_t hook_status;
  uint32_t log_open_result;
  uint32_t hook_side_ticks;
  uint64_t game_base;
  uint64_t hook_addr;
  uint32_t hook_length;
  uint32_t expected_len;
  uint8_t expected[GTAV_MENU_MAX_EXPECTED_BYTES];
  uint8_t original[GTAV_MENU_MAX_EXPECTED_BYTES];
  char target_id[GTAV_MENU_TARGET_ID_LEN];
  char log_path[GTAV_MENU_PATH_LEN];
  uint32_t event_write_index;
  uint32_t event_count;
  GtavMenuStatusEvent events[GTAV_MENU_STATUS_EVENT_COUNT];
} GtavMenuStatus;

int gtav_menu_init(const GtavMenuInit* init);
void gtav_menu_tick(void);
/* gtav_menu_worker_tick (scePad worker thread) and gtav_menu_frame_tick (game thread,
 * driven by the PLAYER_PED_ID frame hook) are declared in gtavmenu/menu.h. */
void gtav_menu_toggle(void);
void gtav_menu_shutdown(void);
int gtav_menu_cam_exists_i32(int cam);
void gtav_menu_control_update_step_hook(void);
void gtav_menu_draw_rect_native_hook(void* native_arg);

#ifdef __cplusplus
}
#endif
