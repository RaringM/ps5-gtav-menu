#include "gtavmenu/abi.h"
#include "gtavmenu/frame_hook.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/render_diag.h"
#include "gtavmenu/render_phase_intercept.h"
#include "gtavmenu/runtime_config.h"
#include "gtavmenu/status.h"
#include "gtavmenu/worker_lifecycle.h"

#include <pthread.h>
#include <stdatomic.h>
#if defined(__PROSPERO__) || defined(__ORBIS__)
#include <pthread_np.h>  // pthread_set_name_np (FreeBSD/prospero spelling)
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Runtime-tunable worker loop period (microseconds), defined in menu.c. Lets us
 * over-render (worker faster than the game frame rate) to remove phase-beat flicker
 * without a rebuild. */
unsigned gtav_menu_worker_period_us(void);

#ifndef GTAV_MENU_SMOKE_MAX_TICKS
#define GTAV_MENU_SMOKE_MAX_TICKS 0
#endif

#ifndef GTAV_MENU_SKIP_RUNTIME_CONFIG
#define GTAV_MENU_SKIP_RUNTIME_CONFIG 0
#endif

#ifndef GTAV_MENU_MANAGED_START_TIMEOUT_USEC
#define GTAV_MENU_MANAGED_START_TIMEOUT_USEC 5000000u
#endif

#ifndef GTAV_MENU_MANAGED_START_POLL_USEC
#define GTAV_MENU_MANAGED_START_POLL_USEC 1000u
#endif

#ifndef GTAV_MENU_MANAGED_QUIESCE_TIMEOUT_USEC
#define GTAV_MENU_MANAGED_QUIESCE_TIMEOUT_USEC 2000000u
#endif

#ifndef GTAV_MENU_MANAGED_QUIESCE_GRACE_USEC
#define GTAV_MENU_MANAGED_QUIESCE_GRACE_USEC 50000u
#endif

#ifndef GTAV_NEXHEN_MANAGED_LIFECYCLE
#define GTAV_NEXHEN_MANAGED_LIFECYCLE 0
#endif

static atomic_uint g_managed_state;
static atomic_int g_managed_stop;
static atomic_int g_managed_shutdown_result;
static pthread_t g_managed_thread;
static int g_managed_thread_valid;

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "managed worker state must be lock-free");

static void* menu_worker(void* arg) {
  GtavMenuInit init;
  unsigned int ticks = 0;
  int config_loaded = 0;
  const int managed = arg != NULL;

#if defined(__PROSPERO__) || defined(__ORBIS__)
  // Name the worker so a PS5 crash dump / klog identifies this thread by name instead of a
  // blank. An earlier in-gameplay SIGSEGV showed an EMPTY thread name -- that absence was the
  // key signal that the faulting thread was our worker (not "[RAGE] Main Thread"), but it cost
  // real decode time to establish. Guarded to the PS5 target so the host static-test build of
  // main.c (Linux cc, no <pthread_np.h>) still compiles.
  pthread_set_name_np(pthread_self(), "GTAVMenuWorker");
#endif

#if GTAV_MENU_SKIP_RUNTIME_CONFIG
  gtav_runtime_init_defaults(&init);
#else
  config_loaded = gtav_runtime_config_load(GTAV_MENU_DEFAULT_CONFIG, &init) == 0;
  if (!config_loaded) {
    gtav_runtime_init_defaults(&init);
    gtav_log_open(init.log_path);
    gtav_logf("runtime config missing; using safe defaults");
  }
#endif

#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_worker_begin();  // identify init-time native calls as worker calls too
#endif
  if (gtav_menu_init(&init) != 0) {
    if (managed)
      atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_EXITED, memory_order_release);
    return NULL;
  }
  if (managed)
    atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_RUNNING, memory_order_release);
  gtav_status_event(config_loaded ? GTAV_MENU_EVENT_CONFIG_LOADED : GTAV_MENU_EVENT_CONFIG_DEFAULT,
                    config_loaded ? "runtime config loaded"
                                  : "runtime config skipped or missing; using defaults");

  while (!gtav_menu_stop_requested() &&
         (!managed || !atomic_load_explicit(&g_managed_stop, memory_order_acquire))) {
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_worker_begin();
#endif
    gtav_menu_worker_tick();
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_worker_end(gtav_menu_worker_period_us(), gtav_native_bridge_render_interval(),
                                gtav_menu_is_visible(), gtav_native_bridge_is_parked());
#endif
    if (GTAV_MENU_SMOKE_MAX_TICKS && ++ticks >= GTAV_MENU_SMOKE_MAX_TICKS) {
      break;
    }
#if GTAV_MENU_RENDER_DIAGNOSTICS
    gtav_render_diag_sleep_begin(gtav_menu_worker_period_us());
#endif
    usleep(gtav_menu_worker_period_us());
  }

  int shutdown_result = gtav_menu_shutdown();
  if (managed) {
    atomic_store_explicit(&g_managed_shutdown_result, shutdown_result, memory_order_release);
    atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_EXITED, memory_order_release);
  }
  return NULL;
}

uint32_t gtav_menu_managed_state(void) {
  return atomic_load_explicit(&g_managed_state, memory_order_acquire);
}

