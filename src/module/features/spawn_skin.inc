// Vehicle spawn + ped skin (model streaming, hash jobs, preload/prewarm)
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

static int wait_for_model(uint32_t model, int allow_block) {
  // Request the model and poll until streamed. allow_block=1 (worker/preload
  // context) spins up to ~1.5s; allow_block=0 (game-thread frame-hook context)
  // must NOT block -- a 1.5s spin inside the per-frame hook would stall the
  // script thread and freeze the game, so it does a single request + check. The
  // spawn-hash job requeues itself while the model is pending.
  // allow_block doubles as the worker-lane flag (see invoke_*_ctx): worker can block.
  const int worker = allow_block;
  invoke_void_ctx(worker, g_n.request_model, model);
  if (!allow_block) {
    return invoke_return_ctx<int>(worker, g_n.has_model_loaded, model);
  }
  for (int i = 0; i < 150; ++i) {
    if (invoke_return_ctx<int>(worker, g_n.has_model_loaded, model)) return 1;
    usleep(10000);
    invoke_void_ctx(worker, g_n.request_model, model);
  }
  return invoke_return_ctx<int>(worker, g_n.has_model_loaded, model);
}

static float f32_from_bits(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t direct_spawn_result(uint32_t result, const char* message) {
  g_state.last_action = GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE;
  g_state.last_result = result;
  set_message(message);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
  return result;
}

// --- Model pre-warm (game-thread) ---------------------------------------------------------
//
// Warming a model means issuing REQUEST_MODEL so the streamer loads it BEFORE a spawn/skin is
// confirmed -- then the drained spawn job finds HAS_MODEL_LOADED already true and reaches
// CREATE_VEHICLE on its FIRST game-thread fire instead of re-queueing for seconds while the
// model streams. REQUEST_MODEL / IS_MODEL_VALID are NOT worker-safe (hardware-confirmed
// 2026-06-17: issuing them from the scePad worker faulted GTA to the dashboard), so the warm
// runs on the GAME thread as a lightweight PREWARM_MODEL job drained by the frame hook -- the
// exact lane the spawn/skin jobs already use. The worker only records the desired model and,
// edge-triggered, enqueues the job; every streaming-native call happens in
// run_prewarm_model_job() below.

// Pre-warm progress marker (diagnostics), written on the game thread: 0 idle, 1 streaming
// (requeued), 2 resident (ready), 3 gave up (deadline/queue full). Exported like
// gtav_spawn_stage so a status/crash read can see the warmer's state.
extern "C" {
volatile uint32_t gtav_prewarm_stage = 0;
}

// The model the worker wants kept warm right now (0 = none). Written on the scePad worker
// (gtav_features_prewarm_enqueue), read on the game thread (run_prewarm_model_job), so the job
// always converges to the LATEST request and any stale queued job is harmless. volatile for
// cross-thread visibility -- a plain 32-bit store/load, no dependent data rides it.
static volatile uint32_t g_prewarm_target_model = 0;

#if GTAV_MENU_ENABLE_VEHICLE_PRELOAD && GTAV_MENU_ENABLE_FRAME_HOOK
// Last model the worker enqueued a PREWARM job for, so a steady hover (the worker tick fires
// ~90 Hz) enqueues ONCE on change instead of flooding the 32-slot queue every tick -- the job
// self-requeues while a model is still streaming, so one enqueue per change suffices.
static uint32_t g_prewarm_last_enqueued = 0;
#endif

// Worker-safe: record the model to keep warm and, on change, kick a game-thread PREWARM job.
// 0 means "stop warming" (cursor left the spawn rows). NEVER calls a streaming native on the
// worker -- it only updates state and pushes to the SPSC job queue (the lane spawns already
// use). No-op unless built with vehicle preload + the frame hook.
static void prewarm_enqueue(uint32_t model) {
#if GTAV_MENU_ENABLE_VEHICLE_PRELOAD && GTAV_MENU_ENABLE_FRAME_HOOK
  g_prewarm_target_model = model;
  if (model == g_prewarm_last_enqueued) return;  // edge-trigger: enqueue only on change
  // Advance the edge-trigger marker ONLY when the job is actually queued. If the hook is not
  // live yet (the install write lands shortly after init) or the queue is momentarily full,
  // leave it unchanged so the next worker tick retries -- otherwise the model would be marked
  // "enqueued" without a job ever running and never get warmed until the cursor moved away.
  if (gtav_frame_hook_is_active() &&
      gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL, model)) {
    g_prewarm_last_enqueued = model;
  }
#else
  (void)model;
#endif
}

// Non-blocking preload of the vehicle model under the menu cursor. Called every worker tick
// with the highlighted vehicle's model hash, or 0 when the cursor is not on a spawn row (or
// the menu is hidden), so the previously-warmed model is released and scrolling the browser
// does not pin every model. Worker-safe: it only records the target and enqueues a game-thread
// PREWARM job -- the REQUEST_MODEL itself runs on the game thread. The actual CREATE_VEHICLE
// still runs from the drained spawn job (which re-requests the model itself), so the preload
// is a pure accelerator: a failed/cold preload never blocks or breaks a spawn.
extern "C" void gtav_features_preload_highlighted_vehicle(uint32_t model) { prewarm_enqueue(model); }

