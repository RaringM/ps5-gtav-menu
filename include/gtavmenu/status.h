#pragma once

#include "gtavmenu/abi.h"

// Shared status block: the single GtavMenuStatus the menu worker writes and the PC reads
// (over process memory) for liveness + telemetry. Single-writer, lock-free -- only the
// worker mutates it (plus the frame hook for the hook-side tick counter), and the remote
// reader tolerates the occasional torn read. The setters below are the only sanctioned
// way to mutate it; threading rule: call them from the WORKER thread, not arbitrary threads.
// In particular the event-ring emitters are worker-thread only -- never call them from the
// game-thread frame hook (it must touch only the scalar hook-side tick counter), or the
// multi-field ring write races the remote reader.

#ifdef __cplusplus
extern "C" {
#endif

// The live status block, and a byte marker the PC scans for to locate it in memory.
extern GtavMenuStatus gtav_menu_status;
extern const char gtav_menu_status_marker[];

// Reset the whole block for a fresh run, seeding identity/config fields from `init`.
void gtav_status_reset(const GtavMenuInit* init);
// Lifecycle/state setters. Codes are the GTAV_MENU_STATE_* / error / hook / log-open
// enums from abi.h.
void gtav_status_set_state(uint32_t state);
void gtav_status_set_init_result(int32_t result);
void gtav_status_set_error(uint32_t error_code, const char* message);
void gtav_status_set_log_open_result(int result);
void gtav_status_set_hook(uint32_t hook_status, uintptr_t hook_addr, uint32_t hook_length,
                          const uint8_t* original, uint32_t original_len);
void gtav_status_set_visible(int visible);
void gtav_status_set_stop_requested(int stop_requested);
void gtav_status_set_initialized(int initialized);
// Counters: gtav_status_tick records the worker tick total; gtav_status_hook_tick adds
// game-thread frame-hook fires.
void gtav_status_tick(uint64_t ticks);
void gtav_status_hook_tick(uint32_t ticks);
// Append a message to the bounded event ring (code = GTAV_MENU_EVENT_*). The `eventf`
// form takes a printf-style format. Messages longer than the slot are truncated. The plain
// forms record the event at info severity; `eventf_lvl` records an explicit GtavLogLevel
// (log.h) so the PC tooling can surface error/warn events distinctly.
void gtav_status_event(uint32_t code, const char* message);
void gtav_status_eventf(uint32_t code, const char* fmt, ...);
void gtav_status_eventf_lvl(uint32_t code, uint32_t level, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
