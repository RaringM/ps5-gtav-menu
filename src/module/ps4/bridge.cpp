// PS4 platform layer for the shared menu: the small set of services the PS5 build gets from
// modules this worker does not link (status/log rings, the frame-hook installer, the custom-pack
// root and reader, profile storage). Each is either backed by the one-shot hook's own state or
// fails closed.

#include "gtavmenu/custom_assets.h"
#include "gtavmenu/custom_pack_checks.h"
#include "gtavmenu/feature_profile.h"
#include "gtavmenu/frame_hook.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu_controller.h"
#include "gtavmenu/native_bridge.h"
#include "gtavmenu/native_job_queue.h"
#include "gtavmenu/patch_broker.h"
#include "gtavmenu/ps4_slice.h"
#include "gtavmenu/ps4_stdio.h"
#include "gtavmenu/status.h"
#include "gtavmenu/target_capabilities.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The build-feature gates the capability header was generated with must match this build's flags.
#if GTAV_MENU_ENABLE_HUD != GTAV_CAPABILITY_GATE_HUD
#error "GTAV_MENU_ENABLE_HUD disagrees with the target's capability gates"
#endif
#if GTAV_MENU_ENABLE_EFFECTS != GTAV_CAPABILITY_GATE_EFFECTS
#error "GTAV_MENU_ENABLE_EFFECTS disagrees with the target's capability gates"
#endif
#if GTAV_MENU_ENABLE_VEHICLE_CLASSES != GTAV_CAPABILITY_GATE_VEHICLE_CLASSES
#error "GTAV_MENU_ENABLE_VEHICLE_CLASSES disagrees with the target's capability gates"
#endif
#if GTAV_MENU_ENABLE_CUSTOM_PACKS != GTAV_CAPABILITY_GATE_CUSTOM_PACKS
#error "GTAV_MENU_ENABLE_CUSTOM_PACKS disagrees with the target's capability gates"
#endif
#if GTAV_MENU_ENABLE_CUSTOM_PACKS && !defined(GTAV_MENU_CUSTOM_PACK_ENGINE)
#error "PS4 custom packs need an explicit engine selection"
#endif
#if GTAV_MENU_GATE_MAINTHREAD_ACTIONS != GTAV_CAPABILITY_GATE_MAIN_THREAD_ACTIONS
#error "GTAV_MENU_GATE_MAINTHREAD_ACTIONS disagrees with the target's capability gates"
#endif
#if GTAV_PS4_ENABLE_TOUCHPAD != GTAV_CAPABILITY_GATE_TOUCHPAD
#error "GTAV_PS4_ENABLE_TOUCHPAD disagrees with the target's capability gates"
#endif

#if GTAV_MENU_ENABLE_SCRIPT_GLOBALS
#include "gtavmenu/script_globals.h"
#include GTAV_PS4_TARGET_HEADER

// A globals base or index from another build is a blind write, so exact identity is a build gate.
static constexpr bool same_target(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || same_target(a + 1, b + 1));
}
static_assert(same_target(GTAV_SCRIPT_GLOBALS_ANCHOR_TARGET_ID, GTAV_PS4_TARGET_ID),
              "script-global anchors were derived for a different build");
#endif

extern "C" GtavNativeJobQueue gtav_ps4_native_jobs;

static GtavLogLevel g_ps4_log_level = GTAV_LOG_INFO;
static GtavPs4KlogSink g_klog_sink;

// Status and log rings are PS5 host-tooling ABIs; the PS4 host reads the control block instead.
extern "C" void gtav_status_event(uint32_t code, const char* message) {
  (void)code;
  (void)message;
}

extern "C" void gtav_status_eventf(uint32_t code, const char* format, ...) {
  // Pack stage/preflight breadcrumbs otherwise disappear on PS4. The PRX sink only queues a
  // bounded line; its supervisor writes it even while the game thread is inside a slow step.
  const GtavPs4KlogSink sink = __atomic_load_n(&g_klog_sink, __ATOMIC_ACQUIRE);
  if (!sink || code != GTAV_MENU_EVENT_NATIVE_SHELL || !format || strncmp(format, "pack ", 5))
    return;
  char text[240];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  sink(text);
}

extern "C" void gtav_log_set_level(GtavLogLevel level) {
  g_ps4_log_level = level;
}

extern "C" GtavLogLevel gtav_log_level(void) {
  return g_ps4_log_level;
}

extern "C" void gtav_logf_cat(GtavLogLevel level, GtavLogCategory category, const char* format,
                              ...) {
  (void)level;
  (void)category;
  (void)format;
}