// One-shot pre-warm issued at the moment a spawn/skin is QUEUED, so an instant select (no
// hover dwell) still starts model streaming at queue time. Shares prewarm_enqueue's single
// warming slot with the hover preload (one game-thread REQUEST_MODEL owner / one release).
extern "C" void gtav_features_prewarm_model_hash(uint32_t model) {
  if (model) prewarm_enqueue(model);
}

// Spawn progress marker, written on the game/script thread as the drained spawn job
// advances through GTA's vehicle natives. Exported (external linkage) so the loader /
// ps5debug can read it after a game-thread fault to pinpoint WHICH native died -- such
// a fault is otherwise opaque. 0 = idle/clean. Stages:
//   1 entered  2 model-ready  3 have-ped  4 have-coords  5 create-vehicle  6 configure  7 done
// A crash read of 5 => died in CREATE_VEHICLE; 3 => died in GET_ENTITY_COORDS;
// 6 => died configuring the spawned vehicle. 7 (or 0 after) => spawn completed.
extern "C" {
volatile uint32_t gtav_spawn_stage = 0;
}

// Weapon progress marker, same idea as gtav_spawn_stage: written on the game/script
// thread as give_weapons()/remove_weapons() walk GTA's weapon natives, so a fault is
// localizable from a crash read instead of being opaque. 0 = idle/clean.
//   give_weapons:   1 entered  2 ped-valid  100+i about-to-give catalog weapon i
//   remove_weapons: 10 entered 11 ped-valid 12 about-to-remove-all
// A crash read of 100+i => died in GIVE_WEAPON_TO_PED for weapon index i;
// 12 => died in REMOVE_ALL_PED_WEAPONS. 0 after => completed cleanly.
extern "C" {
volatile uint32_t gtav_weapon_stage = 0;
}

// Skin progress marker, same idea as gtav_spawn_stage/gtav_weapon_stage: written on the
// game/script thread as gtav_features_run_skin_hash_job() walks GTA's model + player natives,
// so a fault (SET_PLAYER_MODEL recreates the player ped -- the most invasive action) is
// localizable from a crash read instead of being opaque. 0 = idle/clean.
//   1 entered (streaming/validate)  2 model resident  3 about-to SET_PLAYER_MODEL
//   4 about-to SET_PED_DEFAULT_COMPONENT_VARIATION  5 about-to SET_MODEL_AS_NO_LONGER_NEEDED
// A crash read of 3 => died in SET_PLAYER_MODEL; 4 => component variation. 0 after => clean.
extern "C" {
volatile uint32_t gtav_skin_stage = 0;
}

static uint32_t spawn_vehicle_model_hash(uint32_t model, int allow_block) {
  gtav_spawn_stage = 1;
  g_state.last_param = model;
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  // The global prevents shop_controller from ejecting the player from MP/DLC
  // vehicles. Demand-arm it here as well as at menu activation so mailbox/direct
  // job callers cannot bypass the contract. The state machine is bounded and this
  // call runs on the verified game thread. If it cannot prove + read back the flag,
  // fail before CREATE_VEHICLE instead of spawning a car that will be reclaimed.
  GtavScriptGlobalsStatus script_globals{};
  gtav_script_globals_arm();
  // IDLE selects DIRECT/DERIVE on the first step and performs the bounded
  // validation/write on the second. Usually the per-frame tick has already done
  // both while this spawn job waited for its next-frame/model gate.
  for (int step = 0; step < 2 && script_globals.state != GTAV_SCRIPT_GLOBALS_DONE &&
                     script_globals.state != GTAV_SCRIPT_GLOBALS_FAILED;
       ++step) {
    gtav_script_globals_tick(&script_globals);
  }
  if (script_globals.state != GTAV_SCRIPT_GLOBALS_DONE) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_UNAVAILABLE,
                               "vehicle session guard not ready; retry");
  }
