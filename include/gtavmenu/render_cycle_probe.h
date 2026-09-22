#pragma once

#include "gtavmenu/render_diag.h"

#ifndef GTAV_RENDER_CYCLE_PROBE
#define GTAV_RENDER_CYCLE_PROBE 0
#endif
#ifndef GTAV_RENDER_CYCLE_TARGET_010
#define GTAV_RENDER_CYCLE_TARGET_010 0
#endif
#ifndef GTAV_RENDER_PHASE_INTERCEPT
#define GTAV_RENDER_PHASE_INTERCEPT 0
#endif
#define GTAV_RENDER_CYCLE_GATE (GTAV_RENDER_CYCLE_PROBE || GTAV_RENDER_PHASE_INTERCEPT)
#if GTAV_RENDER_CYCLE_PROBE && (!GTAV_RENDER_DIAG_ISOLATED || !GTAV_RENDER_CYCLE_TARGET_010)
#error "Cycle observation requires isolated diagnostics for ppsa04264-01.010.002"
#elif GTAV_RENDER_PHASE_INTERCEPT && !GTAV_RENDER_CYCLE_TARGET_010
#error "Cycle gate is pinned only for ppsa04264-01.010.002"
#endif

#if GTAV_RENDER_CYCLE_GATE
#ifdef __cplusplus
extern "C" {
#endif

// Read-only observations for eboot SHA-256 2a3419b4...953c0d. Live addresses, not RVAs.
// These are NOT a validated drawing gate. See docs/render-phase-intercept.md.
#define GTAV_CYCLE_EPOCH_ADDR 0x61a623cu
#define GTAV_CYCLE_RESET_ADDR 0x546e200u
#define GTAV_CYCLE_PRODUCER_ADDR 0x61a6ae8u
#define GTAV_CYCLE_CONSUMER_ADDR 0x61a6aecu
#define GTAV_CYCLE_COUNT0_ADDR 0x5479df8u
#define GTAV_CYCLE_COUNT1_ADDR 0x5485980u
#define GTAV_CYCLE_TEXT_ADDR 0x5486dc8u
#define GTAV_CYCLE_COUNTER_ADDR 0x61a6264u

enum {
  GTAV_CYCLE_CHANGED_DURING_READ = 1u,
  GTAV_CYCLE_INVALID_SELECTOR = 2u,
  GTAV_CYCLE_INVALID_COUNT = 4u,
  GTAV_CYCLE_TEXT_BUSY = 8u,
  GTAV_CYCLE_RESET_DIFFERS = 16u,
  GTAV_CYCLE_SELECTORS_EQUAL = 32u,
};
typedef struct GtavRenderCycleSample {
  uint32_t epoch, reset_epoch, producer, consumer, count0, count1, text_mode, game_counter;
} GtavRenderCycleSample;

#define GTAV_RENDER_CYCLE_MAGIC 0x31454c4359435447ull
typedef struct GtavRenderCycleState {
  uint64_t magic, abi, size, armed;
  uint64_t observations, changed_reads, invalid_selectors, invalid_counts;
  uint64_t text_busy, reset_differs, selectors_equal, epoch_changes, epoch_gaps;
  uint64_t thread_changes, first_thread, last_thread, last_epoch;
  uint64_t observation_tsc_total, observation_tsc_max;
} GtavRenderCycleState;
#if GTAV_RENDER_CYCLE_PROBE
extern GtavRenderCycleState gtav_render_cycle_probe;
#endif
typedef uint32_t (*GtavRenderCycleReadWord)(uintptr_t address, void* context);

// Exactly two fixed reads of each word, no pointer chasing or retry. Equality cannot detect ABA
// or establish a transactional engine snapshot. Exported for meaningful host reader fixtures.
uint32_t gtav_render_cycle_read(GtavRenderCycleReadWord read_word, void* context,
                                GtavRenderCycleSample* out);
// Same bounded double read against the pinned target words. This exposes the existing validation
// primitive to the phase experiment without turning the observer's historical state into a gate.
uint32_t gtav_render_cycle_sample_target(GtavRenderCycleSample* out);
#if GTAV_RENDER_CYCLE_PROBE
// Arm once, only after research/tools/render/render_diagnostics.py cycle-arm has verified live pins
// + mappings. 1 = arm; 2 = permanently disarm. Does not start/rearm the append-only event capture.
int gtav_render_cycle_arm(uint32_t operation);
// Called only while owning the existing callback drain guard. Never used as a rendering gate.
void gtav_render_cycle_observe(uintptr_t thread, uintptr_t context);
#endif

#ifdef __cplusplus
}
#endif
#endif
