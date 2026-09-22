#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main-thread execution hook/job queue. The queue can be consumed either by a
// prologue trampoline or by a native-handler table slot wrapper. When the game's
// scripts call that native, our consumer runs on the script/main thread: it drains
// the native job queue (so CREATE_VEHICLE etc. execute in a valid game-thread
// context instead of crashing from the scePad worker), then lets the native
// behave normally.
//
// The scePad worker is the single producer (enqueue); the hook/wrapper is the
// single consumer (drain). Everything is a no-op until one prepare/install call
// succeeds.

// Per-job callback invoked on the main thread for each drained job.
typedef void (*GtavFrameHookJobFn)(uint32_t action, uint32_t param, void* context);

// Per-frame effect tick invoked on the script/main thread EVERY time the hook fires
// (unlike the job queue, which only drains when something is enqueued). It runs in the
// same valid script context as the job drain, under the same single-drain guard, so a
// continuous game-thread effect (e.g. explosive ammo: read the last weapon impact, then
// ADD_OWNED_EXPLOSION there) can run safely. Optional; null = no per-frame effect.
typedef void (*GtavFrameHookTickFn)(void);

typedef struct GtavFrameHookProbeSnapshot {
  uint64_t calls;
  uint64_t calls_with_context;
  uint64_t last_fsbase;
  uint64_t last_real_fsbase;
  uint64_t last_script_context;
  uint64_t last_native_thread;
  uint64_t last_native_thread_vtable;
  // Magnitude of the runtime-calibrated game-context TLS offset (fs - off), 0 until the
  // one-shot calibrate discovers it. Read back by the worker for the persistent log breadcrumb.
  uint64_t tls_ctx_off;
} GtavFrameHookProbeSnapshot;

// Install result codes. 0 on success, or one of these negatives on failure.
// gtav_frame_hook_install reports the full set; the other install entry points return a
// subset (mainly ALREADY_ACTIVE). Callers may simply treat any negative as "not installed".
enum {
  GTAV_FRAME_HOOK_INSTALL_OK = 0,
  GTAV_FRAME_HOOK_INSTALL_ERR_ALREADY_ACTIVE = -1,   // a hook is already installed
  GTAV_FRAME_HOOK_INSTALL_ERR_NULL_TARGET = -2,      // target_handler was 0
  GTAV_FRAME_HOOK_INSTALL_ERR_UNSAFE_PROLOGUE = -3,  // prologue not trampoline-safe
  GTAV_FRAME_HOOK_INSTALL_ERR_DETOUR_FAILED = -4,    // detour/patch write failed
};

// Install the trampoline on target_handler. The prologue is read and validated
// against the detour decoder; install fails if it is not trampoline-safe. job_fn runs
// on the main thread for each drained job. Returns GTAV_FRAME_HOOK_INSTALL_OK (0) on
// success or a GTAV_FRAME_HOOK_INSTALL_ERR_* negative on failure. max_jobs_per_frame
// bounds per-call drain work (0 = a built-in default) so a job flood cannot stall the
// game thread.
int gtav_frame_hook_install(uintptr_t target_handler, GtavFrameHookJobFn job_fn, void* job_context,
                            uint32_t max_jobs_per_frame);

// In-process REPLACE install (experimental prologue-replace model). Overwrites the
// target handler's first 14 bytes with an FF25 absolute jump to a replace thunk
// that drains the job queue on the script/main thread and returns a passthrough
// value -- NO gateway, NO chain back to the original. The current prologue is
// validated against gtav_frame_hook_expected_bytes() before patching. This remains
// useful for host tests and legacy artifacts, but the PS5 DOES_CAM_EXIST prologue
// replace path was live-confirmed unsafe on 2026-06-14; prefer the native-handler
// table slot wrapper for vehicle-spawn work.
int gtav_frame_hook_install_replace(uintptr_t target_handler, GtavFrameHookJobFn job_fn,
                                    void* job_context, uint32_t max_jobs_per_frame);