#endif
  if (!g_n.create_vehicle || !g_n.request_model || !g_n.has_model_loaded || !g_n.player_ped_id ||
      !g_n.get_entity_coords) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_UNAVAILABLE, "missing vehicle spawn natives");
  }
  if (g_n.is_model_valid && !invoke_return<int>(g_n.is_model_valid, model)) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not valid");
  }
  if (g_n.is_model_in_cdimage && !invoke_return<int>(g_n.is_model_in_cdimage, model)) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not in cdimage");
  }
  if (!wait_for_model(model, allow_block)) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not loaded");
  }
  gtav_spawn_stage = 2;

  int ped = native_player_ped_id();
  // Entity-creating natives need a live player ped (and thus an active game world).
  // Calling GET_ENTITY_COORDS / CREATE_VEHICLE with an invalid ped -- e.g. on a loading
  // screen, in a cutscene, or before the world is up -- faults the game thread. Bail
  // gracefully so the menu reports it instead of crashing GTA.
  if (ped <= 0) {
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_FAILED,
                               "no valid player ped (be in active gameplay)");
  }
  gtav_spawn_stage = 3;
  Vec3 pos = invoke_vector(g_n.get_entity_coords, ped, 1);
  float sx = pos.x + f32_from_bits(0x40a00000u);  // 5.0f
  float sy = pos.y;
  float sz = pos.z + f32_from_bits(0x3f800000u);  // 1.0f
  float heading = f32_from_bits(0x00000000u);
  gtav_spawn_stage = 4;

  const int worker = allow_block;
  gtav_spawn_stage = 5;
  int vehicle =
      invoke_return_ctx<int>(worker, g_n.create_vehicle, model, sx, sy, sz, heading, 0, 0);
  if (!vehicle) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
    gtav_spawn_stage = 0;
    return direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "create vehicle failed");
  }
  gtav_spawn_stage = 6;

  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(worker, g_n.set_entity_as_mission_entity, vehicle, 1, 1);
  if (g_n.set_vehicle_on_ground_properly)
    invoke_void_ctx(worker, g_n.set_vehicle_on_ground_properly, vehicle);
  if (g_n.set_vehicle_engine_on)
    invoke_void_ctx(worker, g_n.set_vehicle_engine_on, vehicle, 1, 1, 0);
  if (g_n.set_ped_into_vehicle) invoke_void_ctx(worker, g_n.set_ped_into_vehicle, ped, vehicle, -1);
  // Re-stamp mission-entity ownership AFTER seating. MP-only models (e.g. Apocalypse Bruiser)
  // were getting reclaimed by the population/streaming manager shortly after spawn, ejecting
  // the player; claiming the vehicle as a mission entity again once the ped is in it makes the
  // game keep it. (set_entity_as_mission_entity(entity, isMission=1, grabFromOtherScript=1).)
  // NOTE: needs on-hardware iteration -- some MP models may still eject via game scripts; the
  // fuller fix (SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER / population suppression) needs new natives.
  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(worker, g_n.set_entity_as_mission_entity, vehicle, 1, 1);
  // Spawner defaults: Spawn Maxed / Spawn Invincible. Applied on this same game-thread spawn
  // context so the mods/invincibility land on the new handle before the model is released; a
  // no-op when both toggles are off.
  apply_spawn_vehicle_defaults(worker, vehicle);
  // Release the model LAST, after the vehicle exists, is owned, and the ped is seated, so the
  // streamer never frees it out from under the seat assignment.
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);

  g_last_spawned_vehicle = vehicle;
  track_spawned_vehicle(vehicle);
  gtav_spawn_stage = 7;
  return direct_spawn_result(GTAV_FEATURE_RESULT_OK, "spawned vehicle");
}

// Game-thread model-streaming wait deadline (seconds). The spawn/skin jobs re-run once
// per hook fire and self-requeue while a model streams. A fire-counted retry budget is
// wrong: with a fast per-frame pump (e.g. the PLAYER_PED_ID lane, ~6000 fires/s) it burns
// out in milliseconds -- long before a model can stream in -- so the spawn gives up and
// nothing appears, while a slow input-gated pump spans many seconds for the same count.
// Gate the give-up on wall-clock seconds instead, making completion independent of the
// pump's fire rate. time() is 1s-coarse, which is fine: success fires the instant
// HAS_MODEL_LOADED is true; this only bounds when we stop waiting. IS_MODEL_VALID /
// IS_MODEL_IN_CDIMAGE are checked first, so only valid (fast-streaming) models reach the
// wait -- the full window is rarely hit. Tunable via -DGTAV_MODEL_STREAM_WAIT_SECONDS.
#ifndef GTAV_MODEL_STREAM_WAIT_SECONDS
#define GTAV_MODEL_STREAM_WAIT_SECONDS 8
#endif

// Hard ceiling on real (frame-advanced) streaming attempts for one in-flight request,
// independent of the wall clock. The wall-clock budget alone is NOT sufficient: its
// deadline lives in a single static per job family while the hook's queue can hold jobs
// for DIFFERENT models at once, so two spawns in flight each reset the other's deadline on
// every execution and the budget never expires. Live 2026-09-17: ~606k requeues against an
// 8s budget, which saturated the game thread and hung GTA. This cap cannot be reset by an
// interleaved request -- only a terminal status clears it -- so total work stays bounded
// however the queue interleaves. ~2x the frames in the wall-clock budget at 60fps.
#ifndef GTAV_MODEL_STREAM_MAX_ATTEMPTS
#define GTAV_MODEL_STREAM_MAX_ATTEMPTS 960u
#endif

// Shared game-thread model-streaming step for the spawn jobs (vehicle / ped / object /
// bodyguard). The caller has already recorded *pending = model + a deadline and checked
// readiness/validity; this issues REQUEST_MODEL once and reports whether the model is resident,
// still streaming (self-requeued onto the frame hook), or terminally failed -- centralizing the
// frame-hook requeue + deadline/queue-full handling that every job otherwise copy-pastes. It
// clears *pending on every terminal status except LOADED, where the caller clears it after the
// spawn succeeds. The caller maps the returned status onto its own result reporter + message.
enum ModelStreamStatus {
  MODEL_STREAM_LOADED,      // resident -> caller spawns now
  MODEL_STREAM_PENDING,     // still streaming, requeued onto the frame hook
  MODEL_STREAM_QUEUE_FULL,  // frame-hook job queue full
  MODEL_STREAM_NO_HOOK,     // no frame hook in this build
  MODEL_STREAM_TIMEOUT,      // streaming wall-clock budget exhausted
  MODEL_STREAM_NOT_RESIDENT,  // cold model, and issuing the request would poison the streamer
};

