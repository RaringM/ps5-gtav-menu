// Ped / object spawner (model streaming + game-thread create), mirroring spawn_skin.inc.
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp (wait_for_model, f32_from_bits, invoke_*_ctx,
// invoke_vector, native_player_ped_id). Not a standalone translation unit.

// Spawn progress markers, same idea as gtav_spawn_stage: written on the game thread so a
// fault is localizable from a crash read. 0 = idle.
//   1 entered  2 model-ready  3 have-ped  4 create  5 configure/done
extern "C" {
volatile uint32_t gtav_ped_spawn_stage = 0;
volatile uint32_t gtav_object_spawn_stage = 0;
}

// --- Object spawn placement (Spawn > Objects cyclers) ---
// These file-local statics hold the placement settings the object-spawner rows tune; the
// spawn job reads them when it creates the prop. All worker-safe (pure state); the cyclers
// follow the standard stage(1/2)/apply(0) convention -- staging is enough because the spawn
// reads the live index, so apply is just an acknowledging toast.

// Reference frame the prop is placed in front of.
enum { GTAV_OBJ_SPAWN_AT_PLAYER = 0, GTAV_OBJ_SPAWN_AT_CAMERA = 1 };
static const char kObjSpawnAtLabels[][8] = {"Player", "Camera"};
static uint32_t g_obj_spawn_at_index = GTAV_OBJ_SPAWN_AT_PLAYER;

// Distance in front of the reference frame, in metres.
struct GtavObjDistance {
  char label[8];  // inline (no relocation)
  float metres;
};
static const GtavObjDistance kObjDistances[] = {
    {"2.0m", 2.0f}, {"3.0m", 3.0f}, {"5.0m", 5.0f}, {"8.0m", 8.0f}, {"12.0m", 12.0f},
};
static uint32_t g_obj_distance_index = 1;  // default 3.0m (matches the old fixed offset)

// Prop facing. "Match" aligns the prop to the placement-forward yaw; the rest are fixed GTA
// headings (0 = North, increasing counter-clockwise: 90 = West, 180 = South, 270 = East).
struct GtavObjHeading {
  char label[8];  // inline (no relocation)
  float degrees;
  int match;  // 1 = use the placement-forward yaw instead of `degrees`
};
static const GtavObjHeading kObjHeadings[] = {
    {"Match", 0.0f, 1}, {"North", 0.0f, 0},  {"East", 270.0f, 0},
    {"South", 180.0f, 0}, {"West", 90.0f, 0},
};
static uint32_t g_obj_heading_index = 0;  // default Match

// Whether to snap the prop to the ground (PLACE_OBJECT_ON_GROUND_PROPERLY) after placing.
static const char kObjOnGroundLabels[][4] = {"ON", "OFF"};
static uint32_t g_obj_on_ground_index = 0;  // default ON

// Reference yaw (degrees) the prop is placed along: the gameplay-camera yaw when "Spawn At:
// Camera", else the player's heading. Reads GET_GAMEPLAY_CAM_ROT directly (it takes a
// rotationOrder arg, not an entity, so invoke_vector can't drive it); runs on the game thread,
// where the camera getter is valid (this path drains via the frame hook). yaw = the z (heading)
// component, same as noclip's camera read.
static float obj_base_yaw_deg(int ped) {
  if (g_obj_spawn_at_index == GTAV_OBJ_SPAWN_AT_CAMERA && g_n.get_gameplay_cam_rot) {
    gtavmenu::NativeCallContext ctx;
    ctx.reset();
    ctx.push(2);  // rotationOrder 2 (ZXY) -- the Rockstar default
    gtavmenu::note_native_call(g_n.get_gameplay_cam_rot);  // crash-breadcrumb parity with the helpers
    ((void (*)(gtavmenu::NativeArg*))(uintptr_t)g_n.get_gameplay_cam_rot)(&ctx.native_arg);
    return *reinterpret_cast<const float*>(&ctx.returns[2]);
  }
  if (g_n.get_entity_heading) return invoke_return<float>(g_n.get_entity_heading, ped);
  return 0.0f;
}

static uint32_t cycle_object_spawn_at(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_SPAWN_AT;
  const uint32_t count = (uint32_t)(sizeof(kObjSpawnAtLabels) / sizeof(kObjSpawnAtLabels[0]));
  stage_step(&g_obj_spawn_at_index, count, param);
  return ok(action, kObjSpawnAtLabels[g_obj_spawn_at_index % count]);
}

static uint32_t cycle_object_distance(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_DISTANCE;
  const uint32_t count = (uint32_t)(sizeof(kObjDistances) / sizeof(kObjDistances[0]));
  stage_step(&g_obj_distance_index, count, param);
  return ok(action, kObjDistances[g_obj_distance_index % count].label);
}

static uint32_t cycle_object_heading(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_HEADING;
  const uint32_t count = (uint32_t)(sizeof(kObjHeadings) / sizeof(kObjHeadings[0]));
  stage_step(&g_obj_heading_index, count, param);
  return ok(action, kObjHeadings[g_obj_heading_index % count].label);
}

static uint32_t cycle_object_on_ground(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_ON_GROUND;
  const uint32_t count = (uint32_t)(sizeof(kObjOnGroundLabels) / sizeof(kObjOnGroundLabels[0]));
  stage_step(&g_obj_on_ground_index, count, param);
  return ok(action, kObjOnGroundLabels[g_obj_on_ground_index % count]);
}

static uint32_t spawn_ped_model_hash(uint32_t model, int allow_block) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_PED;
  gtav_ped_spawn_stage = 1;
  g_state.last_param = model;
  if (!g_n.create_ped || !g_n.request_model || !g_n.has_model_loaded || !g_n.player_ped_id ||
      !g_n.get_entity_coords) {
    gtav_ped_spawn_stage = 0;
    return unavailable(action, "missing ped spawn natives");
  }
  if (g_n.is_model_valid && !invoke_return<int>(g_n.is_model_valid, model)) {
    gtav_ped_spawn_stage = 0;
    return failed(action, "model not valid");
  }
  if (g_n.is_model_in_cdimage && !invoke_return<int>(g_n.is_model_in_cdimage, model)) {
    gtav_ped_spawn_stage = 0;
    return failed(action, "model not in cdimage");
  }
  if (!wait_for_model(model, allow_block)) {
    gtav_ped_spawn_stage = 0;
    return failed(action, "model not loaded");
  }
  gtav_ped_spawn_stage = 2;
  int ped = native_player_ped_id();
  if (ped <= 0) {
    gtav_ped_spawn_stage = 0;
    return failed(action, "no valid player ped (be in active gameplay)");
  }
  gtav_ped_spawn_stage = 3;
  Vec3 pos = invoke_vector(g_n.get_entity_coords, ped, 1);
  float sx = pos.x + f32_from_bits(0x40400000u);  // +3.0f
  float sy = pos.y + f32_from_bits(0x40400000u);  // +3.0f (offset diagonally, not on the player)
  float sz = pos.z;
  float heading = f32_from_bits(0x00000000u);
  const int worker = allow_block;
  gtav_ped_spawn_stage = 4;
  // CREATE_PED(pedType, modelHash, x, y, z, heading, isNetwork, thisScriptCheck).
  // pedType 4 = CIVMALE.
  int newped =
      invoke_return_ctx<int>(worker, g_n.create_ped, 4, (int)model, sx, sy, sz, heading, 0, 0);
  if (!newped) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
    gtav_ped_spawn_stage = 0;
    return failed(action, "create ped failed");
  }
  gtav_ped_spawn_stage = 5;
  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(worker, g_n.set_entity_as_mission_entity, newped, 1, 1);
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
  // Task the new ped to wander so it walks around instead of standing idle (a CREATE_PED ped gets no
  // primary task; GTA's ambient AI never tasks script-created peds). Keep it mission-owned (above)
  // so the Spawned Entities manager can still enumerate/delete it; set_ped_keep_task stops an
  // ambient event from reverting it to standing. TASK_WANDER_STANDARD(ped, heading, flags); flag 10
  // is the common scripted value.
  if (g_n.task_wander_standard)
    invoke_void_ctx(worker, g_n.task_wander_standard, newped, f32_from_bits(0x41200000u), 10);  // 10.0f
  if (g_n.set_ped_keep_task) invoke_void_ctx(worker, g_n.set_ped_keep_task, newped, 1);
  track_spawned_ped(newped);
  gtav_ped_spawn_stage = 0;
  return ok(action, "spawned ped (wandering)");
}

