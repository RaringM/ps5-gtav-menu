#include "gtavmenu/status.h"

#include "gtavmenu/log.h"
#include "gtavmenu/strutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(GtavMenuStatusEvent) == GTAV_MENU_STATUS_EVENT_SIZE,
               "GtavMenuStatusEvent size changed");
_Static_assert(sizeof(GtavMenuStatus) == GTAV_MENU_STATUS_SIZE, "GtavMenuStatus size changed");

GtavMenuStatus gtav_menu_status __attribute__((used, visibility("default"))) = {
    .magic = GTAV_MENU_STATUS_MAGIC,
    .abi_version = GTAV_MENU_STATUS_ABI_VERSION,
    .struct_size = sizeof(GtavMenuStatus),
    .state = GTAV_MENU_STATE_ZERO,
    .hook_status = GTAV_MENU_HOOK_UNKNOWN,
};

const char gtav_menu_status_marker[] __attribute__((used, visibility("default"))) =
    "GTAVMENU_STATUS_V1";

static void status_event_store(uint32_t code, uint32_t level, const char* message);

static void init_header(void) {
  gtav_menu_status.magic = GTAV_MENU_STATUS_MAGIC;
  gtav_menu_status.abi_version = GTAV_MENU_STATUS_ABI_VERSION;
  gtav_menu_status.struct_size = sizeof(GtavMenuStatus);
}

void gtav_status_reset(const GtavMenuInit* init) {
  memset(&gtav_menu_status, 0, sizeof(gtav_menu_status));
  init_header();
  gtav_menu_status.state = GTAV_MENU_STATE_STARTING;
  gtav_menu_status.hook_status = GTAV_MENU_HOOK_UNKNOWN;

  if (init) {
    gtav_menu_status.flags = init->flags;
    gtav_menu_status.game_base = init->game_base;
    gtav_menu_status.hook_addr = init->hook_addr;
    gtav_menu_status.hook_length = init->hook_length;
    gtav_menu_status.expected_len = init->expected_len;
    if (init->expected_len <= GTAV_MENU_MAX_EXPECTED_BYTES) {
      memcpy(gtav_menu_status.expected, init->expected, init->expected_len);
    }
    gtav_copy_string(gtav_menu_status.target_id, sizeof(gtav_menu_status.target_id),
                     init->target_id);
    gtav_copy_string(gtav_menu_status.log_path, sizeof(gtav_menu_status.log_path), init->log_path);
  }

  gtav_status_event(GTAV_MENU_EVENT_START, "status reset");
}

void gtav_status_set_state(uint32_t state) {
  gtav_menu_status.state = state;
}

void gtav_status_set_init_result(int32_t result) {
  gtav_menu_status.init_result = result;
}

void gtav_status_set_error(uint32_t error_code, const char* message) {
  gtav_menu_status.last_error = error_code;
  gtav_menu_status.state = GTAV_MENU_STATE_ERROR;
  if (message && message[0]) {
    status_event_store(error_code, GTAV_LOG_ERROR, message);
  }
}

void gtav_status_set_log_open_result(int result) {
  gtav_menu_status.log_open_result = (uint32_t)result;
  gtav_status_eventf(GTAV_MENU_EVENT_LOG_OPEN, "log_open_result=%d", result);
}

void gtav_status_set_hook(uint32_t hook_status, uintptr_t hook_addr, uint32_t hook_length,
                          const uint8_t* original, uint32_t original_len) {
  gtav_menu_status.hook_status = hook_status;
  gtav_menu_status.hook_addr = (uint64_t)hook_addr;
  gtav_menu_status.hook_length = hook_length;
  if (original && original_len <= GTAV_MENU_MAX_EXPECTED_BYTES) {
    memcpy(gtav_menu_status.original, original, original_len);
  }
}

void gtav_status_set_visible(int visible) {
  gtav_menu_status.visible = visible ? 1u : 0u;
}

void gtav_status_set_stop_requested(int stop_requested) {
  gtav_menu_status.stop_requested = stop_requested ? 1u : 0u;
}

void gtav_status_set_initialized(int initialized) {
  gtav_menu_status.initialized = initialized ? 1u : 0u;
}

void gtav_status_tick(uint64_t ticks) {
  gtav_menu_status.ticks = ticks;
}

void gtav_status_hook_tick(uint32_t ticks) {
  gtav_menu_status.hook_side_ticks = ticks;
}

// Single slot writer shared by every event emitter (the only multi-field RMW on the ring).
static void status_event_store(uint32_t code, uint32_t level, const char* message) {
  uint32_t index = gtav_menu_status.event_write_index % GTAV_MENU_STATUS_EVENT_COUNT;
  GtavMenuStatusEvent* event = &gtav_menu_status.events[index];

  event->tick = gtav_menu_status.ticks;
  event->code = code;
  event->level = level;
  gtav_copy_string(event->message, sizeof(event->message), message);

  gtav_menu_status.event_write_index++;
  if (gtav_menu_status.event_count < GTAV_MENU_STATUS_EVENT_COUNT) {
    gtav_menu_status.event_count++;
  }
}

void gtav_status_event(uint32_t code, const char* message) {
  status_event_store(code, GTAV_LOG_INFO, message);
}

void gtav_status_eventf(uint32_t code, const char* fmt, ...) {
  char message[GTAV_MENU_STATUS_EVENT_LEN];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  status_event_store(code, GTAV_LOG_INFO, message);
}

void gtav_status_eventf_lvl(uint32_t code, uint32_t level, const char* fmt, ...) {
  char message[GTAV_MENU_STATUS_EVENT_LEN];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);

  status_event_store(code, level, message);
}