// Remove the trampoline (restores the original prologue). Safe to call when not
// installed. Returns 0 on success.
int gtav_frame_hook_restore(void);

// External-install mode: the payload sets up the queue + thunk + a compile-time
// gateway stub but does NOT patch game .text. Instead an external agent can write
// the jump to the target. After this returns 0, gtav_frame_hook_thunk_address()
// gives the runtime address the external agent must jump the target to. This only
// prepares state; target safety still depends on the selected hook site.
int gtav_frame_hook_install_external(GtavFrameHookJobFn job_fn, void* job_context,
                                     uint32_t max_jobs_per_frame);

// Prepare only the job queue/consumer state. Used by native-handler slot wrappers
// that are installed elsewhere and call gtav_frame_hook_run_pending_jobs() from a
// normal handler-call context, avoiding executable prologue patching entirely.
int gtav_frame_hook_prepare_jobs(GtavFrameHookJobFn job_fn, void* job_context,
                                 uint32_t max_jobs_per_frame);

// Runtime address of the thunk (external agent jumps the hooked native here), or
// 0 if external mode is not compiled in. Stable for the life of the payload.
uintptr_t gtav_frame_hook_thunk_address(void);

// External-install metadata published for a broker that kernel-writes the hook.
uintptr_t gtav_frame_hook_gateway_address(void);
uintptr_t gtav_frame_hook_gateway_continuation(void);
uintptr_t gtav_frame_hook_gateway_continuation_slot(void);
uint32_t gtav_frame_hook_patch_len(void);
uint32_t gtav_frame_hook_stolen_len(void);
const uint8_t* gtav_frame_hook_expected_bytes(uint32_t* len_out);

// Non-zero once a trampoline is installed and the thunk is live.
int gtav_frame_hook_is_active(void);

// Register (or clear, with null) the per-frame effect tick. Independent of which
// install mode is active; takes effect immediately. The tick runs on the script/main
// thread inside gtav_frame_hook_run_pending_jobs(). Safe to call before or after install.
void gtav_frame_hook_set_tick_fn(GtavFrameHookTickFn tick_fn);

// Producer side (scePad worker): queue a job for the main-thread thunk. Returns
// 1 if enqueued, 0 if the hook is inactive or the queue is full.
int gtav_frame_hook_enqueue(uint32_t action, uint32_t param);

// Clear queued jobs and unregister the per-frame tick. Used before Stop Runtime
// tears hooks down so no feature work runs after shutdown starts.
void gtav_frame_hook_clear_jobs(void);

// Consumer side: run from a script/main-thread hook or handler-slot wrapper.
// Increments call telemetry every time it is reached and drains up to the
// configured per-call job budget. Returns the number of jobs executed.
uint32_t gtav_frame_hook_run_pending_jobs(void);

// Telemetry for the live fire-rate check: how many times the thunk has run, and
// how many jobs it has executed. Used to confirm the hooked native is actually
// called every frame before relying on it.
uint32_t gtav_frame_hook_call_count(void);
uint32_t gtav_frame_hook_jobs_run(void);
uint32_t gtav_frame_hook_jobs_dropped(void);
void gtav_frame_hook_probe_snapshot(GtavFrameHookProbeSnapshot* out);

// Callback/job-only: the CGameScriptHandler of the ACCEPTED outer invocation, or 0 when it
// could not be resolved. Rejected nested/cross-thread probes cannot overwrite it. Cleared on
// callback exit; this is not a worker-side last-fire diagnostic or a transferable permission.
//
// This is the pointer GTA's own model-streaming natives resolve to decide which script owns a
// streaming request, so a game-thread tick or a drained job can use it to tell "this fire belongs
// to a long-lived script" from "this fire belongs to a transient one". See
// include/gtavmenu/script_handler_census.h. Needs GTAV_FRAME_HOOK_CAPTURE_SCRIPT_HANDLER.
uint64_t gtav_frame_hook_current_script_handler(void);

#ifdef __cplusplus
}
#endif
