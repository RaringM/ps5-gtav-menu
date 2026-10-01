#pragma once

#include "gtavmenu/render_diag.h"

#ifndef GTAV_RENDER_CYCLE_PROBE
#define GTAV_RENDER_CYCLE_PROBE 0
#endif
#ifndef GTAV_RENDER_CYCLE_TARGET_010
#define GTAV_RENDER_CYCLE_TARGET_010 0
#endif
#ifndef GTAV_RENDER_PHASE_TARGET_VALID
// Compatibility for older diagnostic-only host builds. Versioned production builds define the
// generic target-valid gate from their JSON profile.
#define GTAV_RENDER_PHASE_TARGET_VALID GTAV_RENDER_CYCLE_TARGET_010
#endif
#ifndef GTAV_RENDER_PHASE_INTERCEPT
#define GTAV_RENDER_PHASE_INTERCEPT 0
#endif
#define GTAV_RENDER_CYCLE_GATE (GTAV_RENDER_CYCLE_PROBE || GTAV_RENDER_PHASE_INTERCEPT)
#if GTAV_RENDER_CYCLE_PROBE && (!GTAV_RENDER_DIAG_ISOLATED || !GTAV_RENDER_PHASE_TARGET_VALID)
#error "Cycle observation requires isolated diagnostics and a pinned target"
#elif GTAV_RENDER_PHASE_INTERCEPT && !GTAV_RENDER_PHASE_TARGET_VALID
#error "Cycle gate requires a pinned render-phase target"
#endif

#if GTAV_RENDER_CYCLE_GATE
#ifdef __cplusplus
extern "C" {
#endif

// Exact live addresses come from renderPhase.cycle in the selected target manifest. No fallback
// is safe: these words are read on the game's render thread, so a missing target pin must fail the
// build instead of silently compiling another version's addresses.
#ifndef GTAV_CYCLE_EPOCH_ADDR
#define GTAV_CYCLE_EPOCH_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_RESET_ADDR
#define GTAV_CYCLE_RESET_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_PRODUCER_ADDR
#define GTAV_CYCLE_PRODUCER_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_CONSUMER_ADDR
#define GTAV_CYCLE_CONSUMER_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_COUNT0_ADDR
#define GTAV_CYCLE_COUNT0_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_COUNT1_ADDR
#define GTAV_CYCLE_COUNT1_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_TEXT_ADDR
#define GTAV_CYCLE_TEXT_ADDR 0ull
#endif
#ifndef GTAV_CYCLE_COUNTER_ADDR
#define GTAV_CYCLE_COUNTER_ADDR 0ull
#endif
#if GTAV_RENDER_PHASE_TARGET_VALID &&                                                   \
    (!GTAV_CYCLE_EPOCH_ADDR || !GTAV_CYCLE_RESET_ADDR || !GTAV_CYCLE_PRODUCER_ADDR ||   \
     !GTAV_CYCLE_CONSUMER_ADDR || !GTAV_CYCLE_COUNT0_ADDR || !GTAV_CYCLE_COUNT1_ADDR || \
     !GTAV_CYCLE_TEXT_ADDR || !GTAV_CYCLE_COUNTER_ADDR)
#error "Render-cycle gate requires complete target-manifest cycle addresses"
#endif

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