// Per-job-family streaming state.
//
// Every field here used to be a function-local `static` shared by ALL the streaming families
// (vehicle / ped / object / bodyguard / prewarm), which was wrong in two ways that both showed up
// live:
//   * `attempts` -- a terminal status in one family zeroed another family's accumulated budget, so
//     GTAV_MODEL_STREAM_MAX_ATTEMPTS was not per-request as its comment claims.
//   * `last_frame` -- the frame claim was global, so with two streaming jobs in flight the first
//     to run each frame consumed the frame and the other starved. They alternated frames at best.
// Giving each family its own instance makes the budget and the frame claim mean what they say.
typedef struct ModelStreamJob {
  uint32_t pending;     // model currently in flight for this family (0 = idle)
  time_t deadline;      // wall-clock give-up for `pending`
  uint32_t attempts;    // frame-advanced attempts spent on `pending`
  uint32_t last_frame;   // frame this family last claimed
  uint32_t reported;     // model the "pending" status has already been reported for
  uint32_t owner_waits;  // consecutive fires declined by the owner gate
} ModelStreamJob;

// ---------------------------------------------------------------------------------------------
// Script-resource owner census (read-only).
//
// See include/gtavmenu/script_handler_census.h for the mechanism. In short: the streaming natives
// attribute a model to the script that called them, and our hook chains a native every script
// calls, so an ungated request lands on a transient script and is reclaimed. The census watches
// which CGameScriptHandler owns each fire so a streaming job can wait for a long-lived one.
//
// Compiled out entirely unless the census gate is on; `model_stream_owner_ok()` then returns 1 so
// the lane behaves exactly as it does today.
// ---------------------------------------------------------------------------------------------

// Minimum frames a candidate must have been alive before we trust it with a streaming request.
// Until the census has this much history every fire is declined and the job simply requeues, so a
// spawn issued in the first ~2s after inject waits that long.
#ifndef GTAV_MODEL_STREAM_OWNER_MIN_AGE_FRAMES
#define GTAV_MODEL_STREAM_OWNER_MIN_AGE_FRAMES 120u
#endif

// How stale the dominant slot may be and still be trusted. A CGameScriptHandler is a heap
// pointer: a dead script's handler can be freed and a different script allocated at the same
// address, so a slot we have not seen recently must not be trusted however old it looks.
#ifndef GTAV_MODEL_STREAM_OWNER_MAX_STALE_FRAMES
#define GTAV_MODEL_STREAM_OWNER_MAX_STALE_FRAMES 8u
#endif

// Hard ceiling on consecutive fires the owner gate may decline for ONE request.
//
// Without a ceiling the owner gate is an unbounded loop: the wall-clock deadline and the attempt
// budget are both checked inside model_stream_step, which a declined fire never reaches, so a
// request whose owner never becomes dominant would requeue at the hook's full rate forever with
// gtav_spawn_stage stuck at 0 and no error ever reported. It calls no native, so it cannot hang
// the game the way the earlier ungated loops did -- but it would spin silently, which is worse to
// diagnose. On expiry the job falls through to the normal path: it issues one request on whatever
// fire it has (pre-fix behaviour), the wall-clock deadline is already long past, and
// model_stream_step reports "model not loaded". A fire count rather than time() because this is
// evaluated up to ~8235 times a second. ~24s at the measured fire rate.
#ifndef GTAV_MODEL_STREAM_OWNER_MAX_WAIT_FIRES
#define GTAV_MODEL_STREAM_OWNER_MAX_WAIT_FIRES 200000u
#endif

#if GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_CENSUS && GTAV_MENU_ENABLE_FRAME_HOOK
static GtavScriptHandlerCensus g_sh_census;

// Published for research/tools/live/peek_frame_hook.py. Exported (external linkage) like gtav_spawn_stage so
// --gc-sections cannot drop them and llvm-nm can find them in the mapped module.
extern "C" {
volatile uint64_t gtav_sh_fires = 0;
volatile uint64_t gtav_sh_fires_no_handler = 0;
volatile uint32_t gtav_sh_distinct = 0;
volatile uint32_t gtav_sh_overflow = 0;
volatile uint32_t gtav_sh_frames_observed = 0;
volatile uint64_t gtav_sh_dominant = 0;
volatile uint64_t gtav_sh_dominant_fires = 0;
volatile uint32_t gtav_sh_dominant_frames = 0;
volatile uint32_t gtav_sh_dominant_age = 0;
volatile uint32_t gtav_sh_coverage_pct = 0;
}

static void model_stream_census_publish(void) {
  gtav_sh_fires = g_sh_census.fires;
  gtav_sh_fires_no_handler = g_sh_census.fires_no_handler;
  gtav_sh_distinct = g_sh_census.used;
  gtav_sh_overflow = g_sh_census.overflow;
  gtav_sh_frames_observed = g_sh_census.frames_observed;
  gtav_sh_coverage_pct = gtav_sh_census_coverage_pct(&g_sh_census);
  const int slot = gtav_sh_census_dominant_slot(&g_sh_census);
  if (slot < 0) {
    gtav_sh_dominant = 0;
    gtav_sh_dominant_fires = 0;
    gtav_sh_dominant_frames = 0;
    gtav_sh_dominant_age = 0;
    return;
  }
  gtav_sh_dominant = g_sh_census.slots[slot].handler;
  gtav_sh_dominant_fires = g_sh_census.slots[slot].fires;
  gtav_sh_dominant_frames = g_sh_census.slots[slot].frames_present;
  gtav_sh_dominant_age = g_sh_census.slots[slot].last_frame - g_sh_census.slots[slot].first_frame;
}
#endif

