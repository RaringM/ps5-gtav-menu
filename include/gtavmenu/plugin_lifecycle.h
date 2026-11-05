#pragma once

/* Load/unload serialization for a plugin whose host may call its entry points from several threads
 * at once (the host's plugin_load, plugin_unload and module_stop). One atomic state:
 *
 *   IDLE --load--> STARTING --thread up--> RUNNING --unload--> STOPPING --joined--> STOPPED
 *                     `--thread failed--> START_FAILED
 *
 * Each transition is won by exactly one caller with a compare-and-exchange. A stopper that loses
 * waits (bounded, by its own sleeps) for STOPPED and returns the stored result, so the supervisor
 * thread is joined once. STOPPED re-arms to IDLE only when the last cleanup returned 0: a failed
 * cleanup may have left game code reaching into the plugin, so it never starts again in this
 * process. START_FAILED re-arms (nothing ran). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GTAV_PLUGIN_IDLE = 0,
  GTAV_PLUGIN_STARTING = 1,
  GTAV_PLUGIN_RUNNING = 2,
  GTAV_PLUGIN_STOPPING = 3,
  GTAV_PLUGIN_STOPPED = 4,
  GTAV_PLUGIN_START_FAILED = 5,
};

typedef struct {
  uint32_t state;
  /* What a load or stop returns once the state is terminal (STOPPED / START_FAILED). */
  int32_t result;
  /* 1 when the last cleanup returned 0 (STOPPED may re-arm). */
  uint32_t rearmable;
} GtavPluginLifecycle;

static inline uint32_t gtav_plugin_lifecycle_state(GtavPluginLifecycle* lifecycle) {
  return __atomic_load_n(&lifecycle->state, __ATOMIC_ACQUIRE);
}

/* Valid once gtav_plugin_lifecycle_state returned a terminal state. */
static inline int32_t gtav_plugin_lifecycle_result(GtavPluginLifecycle* lifecycle) {
  return __atomic_load_n(&lifecycle->result, __ATOMIC_ACQUIRE);
}

/* 1 when this caller moved the state from `from` to `to`. */
static inline int gtav_plugin_lifecycle_claim(GtavPluginLifecycle* lifecycle, uint32_t from,
                                              uint32_t to) {
  uint32_t expected = from;
  return __atomic_compare_exchange_n(&lifecycle->state, &expected, to, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
}

/* The owner of a claim publishes its outcome, then the next state. */
static inline void gtav_plugin_lifecycle_finish(GtavPluginLifecycle* lifecycle, uint32_t to,
                                                int32_t result, int rearmable) {
  __atomic_store_n(&lifecycle->result, result, __ATOMIC_RELAXED);
  __atomic_store_n(&lifecycle->rearmable, rearmable ? 1u : 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&lifecycle->state, to, __ATOMIC_RELEASE);
}

/* IDLE again after a failed start or a clean stop. 1 when the lifecycle is IDLE afterwards. */
static inline int gtav_plugin_lifecycle_rearm(GtavPluginLifecycle* lifecycle) {
  const uint32_t state = gtav_plugin_lifecycle_state(lifecycle);
  if (state == GTAV_PLUGIN_START_FAILED ||
      (state == GTAV_PLUGIN_STOPPED && __atomic_load_n(&lifecycle->rearmable, __ATOMIC_RELAXED))) {
    (void)gtav_plugin_lifecycle_claim(lifecycle, state, GTAV_PLUGIN_IDLE);
  }
  return gtav_plugin_lifecycle_state(lifecycle) == GTAV_PLUGIN_IDLE;
}

#ifdef __cplusplus
}
#endif