// The loaded pack spawn row declaring `model` (custom_pack_autoload.inc; a stub without the pack
// lane): its pack id and model name, and whether the packs are loaded.
static int scene_pack_model(uint32_t model, const char** pack_id, const char** name, int* ready);

// A placement event for the hardware check: a status event and, in pack-lane builds (the worker
// klog is linked with it), a "GTAVMenu pack ..." klog / pack-notes line. `format` starts "pack ".
static void placement_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void placement_note(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, text);
#if GTAV_MENU_ENABLE_CUSTOM_PACKS
  char line[sizeof(text) + 16];
  snprintf(line, sizeof(line), "GTAVMenu %s", text);
  gtav_worker_klog(line);
#endif
}

static uint32_t spawn_object_model_hash(uint32_t model, int allow_block) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT;
  gtav_object_spawn_stage = 1;
  g_state.last_param = model;
  if (!g_n.create_object || !g_n.request_model || !g_n.has_model_loaded || !g_n.player_ped_id ||
      !g_n.get_entity_coords) {
    gtav_object_spawn_stage = 0;
    return unavailable(action, "missing object spawn natives");
  }
  if (g_n.is_model_valid && !invoke_return<int>(g_n.is_model_valid, model)) {
    gtav_object_spawn_stage = 0;
    return failed(action, "model not valid");
  }
  if (g_n.is_model_in_cdimage && !invoke_return<int>(g_n.is_model_in_cdimage, model)) {
    gtav_object_spawn_stage = 0;
    return failed(action, "model not in cdimage");
  }
  if (!wait_for_model(model, allow_block)) {
    gtav_object_spawn_stage = 0;
    return failed(action, "model not loaded");
  }
  gtav_object_spawn_stage = 2;
  int ped = native_player_ped_id();
  if (ped <= 0) {
    gtav_object_spawn_stage = 0;
    return failed(action, "no valid player ped (be in active gameplay)");
  }
  gtav_object_spawn_stage = 3;
  // Placement is driven by the Spawn Object cyclers (Spawn At / Distance / Heading / On
  // Ground). Project `dist` metres along the reference-frame forward (player heading, or the
  // gameplay-camera yaw for "Spawn At: Camera"). GTA forward from yaw is (-sin, cos); see
  // noclip.inc for the same convention.
  Vec3 pos = invoke_vector(g_n.get_entity_coords, ped, 1);
  const uint32_t dist_count = (uint32_t)(sizeof(kObjDistances) / sizeof(kObjDistances[0]));
  const float dist = kObjDistances[g_obj_distance_index % dist_count].metres;
  const float base_yaw_deg = obj_base_yaw_deg(ped);
  const float yaw_rad = base_yaw_deg * GTAV_FEAT_DEG2RAD;
  float sx = pos.x - feat_sinf(yaw_rad) * dist;
  float sy = pos.y + feat_cosf(yaw_rad) * dist;
  float sz = pos.z;
  const int worker = allow_block;
  gtav_object_spawn_stage = 4;
  // CREATE_OBJECT(modelHash, x, y, z, isNetwork, thisScriptCheck, dynamic).
  int obj =
      invoke_return_ctx<int>(worker, g_n.create_object, (int)model, sx, sy, sz, 0, 1, 1);
  if (!obj) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
    gtav_object_spawn_stage = 0;
    return failed(action, "create object failed");
  }
  gtav_object_spawn_stage = 5;
  // Facing: align to the placement-forward yaw ("Match") or a fixed cardinal heading.
  if (g_n.set_entity_heading) {
    const uint32_t head_count = (uint32_t)(sizeof(kObjHeadings) / sizeof(kObjHeadings[0]));
    const GtavObjHeading& h = kObjHeadings[g_obj_heading_index % head_count];
    invoke_void_ctx(worker, g_n.set_entity_heading, obj, h.match ? base_yaw_deg : h.degrees);
  }
  // Ground snap is opt-out via the "Place on Ground" cycler (index 0 = ON).
  if (g_obj_on_ground_index == 0 && g_n.place_object_on_ground_properly)
    invoke_void_ctx(worker, g_n.place_object_on_ground_properly, obj);
  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(worker, g_n.set_entity_as_mission_entity, obj, 1, 1);
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
  track_spawned_object_model(obj, (int)model);  // record the hash so move mode can duplicate
  g_last_spawned_object = obj;                   // edit target for the interactive move mode
  // Auto-edit on spawn: drop straight into the controller-driven move/reorient mode (the menu
  // hides). We are on the game thread here (the spawn job drains via the frame hook), so the
  // next driver tick acquires this object. Flag-only: clear noclip (mutual exclusion), force a
  // re-acquire (entity = 0) and raise the menu-hide request the worker consumes. Start each
  // freshly-spawned prop in free-move on itself (reset aim sub-mode + selection cursor).
  if (g_auto_edit_object_enabled) {
    g_noclip_enabled = 0;
    g_object_move_entity = 0;
    g_object_move_mode_enabled = 1;
    g_object_move_vis_request = 1;
    g_object_move_aim_mode = 0;
    g_object_move_surface_snap = 0;
    // Auto-clamp the fresh prop to the ground each frame so it follows the terrain instead of
    // hanging at the player's spawn height (the reported "Y level stays the same"); the user can
    // still raise it with L2/R2 or toggle the lock off with D-pad Down.
    g_object_move_ground_lock = 1;
    // Circle cancels the spawn (deletes this prop), Cross places it -- the prop is brand new here.
    g_object_move_cancel_deletes = 1;
    g_object_move_sel_index = -1;
    // Match enter_object_move_mode: ignore a Cross/Square still held from the spawn press so the
    // freshly-spawned prop is not instantly committed before the user can move it. The driver seeds
    // its button latches from the live pad state on its first tick (object_move.inc).
    g_object_move_consume_entry = 1;
  }
  gtav_object_spawn_stage = 0;
  // A pack prop: its model name in the toast and a pack-notes line (Save Map stores the same name).
  const char* pack_id = nullptr;
  const char* name = nullptr;
  if (scene_pack_model(model, &pack_id, &name, nullptr)) {
    placement_note("pack object spawned model=0x%08x name=%s pack=%s handle=%d", (unsigned)model,
                   name, pack_id, obj);
    char msg[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(msg, sizeof(msg), "spawned %.60s", name);
    return ok(action, msg);
  }
  return ok(action, "spawned object");
}