// Observe this hook fire. Called from gtav_features_game_thread_tick(), which the frame hook runs
// on EVERY fire, in the same script context and inside the same drain guard as the jobs -- so the
// handler the census records and the handler a job reads are the same fire's, by construction.
// [[maybe_unused]]: the only caller is gtav_features_game_thread_tick(), which lives inside
// weapon_fx.inc's GTAV_MENU_ENABLE_FRAME_HOOK block, so a no-frame-hook build has no caller.
[[maybe_unused]] static void model_stream_census_tick(void) {
#if GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_CENSUS && GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.get_frame_count) return;  // nothing to bucket fires into
  gtav_sh_census_observe(&g_sh_census, gtav_frame_hook_current_script_handler(),
                         invoke_return<uint32_t>(g_n.get_frame_count));
  model_stream_census_publish();
#endif
}

#if GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_FILTER && GTAV_MENU_ENABLE_FRAME_HOOK
extern "C" {
volatile uint64_t gtav_ms_owner_skips = 0;
}
#endif

// Is THIS hook fire attributable to a long-lived script?
//
// MUST be checked BEFORE model_stream_defer_to_next_frame's frame claim, and that ordering is the
// whole design. The claim is check-and-set: the first execution in a new frame takes the frame and
// every other execution that frame defers. The drain gives a pending job ~137 executions per frame
// (one per fire), so declining a fire costs essentially nothing -- but only if declining it does
// NOT consume the frame. Check the owner after the claim and the success condition collapses from
// "does the dominant script fire at least once this frame" (~1.0) to "is it the FIRST caller this
// frame" (~1/137), which would issue ~3 requests across the whole 8s budget and read as a false
// negative on the ownership hypothesis. tests/test_features_static.py asserts the source order.
static int model_stream_owner_ok(void) {
#if GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_FILTER && GTAV_MENU_ENABLE_FRAME_HOOK
  const uint64_t handler = gtav_frame_hook_current_script_handler();
  if (gtav_sh_census_is_dominant(&g_sh_census, handler,
                                 GTAV_MODEL_STREAM_OWNER_MIN_AGE_FRAMES,
                                 GTAV_MODEL_STREAM_OWNER_MAX_STALE_FRAMES)) {
    return 1;
  }
  gtav_ms_owner_skips = gtav_ms_owner_skips + 1;
  return 0;
#else
  return 1;  // filter compiled out: every fire is eligible (pre-existing behaviour)
#endif
}

// Has the game rendered a new frame since this family's last streaming step?
//
// The frame hook CHAINS a native the scripts call many times per rendered frame -- on the
// PLAYER_PED_ID lane that measured ~8235 fires/sec, i.e. ~137 per frame. The streamer,
// however, makes progress per FRAME. Re-issuing REQUEST_MODEL on every fire therefore does
// no extra good and actively harms: it re-registers the same request ~137x per frame and
// floods the streaming system, which is how a single spawn ran up 2.7M drained jobs and
// left the model permanently "pending" (live 2026-09-17).
//
// Returning 0 means "defer": the caller requeues without touching a native, so the job
// stays alive and its wall-clock deadline keeps running, but the streamer is touched at
// most once per frame. Mirrors reassert_toggles_game_thread()'s GET_FRAME_COUNT gate.
// Without the counter resolved there is nothing to gate on, so fall back to per-fire.
static int model_stream_frame_advanced(ModelStreamJob* job) {
  if (!g_n.get_frame_count) return 1;
  const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
  if (frame == job->last_frame) return 0;
  job->last_frame = frame;
  return 1;
}

// Requeue this streaming job unchanged, doing no native work. Shared by the owner gate, the
// frame-gate defer path and the "still streaming" tail below.
static ModelStreamStatus model_stream_requeue([[maybe_unused]] uint32_t action,
                                              [[maybe_unused]] uint32_t model,
                                              ModelStreamJob* job) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (gtav_frame_hook_enqueue(action, model)) {
    return MODEL_STREAM_PENDING;
  }
  job->pending = 0;
  return MODEL_STREAM_QUEUE_FULL;
#else
  job->pending = 0;
  return MODEL_STREAM_NO_HOOK;
#endif
}

// Adopt `model` as this family's in-flight request if it is new, (re)arming the wall-clock budget.
// Split out of the job bodies so the deadline/attempt reset happens in exactly one place.
static void model_stream_adopt(uint32_t model, ModelStreamJob* job) {
  if (job->pending == model) return;
  job->pending = model;
  job->deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
  job->attempts = 0;
  job->reported = 0;
  job->owner_waits = 0;
}

