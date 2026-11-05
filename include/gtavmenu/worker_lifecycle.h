#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GTAV_MENU_WORKER_IDLE = 0,
  GTAV_MENU_WORKER_STARTING = 1,
  GTAV_MENU_WORKER_RUNNING = 2,
  GTAV_MENU_WORKER_STOPPING = 3,
  GTAV_MENU_WORKER_EXITED = 4,
};

// Starts a joinable worker owned by a dynamically loaded module. Success means
// gtav_menu_init completed and the worker reached its tick loop.
int gtav_menu_start_managed(void);

// Requests full menu shutdown and joins the owned worker. It never returns
// while that worker can still execute module code.
int gtav_menu_stop_managed(void);

uint32_t gtav_menu_managed_state(void);

#ifdef __cplusplus
}
#endif
