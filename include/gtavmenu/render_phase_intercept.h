#pragma once

#include <stdint.h>

#include "gtavmenu/render_cycle_probe.h"

#ifndef GTAV_RENDER_PHASE_INTERCEPT
#define GTAV_RENDER_PHASE_INTERCEPT 0
#endif

#ifndef GTAV_MENU_PHASE_DRAW_LIST
#define GTAV_MENU_PHASE_DRAW_LIST 0
#endif

#if GTAV_RENDER_PHASE_INTERCEPT && \
    (!GTAV_RENDER_CYCLE_TARGET_010 || (!GTAV_RENDER_DIAG_ISOLATED && !GTAV_MENU_PHASE_DRAW_LIST))
#error "Phase interception requires isolated diagnostics or the .010.002 phase draw list"
#endif

#if GTAV_RENDER_PHASE_INTERCEPT
#ifdef __cplusplus
extern "C" {
#endif

// Pinned to normalized ELF SHA-256 2a3419b4...953c0d. These are live addresses.
#define GTAV_RENDER_PHASE_ORIGINAL 0x1cae9c0ull
#define GTAV_RENDER_PHASE_LEAF_VTABLE 0x408f118ull
#define GTAV_RENDER_PHASE_TASK_ID 0x249760f7u

#define GTAV_RENDER_PHASE_MAGIC 0x3148505249545447ull
enum {
  GTAV_RENDER_PHASE_UNCONFIGURED = 0,
  GTAV_RENDER_PHASE_READY = 1,
  GTAV_RENDER_PHASE_INSTALL_PENDING = 2,
  GTAV_RENDER_PHASE_INSTALLED = 3,
  GTAV_RENDER_PHASE_RESTORE_PENDING = 4,
  GTAV_RENDER_PHASE_RESTORED = 5,
  GTAV_RENDER_PHASE_FAILED = 6,
};
enum {
  GTAV_RENDER_PHASE_ERROR_NONE = 0,
  GTAV_RENDER_PHASE_ERROR_CONFIG = 1,
  GTAV_RENDER_PHASE_ERROR_OBJECT = 2,
  GTAV_RENDER_PHASE_ERROR_INSTALL_CAS = 3,
  GTAV_RENDER_PHASE_ERROR_RESTORE_CAS = 4,
};

typedef struct GtavRenderPhaseState {
  uint64_t magic, abi, size, phase;
  uint64_t slot, object, original, wrapper;
  uint64_t services, installs, restores, wrapper_calls;
  uint64_t draw_attempts, draw_completed, duplicate_epochs, gate_rejections;
  uint64_t first_epoch, last_epoch, last_flags, last_object, error;
} GtavRenderPhaseState;

extern GtavRenderPhaseState gtav_render_phase_state;
extern uintptr_t gtav_render_phase_original;

// key 0: configure the exact callback-object +0x20 slot while unconfigured.
// key 1: operation 1 requests install; operation 2 restores exactly and permanently.
int gtav_render_phase_control(uint32_t key, uint64_t value);

// Called under the existing accepted PLAYER_PED_ID drain guard. It performs at most one aligned
// compare-exchange and no allocation, blocking, I/O, formatting, or native invocation.
void gtav_render_phase_service(void);

// Called by the state-preserving assembly wrapper before it tail-jumps to the original callback.
void gtav_render_phase_wrapper_body(uintptr_t object);
void gtav_render_phase_wrapper(void);

// Best-effort exact restoration for shutdown. Returns 0 only when the slot is original afterwards.
int gtav_render_phase_restore(void);

#ifdef __cplusplus
}
#endif
#endif