// Should this streaming job stand down until the game renders another frame -- or until a fire
// owned by a long-lived script comes along?
//
// Call this EXACTLY ONCE per job execution, at the very top of the body, before any native. It is
// check-and-set on the frame: the first call in a new frame claims it and returns 0 (proceed),
// later calls return 1 after silently requeueing. Calling it twice in one execution is a bug --
// the first call claims the frame, the second then defers unconditionally, so the job requeues
// forever (live 2026-09-17: jobs_run climbing ~16k/sec with gtav_spawn_stage stuck at 0).
//
// Deferred fires are deliberately silent: at ~137 fires per frame per job, reporting them would
// push a status event per fire, evicting the 16-slot event ring in milliseconds and hiding the
// result an operator is waiting for.
//
// Gating the WHOLE body (not just the REQUEST_MODEL call) is required, not merely tidy: the
// terminal SET_MODEL_AS_NO_LONGER_NEEDED releases the model from the CALLING script's handler too
// (eboot 0x1c09760: same fs:[0] -> -0x140 -> +0x198 chain, vtable slot 15). If requests were
// attributed to a long-lived script while releases kept landing on random transient ones, every
// model we streamed would be pinned on that script forever.
static int model_stream_defer_to_next_frame(uint32_t action, uint32_t model, ModelStreamJob* job) {
  // Adopt FIRST so the wall-clock budget is armed before anything can defer. The owner gate below
  // can decline for many seconds, and a request that had not armed its deadline yet would have
  // nothing to expire against.
  model_stream_adopt(model, job);
  if (!model_stream_owner_ok()) {
    if (++job->owner_waits < GTAV_MODEL_STREAM_OWNER_MAX_WAIT_FIRES) {
      (void)model_stream_requeue(action, model, job);  // wrong owner: do NOT consume the frame
      return 1;
    }
    // Waited too long for an owner: stop declining and let the body run, so the request is issued
    // and the normal deadline path reports a failure instead of spinning silently.
    job->owner_waits = 0;
  }
  if (model_stream_frame_advanced(job)) return 0;
  (void)model_stream_requeue(action, model, job);
  return 1;
}

// Should the caller report "model pending" for this execution?
//
// Edge-triggered: once per in-flight model, not once per frame. The frame gate already cut the
// rate from ~8235/s to ~60/s, but the status ring is 16 slots, so even 60 writes/sec evicts
// everything else ~4x a second and hides exactly the result the operator is waiting for -- and
// each write is a string copy inside the drain's critical section.
static int model_stream_should_report_pending(ModelStreamJob* job) {
  if (job->reported == job->pending) return 0;
  job->reported = job->pending;
  return 1;
}

// Which native issues the streaming request.
//
// REQUEST_MENU_PED_MODEL (eboot 0x1c09560, hash-confirmed 0xA0261AEF7ACFC51E) is REQUEST_MODEL
// (0x1c09550) plus `or [pending streaming flags], 6` before the identical shared helper, i.e. the
// same request with two extra streaming flags on top of the helper's own 0x10. If those flags are
// what actually kicks a load, a cold model streams without any ownership work at all -- so this is
// a one-line A/B against the owner filter. Default off; falls back when unresolved.
static uint64_t model_stream_request_native(void) {
#if GTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST
  if (g_n.request_menu_ped_model) return g_n.request_menu_ped_model;
#endif
  return g_n.request_model;
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
extern "C" {
volatile uint32_t gtav_ms_requests_issued = 0;
}
#endif
#if GTAV_MENU_ENABLE_STREAM_FORCE_LOAD
extern "C" {
volatile uint32_t gtav_ms_force_loads = 0;
}
#endif

static ModelStreamStatus model_stream_step(uint32_t action, uint32_t model, ModelStreamJob* job) {
#if GTAV_MENU_STREAM_RESIDENT_ONLY
  // Refuse to request a model that is not ALREADY resident.
  //
  // Cold streaming has never worked on either build -- a requested-but-not-resident model sits at
  // strStreamingInfo status 2 indefinitely -- so this removes no working functionality. It is also
  // correlated with a harmful state: the ring consumer in the pump 0x35633f0 stops at the first
  // slot whose completion flag ([slot + 0x1848]) is clear, so one never-completing read can freeze
  // the ring `tail` and block completed reads behind it, and a stuck ring was directly observed.
  //
  // Whether OUR request CAUSES that jam is NOT established -- a process whose injection failed and
  // rolled back (no worker, no hook, no spawn) reached the same saturated ring state. Either way
  // this guard is the conservative choice. See docs/ptrace-free-injection.md.
  if (!invoke_return_ctx<int>(0, g_n.has_model_loaded, model)) {
    job->attempts = 0;
    job->pending = 0;
    return MODEL_STREAM_NOT_RESIDENT;
  }
#endif
  invoke_void_ctx(0, model_stream_request_native(), model);
#if GTAV_MENU_ENABLE_FRAME_HOOK
  gtav_ms_requests_issued = gtav_ms_requests_issued + 1;
#endif
  if (invoke_return_ctx<int>(0, g_n.has_model_loaded, model)) {
    job->attempts = 0;
    return MODEL_STREAM_LOADED;
  }
#if GTAV_MENU_ENABLE_STREAM_FORCE_LOAD
  // The request is recorded but unserviced -- see GTAV_MENU_ENABLE_STREAM_FORCE_LOAD. Flush the
  // streamer synchronously and re-check; a completed flush makes this the ONLY extra work, because
  // returning LOADED ends the requeue loop immediately. Runs at most once per frame (the caller's
  // frame gate), and only while a model is genuinely pending.
  if (g_n.load_all_objects_now) {
    invoke_void_ctx(0, g_n.load_all_objects_now);
    gtav_ms_force_loads = gtav_ms_force_loads + 1;
    if (invoke_return_ctx<int>(0, g_n.has_model_loaded, model)) {
      job->attempts = 0;
      return MODEL_STREAM_LOADED;
    }
  }
#endif
  // Counts only frame-advanced attempts, and is cleared solely by a terminal status here or by
  // model_stream_adopt() taking a new model -- see GTAV_MODEL_STREAM_MAX_ATTEMPTS.
  if (time(NULL) >= job->deadline || ++job->attempts >= GTAV_MODEL_STREAM_MAX_ATTEMPTS) {
    job->attempts = 0;
    job->pending = 0;
    return MODEL_STREAM_TIMEOUT;
  }
  return model_stream_requeue(action, model, job);
}

// Game-thread PREWARM job, drained by the frame hook (run_gated_action). Mirrors
// gtav_features_run_spawn_hash_job MINUS the CREATE_VEHICLE tail: validate + REQUEST_MODEL on
// the game-thread lane (ctx 0), self-requeue while the model streams (wall-clock bounded by
// GTAV_MODEL_STREAM_WAIT_SECONDS), and stop once it is resident. Reads g_prewarm_target_model
// (not its param) so it converges to the worker's latest request and stale queued jobs are
// harmless; releases the previously-warmed model when the target moves, so browsing the spawn
// list does not pin every model. Does NOT touch g_state / the toast -- it is a silent
// background accelerator. Always compiled; the requeue is frame-hook-gated.
static int model_stream_frame_advanced(ModelStreamJob* job);

static void run_prewarm_model_job(uint32_t param) {
  (void)param;
  static uint32_t warmed_model = 0;  // game-thread-owned: the model currently kept warm
  // Unused under GTAV_MENU_STREAM_RESIDENT_ONLY, where the warmer stands down before any
  // wall-clock budget matters.
  [[maybe_unused]] static time_t deadline = 0;
  // Own streaming state, so the warmer and a real spawn cannot steal each other's frame claim.
  static ModelStreamJob warm_job;
  const uint32_t target = g_prewarm_target_model;

  // The release below and the REQUEST_MODEL further down are both attributed to the script that
  // owns this fire (eboot 0x1c09760 / 0x1c07fb0), so decline a fire we would not want to own --
  // before the frame claim, for the reason spelled out in model_stream_defer_to_next_frame.
  if (!model_stream_owner_ok()) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (g_prewarm_target_model &&
        gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL, g_prewarm_target_model)) {
      gtav_prewarm_stage = 1;
    }
#endif
    return;
  }

  if (target != warmed_model) {
    // Target moved: release the old model so the streamer can drop it, then adopt the new one.
    if (warmed_model && g_n.set_model_as_no_longer_needed) {
      invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, warmed_model);
    }
    warmed_model = target;
    deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
  }
  if (!warmed_model) {
    gtav_prewarm_stage = 0;  // nothing to warm
    return;
  }
  if (!g_n.request_model || !g_n.has_model_loaded) {
    warmed_model = 0;
    gtav_prewarm_stage = 0;
    return;
  }
  // Same per-frame gate as model_stream_step: the warmer self-requeues on the same
  // many-fires-per-frame hook, so without it it would re-issue REQUEST_MODEL ~137x a frame.
  if (!model_stream_frame_advanced(&warm_job)) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL, warmed_model)) {
      gtav_prewarm_stage = 1;
      return;
    }