// No kernel log in the freestanding worker: without a sink (the development worker) the lines are
// dropped and the host reads the control block; the PS4 plugin queues them into its log.
extern "C" void gtav_ps4_set_klog_sink(GtavPs4KlogSink sink) {
  __atomic_store_n(&g_klog_sink, sink, __ATOMIC_RELEASE);
}

extern "C" void gtav_worker_klog(const char* message) {
  const GtavPs4KlogSink sink = __atomic_load_n(&g_klog_sink, __ATOMIC_ACQUIRE);
  if (sink && message) sink(message);
}

// Frame-hook API used by the feature layer: the one-shot hook (game.cpp) drains this queue.
extern "C" int gtav_frame_hook_enqueue(uint32_t action, uint32_t param) {
  return gtav_job_queue_push(&gtav_ps4_native_jobs, action, param);
}

extern "C" int gtav_frame_hook_is_active(void) {
  const uint32_t host = __atomic_load_n(&gtav_ps4_control.host_flags, __ATOMIC_ACQUIRE);
  const uint32_t worker = __atomic_load_n(&gtav_ps4_control.worker_flags, __ATOMIC_ACQUIRE);
  return (host & GTAV_PS4_HOST_HOOK_ENABLED) != 0 && (host & GTAV_PS4_HOST_QUIESCE) == 0 &&
         (worker & GTAV_PS4_WORKER_SELF_QUIESCE) == 0;
}

// Only ever called on the game thread (feature shutdown), which is also the queue's consumer.
extern "C" void gtav_frame_hook_clear_jobs(void) {
  gtav_job_queue_init(&gtav_ps4_native_jobs);
}

extern "C" void gtav_frame_hook_set_tick_fn(GtavFrameHookTickFn tick_fn) {
  (void)tick_fn;
}

extern "C" uint32_t gtav_frame_hook_call_count(void) {
  return (uint32_t)gtav_ps4_control.hook_calls;
}

extern "C" uint32_t gtav_frame_hook_jobs_run(void) {
  return (uint32_t)gtav_ps4_control.jobs_run;
}

extern "C" uint32_t gtav_frame_hook_jobs_dropped(void) {
  return gtav_ps4_native_jobs.dropped;
}

extern "C" void gtav_frame_hook_probe_snapshot(GtavFrameHookProbeSnapshot* out) {
  if (!out) return;
  out->calls = gtav_ps4_control.hook_calls;
  out->calls_with_context = gtav_ps4_control.game_frames;
  out->last_fsbase = 0;
  out->last_real_fsbase = 0;
  out->last_script_context = 0;
  out->last_native_thread = 0;
  out->last_native_thread_vtable = 0;
  out->tls_ctx_off = 0;
}

extern "C" void gtav_menu_frame_tick(void) {
  gtav_native_bridge_frame_tick(gtav_ps4_control.game_frames, gtav_menu_controller_visible());
}

extern "C" void gtav_patch_broker_tick(uint64_t ticks, uint64_t thunk_call_count,
                                       uint64_t jobs_run) {
  (void)ticks;
  (void)thunk_call_count;
  (void)jobs_run;
}

// The custom-content root under /data, shared with the profile and the saved maps. The pack engine
// lane reads packs through the game's file device; an engine-less pack build
// (GTAV_MENU_CUSTOM_PACK_ENGINE=0) reads them through gtav_ps4_read_file.
extern "C" const char* gtav_custom_game_root(void) {
  return GTAV_CUSTOM_GAME_ROOT;
}

extern "C" void gtav_custom_game_root_init(void) {}

extern "C" int gtav_custom_game_root_available(void) {
#if GTAV_PS4_ENABLE_STORAGE
  // Save/Load Map write under /data/GTAVMenu/custom, the same tree as the profile.
  return gtav_ps4_stdio_enabled();
#else
  return 0;
#endif
}

extern "C" int64_t gtav_custom_read_file(const char* path, void* buffer, uint64_t capacity) {
  return gtav_ps4_read_file(path, buffer, capacity);
}

#if !GTAV_MENU_ENABLE_CUSTOM_PACKS
// Pack builds link the shared custom_pack_checks.c, which maps the texts.
extern "C" const char* gtav_custom_pack_user_reason(const char* technical) {
  return technical;
}
#endif

#if !GTAV_PS4_ENABLE_STORAGE
// Builds without the /data storage feature: saves fail closed and the garage/outfit slots stay
// session-only. Storage builds link the shared feature_profile.c instead.
extern "C" int gtav_feature_profile_save(const char* path, const GtavFeatureProfile* profile) {
  (void)path;
  (void)profile;
  return -1;
}

extern "C" int gtav_feature_profile_load(const char* path, GtavFeatureProfile* profile) {
  (void)path;
  (void)profile;
  return -1;
}
#endif
