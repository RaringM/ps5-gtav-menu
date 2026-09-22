#pragma once

#include "gtavmenu/render_cycle_probe.h"

#ifndef GTAV_RENDER_PATH_PROBE
#define GTAV_RENDER_PATH_PROBE 0
#endif
#if GTAV_RENDER_PATH_PROBE && !GTAV_RENDER_CYCLE_PROBE
#error "Callback path observation requires the pinned isolated cycle-probe build"
#endif

#if GTAV_RENDER_PATH_PROBE
#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PATH_MAGIC 0x3148544150545447ull
#define GTAV_PATH_CAPACITY 512u
#define GTAV_PATH_DEPTH 32u
enum {
  GTAV_PATH_BAD_FRAME = 1u,
  GTAV_PATH_NONMONOTONIC = 2u,
  GTAV_PATH_DEPTH_LIMIT = 4u,
  GTAV_PATH_ROOT_LAYOUT_UNKNOWN = 8u,
};
typedef struct GtavRenderPathRecord {
  uint64_t ready, sequence, stamp, thread, context;
  uint64_t epoch, reset_epoch, producer, consumer, dispatch_mode, game_state, gates, bracket;
  uint64_t frame, root_return, root_node, flags, depth, cost;
  uint64_t pcs[GTAV_PATH_DEPTH];
} GtavRenderPathRecord;
typedef struct GtavRenderPathState {
  uint64_t magic, abi, size, phase;
  uint64_t seed_ready, seed_thread, seed_frame, stack_low, stack_high, configured;
  uint64_t callbacks, samples, dropped, wrong_thread, last_epoch, have_epoch;
  uint64_t cost_total, cost_max;
  GtavRenderPathRecord records[GTAV_PATH_CAPACITY];
} GtavRenderPathState;
extern GtavRenderPathState gtav_render_path_probe;
typedef uint64_t (*GtavRenderPathRead)(uintptr_t address, void* context);
// Pure bounded walker for host fixtures. Only [low,high) stack reads, never follows a code/task
// pointer.
void gtav_render_path_walk(uintptr_t frame, uintptr_t low, uintptr_t high, GtavRenderPathRead read,
                           void* context, GtavRenderPathRecord* out);
// Worker-only setup: keys 0/1 set bounds while unarmed; key 2 arms once (1) or disarms forever (2).
int gtav_render_path_control(uint32_t key, uint64_t value);
// Existing callback drain-guard owner only. Unarmed: publish own frame/TLS once; no target reads.
void gtav_render_path_observe(uintptr_t thread, uintptr_t context, uintptr_t frame);

#ifdef __cplusplus
}
#endif
#endif