#endif
    gtav_prewarm_stage = 3;
    return;
  }
  if ((g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, warmed_model)) ||
      (g_n.is_model_in_cdimage &&
       !invoke_return_ctx<int>(0, g_n.is_model_in_cdimage, warmed_model))) {
    warmed_model = 0;  // a hash the game would reject -- drop it, never pin
    gtav_prewarm_stage = 0;
    return;
  }
#if GTAV_MENU_STREAM_RESIDENT_ONLY
  // Same hazard as model_stream_step: a request for a non-resident model jams the streamer's I/O
  // ring for the rest of the session. The warmer's whole purpose is to pull in a COLD model, so
  // under this guard it cannot do anything useful and must simply stand down.
  if (!invoke_return_ctx<int>(0, g_n.has_model_loaded, warmed_model)) {
    warmed_model = 0;
    gtav_prewarm_stage = 3;
    return;
  }
  gtav_prewarm_stage = 2;
  return;
#else
  invoke_void_ctx(0, g_n.request_model, warmed_model);
  if (invoke_return_ctx<int>(0, g_n.has_model_loaded, warmed_model)) {
    gtav_prewarm_stage = 2;  // resident: a spawn lands on its first fire. Stop requeuing.
    return;
  }
  // Still streaming: keep warming until it loads or the wall-clock budget runs out. The read
  // of `deadline` stays OUTSIDE the frame-hook guard so it is never a set-but-unused static in
  // a no-frame-hook build (mirrors gtav_features_run_spawn_hash_job).
  if (time(NULL) < deadline && g_prewarm_target_model == warmed_model) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL, warmed_model)) {
      gtav_prewarm_stage = 1;  // requeued -- warm again on the next fire
      return;
    }
#endif
  }
  gtav_prewarm_stage = 3;  // deadline hit / queue full / target changed: stop warming
#endif
}