// Game-thread spawn jobs: non-blocking model-streaming with self-requeue while the model
// loads (deadline-bounded), then spawn. Mirrors gtav_features_run_spawn_hash_job. The two
// jobs keep independent pending/deadline state.
static void run_entity_spawn_job(uint32_t action, uint32_t model, int is_ped) {
  // Independent streaming state per family: a ped and an object streaming at the same time must
  // not share a frame claim or an attempt budget.
  static ModelStreamJob job_ped, job_obj;
  ModelStreamJob* job = is_ped ? &job_ped : &job_obj;
  if (model_stream_defer_to_next_frame(action, model, job)) return;
  if (!g_n.request_model || !g_n.has_model_loaded) {
    job->pending = 0;
    unavailable(action, "missing model streaming natives");
    return;
  }
  if (g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, model)) {
    job->pending = 0;
    failed(action, "model not valid");
    return;
  }
  switch (model_stream_step(action, model, job)) {
    case MODEL_STREAM_LOADED:
      break;
    case MODEL_STREAM_PENDING:
      if (model_stream_should_report_pending(job)) ok(action, "model pending");
      return;
    case MODEL_STREAM_QUEUE_FULL:
      failed(action, "main-thread job queue full");
      return;
    case MODEL_STREAM_NO_HOOK:
      failed(action, "frame hook unavailable");
      return;
    case MODEL_STREAM_TIMEOUT:
      failed(action, "model not loaded");
      return;
    case MODEL_STREAM_NOT_RESIDENT:
      failed(action, "model not streamed in");
      return;
  }
  if (is_ped) {
    spawn_ped_model_hash(model, /*allow_block=*/0);
  } else {
    spawn_object_model_hash(model, /*allow_block=*/0);
  }
  job->pending = 0;
}