int gtav_menu_start_managed(void) {
  GtavMenuInit init;
  unsigned int waited = 0;
  unsigned int expected = GTAV_MENU_WORKER_IDLE;

  if (!atomic_compare_exchange_strong_explicit(&g_managed_state, &expected,
                                               GTAV_MENU_WORKER_STARTING, memory_order_acq_rel,
                                               memory_order_acquire)) {
    return expected == GTAV_MENU_WORKER_RUNNING ? 0 : -1;
  }

  atomic_store_explicit(&g_managed_stop, 0, memory_order_release);
  atomic_store_explicit(&g_managed_shutdown_result, -1, memory_order_release);
  gtav_runtime_init_defaults(&init);
  gtav_status_reset(&init);
  gtav_status_event(GTAV_MENU_EVENT_INIT, "managed thread start requested");

  int rc = pthread_create(&g_managed_thread, NULL, menu_worker, &g_managed_state);
  if (rc != 0) {
    gtav_log_open(GTAV_MENU_DEFAULT_LOG);
    gtav_logf("managed pthread_create failed rc=%d", rc);
    gtav_status_set_error(GTAV_MENU_ERROR_THREAD_CREATE_FAILED, "managed pthread_create failed");
    gtav_status_eventf(GTAV_MENU_EVENT_THREAD_FAILED, "managed pthread_create rc=%d", rc);
    atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_IDLE, memory_order_release);
    return -1;
  }
  g_managed_thread_valid = 1;

  while (waited < GTAV_MENU_MANAGED_START_TIMEOUT_USEC) {
    unsigned int state = atomic_load_explicit(&g_managed_state, memory_order_acquire);
    if (state == GTAV_MENU_WORKER_RUNNING) return 0;
    if (state == GTAV_MENU_WORKER_EXITED) break;
    usleep(GTAV_MENU_MANAGED_START_POLL_USEC);
    waited += GTAV_MENU_MANAGED_START_POLL_USEC;
  }

  atomic_store_explicit(&g_managed_stop, 1, memory_order_release);
  gtav_menu_request_stop("managed start did not reach running");
  if (g_managed_thread_valid) {
    pthread_join(g_managed_thread, NULL);
    g_managed_thread_valid = 0;
  }
  atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_IDLE, memory_order_release);
  return -1;
}

int gtav_menu_stop_managed(void) {
  unsigned int state = atomic_load_explicit(&g_managed_state, memory_order_acquire);
  if (state == GTAV_MENU_WORKER_IDLE) return 0;
  if (!g_managed_thread_valid) return -1;

  atomic_store_explicit(&g_managed_stop, 1, memory_order_release);
  atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_STOPPING, memory_order_release);
  gtav_menu_request_stop("nexHEN plugin unload");
  if (pthread_join(g_managed_thread, NULL) != 0) return -1;
  g_managed_thread_valid = 0;
  if (atomic_load_explicit(&g_managed_shutdown_result, memory_order_acquire) != 0) return -1;
#if GTAV_NEXHEN_MANAGED_LIFECYCLE
  unsigned int waited = 0;
  while (waited < GTAV_MENU_MANAGED_QUIESCE_TIMEOUT_USEC) {
    int phase_quiescent = 1;
#if GTAV_RENDER_PHASE_INTERCEPT
    phase_quiescent = __atomic_load_n(&gtav_render_phase_inflight_count, __ATOMIC_ACQUIRE) == 0u;
#endif
    if (__atomic_load_n(&gtav_frame_hook_inflight_count, __ATOMIC_ACQUIRE) == 0u &&
        phase_quiescent) {
      usleep(GTAV_MENU_MANAGED_QUIESCE_GRACE_USEC);
      phase_quiescent = 1;
#if GTAV_RENDER_PHASE_INTERCEPT
      phase_quiescent = __atomic_load_n(&gtav_render_phase_inflight_count, __ATOMIC_ACQUIRE) == 0u;
#endif
      if (__atomic_load_n(&gtav_frame_hook_inflight_count, __ATOMIC_ACQUIRE) == 0u &&
          phase_quiescent) {
        atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_IDLE, memory_order_release);
        return 0;
      }
    }
    usleep(GTAV_MENU_MANAGED_START_POLL_USEC);
    waited += GTAV_MENU_MANAGED_START_POLL_USEC;
  }
  gtav_status_set_error(GTAV_MENU_ERROR_CALLBACK_QUIESCE_FAILED,
                        "nexHEN callback quiescence timed out");
  return -1;
#else
  atomic_store_explicit(&g_managed_state, GTAV_MENU_WORKER_IDLE, memory_order_release);
  return 0;
#endif
}

__attribute__((used, visibility("default"))) int gtav_menu_start_thread(void) {
  GtavMenuInit init;
  pthread_t thread;

  gtav_runtime_init_defaults(&init);
  gtav_status_reset(&init);
  gtav_status_event(GTAV_MENU_EVENT_INIT, "thread start requested");

  int rc = pthread_create(&thread, NULL, menu_worker, NULL);
  if (rc != 0) {
    gtav_log_open(GTAV_MENU_DEFAULT_LOG);
    gtav_logf("pthread_create failed rc=%d", rc);
    gtav_status_set_error(GTAV_MENU_ERROR_THREAD_CREATE_FAILED, "pthread_create failed");
    gtav_status_eventf(GTAV_MENU_EVENT_THREAD_FAILED, "pthread_create rc=%d", rc);
    return -1;
  }
  pthread_detach(thread);
  return 0;
}

#if !defined(GTAV_MENU_NO_STANDALONE_MAIN)
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

#ifdef GTAV_MENU_INJECT_SYNC_SMOKE
  menu_worker(NULL);
  return 0;
#else
  return gtav_menu_start_thread();
#endif
}
#endif
