#pragma once

#include <stdint.h>

#ifndef GTAV_MENU_RENDER_DIAGNOSTICS
#define GTAV_MENU_RENDER_DIAGNOSTICS 0
#endif
#ifndef GTAV_RENDER_DIAG_INITIAL_MODE
#define GTAV_RENDER_DIAG_INITIAL_MODE 0
#endif
#ifndef GTAV_RENDER_DIAG_ALPHA
#define GTAV_RENDER_DIAG_ALPHA 255
#endif
#ifndef GTAV_RENDER_PHASE_INTERCEPT
#define GTAV_RENDER_PHASE_INTERCEPT 0
#endif
#ifndef GTAV_MENU_PHASE_DRAW_LIST
#define GTAV_MENU_PHASE_DRAW_LIST 0
#endif
#define GTAV_RENDER_DIAG_ISOLATED \
  (GTAV_MENU_RENDER_DIAGNOSTICS && GTAV_RENDER_DIAG_INITIAL_MODE != 0)

#if GTAV_RENDER_DIAG_INITIAL_MODE < 0 || \
    GTAV_RENDER_DIAG_INITIAL_MODE >      \
        (GTAV_MENU_PHASE_DRAW_LIST ? 7 : (GTAV_RENDER_PHASE_INTERCEPT ? 6 : 3))
#error "Game-side diagnostic drawing is unavailable without the phase-interception experiment"
#endif
#if GTAV_RENDER_DIAG_ALPHA != 255 && GTAV_RENDER_DIAG_ALPHA != 128
#error "Diagnostic alpha must be 255 or 128"
#endif
#if !GTAV_MENU_RENDER_DIAGNOSTICS && GTAV_RENDER_DIAG_INITIAL_MODE != 0
#error "Non-normal modes require RENDER_DIAGNOSTICS=1"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GTAV_RD_NORMAL = 0,
  GTAV_RD_DISABLED = 1,
  GTAV_RD_WORKER_RECT = 2,
  GTAV_RD_WORKER_TEXT = 3,
  GTAV_RD_GAME_RECT = 4,
  GTAV_RD_GAME_TEXT = 5,
  GTAV_RD_GAME_COMBINED = 6,
  GTAV_RD_GAME_STATIC_MENU = 7,
};
enum { GTAV_RD_WORKER = 0, GTAV_RD_HOOK = 1 };
enum {
  GTAV_RD_RECT = 0,
  GTAV_RD_SPRITE,
  GTAV_RD_SCALEFORM,
  GTAV_RD_TEXT_BEGIN,
  GTAV_RD_TEXT_ADD,
  GTAV_RD_TEXT_END,
  GTAV_RD_OTHER,
  GTAV_RD_NATIVE_KINDS
};
enum {
  GTAV_RD_TICK = 1,
  GTAV_RD_CALLBACK,
  GTAV_RD_TRANSITION,
  GTAV_RD_COUNTER_OBSERVATION,
  GTAV_RD_CYCLE_OBSERVATION
};
enum {
  GTAV_RD_SKIP_DISABLED = 1,
  GTAV_RD_SKIP_HIDDEN,
  GTAV_RD_SKIP_PARKED,
  GTAV_RD_SKIP_CONTEXT,
  GTAV_RD_SKIP_INTERVAL,
  GTAV_RD_SKIP_WARMUP,
  GTAV_RD_SKIP_ADDRESS,
  GTAV_RD_SKIP_LANE
};

#if GTAV_MENU_RENDER_DIAGNOSTICS
#define GTAV_RENDER_DIAG_CAPACITY 4096u
#define GTAV_RENDER_DIAG_MAGIC 0x3147414944525447ull

// Append-only capture: slots are never recycled in a process. Only ready is published atomically;
// payload is immutable after release publication. Readers ignore uncommitted slots. All fields
// are uint64_t to keep the host read-only decoder independent of C padding/float representation.
typedef struct GtavRenderDiagEvent {
  uint64_t ready, sequence, stamp, kind, lane, config, thread_key, context;
  uint64_t tick, duration, wall_ns, work_ns, wake_late_ns, skip;
  uint64_t attempts, completed, settings;
} GtavRenderDiagEvent;

typedef struct GtavRenderDiagState {
  uint64_t magic, abi, size, capacity, event_size, initial_mode, clock_kind;
  uint64_t config, capture, reserved, dropped;
  uint64_t hook_raw, hook_accepted, hook_guard_rejected, hook_no_context;
  uint64_t worker_ticks, blocked_jobs, suppressed_snapshots, worker_thread;
  uint64_t worker_period_us, render_interval, visible, parked;
  uint64_t snapshot_inflight, snapshot_overlaps;
  uint64_t attempted[2][GTAV_RD_NATIVE_KINDS];
  uint64_t completed[2][GTAV_RD_NATIVE_KINDS];
  GtavRenderDiagEvent events[GTAV_RENDER_DIAG_CAPACITY];
} GtavRenderDiagState;

extern GtavRenderDiagState gtav_render_diag;
void gtav_render_diag_emit(GtavRenderDiagEvent* event);
uint64_t gtav_render_diag_stamp(void);
uint64_t gtav_render_diag_thread(void);  // TLS identity, NOT an OS thread id
uint64_t gtav_render_diag_config(void);
// Worker boundary only. Normal <-> isolated requires a fresh game process/build.
int gtav_render_diag_select(uint32_t mode, uint32_t alpha);
int gtav_render_diag_capture(uint32_t operation);  // 1=start once, 2=stop permanently
int gtav_render_diag_warming(void);
void gtav_render_diag_worker_begin(void);
void gtav_render_diag_worker_end(uint32_t period_us, uint32_t interval, int visible, int parked);
void gtav_render_diag_sleep_begin(uint32_t period_us);
void gtav_render_diag_skip(uint32_t reason);
void gtav_render_diag_hook_count(uint32_t which);  // 0 raw, 1 accepted, 2 guard, 3 no-context
void gtav_render_diag_hook_end(uint64_t start, uintptr_t thread, uintptr_t context);
void gtav_render_diag_suppressed_snapshot(void);
void gtav_render_diag_snapshot(int entering);
void gtav_render_diag_blocked_job(void);
void gtav_render_diag_bind(uint32_t kind, uint64_t address);
void gtav_render_diag_bind_counter(uint64_t address);
void gtav_render_diag_native(uint64_t address, int completed);
// Observe only an existing GET_FRAME_COUNT return, never issue an observation native.
void gtav_render_diag_native_return(uint64_t address, uint64_t value);
#endif

#ifdef __cplusplus
}
#endif
