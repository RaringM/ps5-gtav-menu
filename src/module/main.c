#include "gtavmenu/abi.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/render_diag.h"
#include "gtavmenu/runtime_config.h"
#include "gtavmenu/status.h"

#include <pthread.h>
#if defined(__PROSPERO__) || defined(__ORBIS__)
#include <pthread_np.h>  // pthread_set_name_np (FreeBSD/prospero spelling)
#endif
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int gtav_menu_stop_requested(void);
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

static void* menu_worker(void* arg) {
  GtavMenuInit init;
  unsigned int ticks = 0;
  int config_loaded = 0;

  (void)arg;

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
    return NULL;
  }
  gtav_status_event(config_loaded ? GTAV_MENU_EVENT_CONFIG_LOADED : GTAV_MENU_EVENT_CONFIG_DEFAULT,
                    config_loaded ? "runtime config loaded"
                                  : "runtime config skipped or missing; using defaults");

  while (!gtav_menu_stop_requested()) {
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

  gtav_menu_shutdown();
  return NULL;
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