extern "C" void gtav_features_run_spawn_hash_job(uint32_t action, uint32_t model, void* ctx) {
  (void)ctx;
  static ModelStreamJob job;
  g_state.last_param = model;
  // Defer the WHOLE body -- IS_MODEL_* getters included -- until the game renders a new frame
  // AND this fire belongs to an owner worth attributing the request to. Exactly one gate per
  // execution, and it also adopts the model / arms the budget; see
  // model_stream_defer_to_next_frame.
  if (model_stream_defer_to_next_frame(action, model, &job)) return;
  if (!g_n.request_model || !g_n.has_model_loaded) {
    job.pending = 0;
    direct_spawn_result(GTAV_FEATURE_RESULT_UNAVAILABLE, "missing model streaming natives");
    return;
  }
  if (g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, model)) {
    job.pending = 0;
    direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not valid");
    return;
  }
  if (g_n.is_model_in_cdimage && !invoke_return_ctx<int>(0, g_n.is_model_in_cdimage, model)) {
    job.pending = 0;
    direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not in cdimage");
    return;
  }

  switch (model_stream_step(action, model, &job)) {
    case MODEL_STREAM_LOADED:
      break;
    case MODEL_STREAM_PENDING:
      // Edge-triggered: once per in-flight model, not once per frame -- see
      // model_stream_should_report_pending.
      if (model_stream_should_report_pending(&job)) {
        direct_spawn_result(GTAV_FEATURE_RESULT_OK, "model pending");
      }
      return;
    case MODEL_STREAM_QUEUE_FULL:
      direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "main-thread job queue full");
      return;
    case MODEL_STREAM_NO_HOOK:
      direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "frame hook unavailable");
      return;
    case MODEL_STREAM_TIMEOUT:
      direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not loaded");
      return;
    case MODEL_STREAM_NOT_RESIDENT:
      direct_spawn_result(GTAV_FEATURE_RESULT_FAILED, "model not streamed in");
      return;
  }

  spawn_vehicle_model_hash(model, /*allow_block=*/0);
  job.pending = 0;
}

// Skin changer: rebuild the player ped to `model`. Mirrors gtav_features_run_spawn_hash_job
// (game-thread, non-blocking model-streaming with self-requeue) but finishes with
// SET_PLAYER_MODEL + SET_PED_DEFAULT_COMPONENT_VARIATION instead of CREATE_VEHICLE.
// SET_PLAYER_MODEL is allocation-class (deletes/recreates the player ped) so it must
// run on the game thread, exactly like spawning.
extern "C" void gtav_features_run_skin_hash_job(uint32_t action, uint32_t model, void* ctx) {
  (void)ctx;
  const uint32_t feat = GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL;
  static ModelStreamJob job;
  g_state.last_param = model;
  // Defer the WHOLE body until the game renders a new frame AND this fire has an owner worth
  // attributing the request to. Exactly one gate per execution. Before this existed the job
  // issued REQUEST_MODEL and wrote a status event on EVERY fire -- ~137 of each per frame, the
  // same double pathology that starved the streamer and hung GTA on the vehicle lane
  // (2026-09-17). gtav_skin_stage is written AFTER the gate, or the breadcrumb is stamped
  // ~8.2k/sec and localizes nothing.
  if (model_stream_defer_to_next_frame(action, model, &job)) return;
  gtav_skin_stage = 1;
  if (!g_n.set_player_model || !g_n.player_id || !g_n.request_model || !g_n.has_model_loaded) {
    job.pending = 0;
    unavailable(feat, "missing skin natives");
    return;
  }
  // Validate before streaming so a bad/foreign catalog name degrades cleanly.
  if (g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, model)) {
    job.pending = 0;
    failed(feat, "model not valid");
    return;
  }
  if (g_n.is_model_in_cdimage && !invoke_return_ctx<int>(0, g_n.is_model_in_cdimage, model)) {
    job.pending = 0;
    failed(feat, "model not in cdimage");
    return;
  }

  switch (model_stream_step(action, model, &job)) {
    case MODEL_STREAM_LOADED:
      break;
    case MODEL_STREAM_PENDING:
      if (model_stream_should_report_pending(&job)) ok(feat, "model pending");
      return;
    case MODEL_STREAM_QUEUE_FULL:
      failed(feat, "main-thread job queue full");
      return;
    case MODEL_STREAM_NO_HOOK:
      failed(feat, "frame hook unavailable");
      return;
    case MODEL_STREAM_TIMEOUT:
      failed(feat, "model not loaded");
      return;
    case MODEL_STREAM_NOT_RESIDENT:
      failed(feat, "model not streamed in");
      return;
  }

  // Model resident. SET_PLAYER_MODEL takes the PLAYER index (0 is the valid local
  // player; -1 is invalid), so guard with valid_player_id() which preserves index 0.
  gtav_skin_stage = 2;
  int player = valid_player_id();
  if (player < 0) {
    job.pending = 0;
    gtav_skin_stage = 0;
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, model);
    failed(feat, "no valid player");
    return;
  }
  gtav_skin_stage = 3;
  invoke_void_ctx(0, g_n.set_player_model, player, model);
  // SET_PLAYER_MODEL recreates the ped, so the handle changes -- re-fetch it and apply
  // the default component variation, or the new model can appear with no clothes.
  int ped = valid_player_ped();
  if (ped && g_n.set_ped_default_component_variation) {
    gtav_skin_stage = 4;
    invoke_void_ctx(0, g_n.set_ped_default_component_variation, ped);
  }
  gtav_skin_stage = 5;
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, model);
  job.pending = 0;
  gtav_skin_stage = 0;
  ok(feat, "player model set");
}
