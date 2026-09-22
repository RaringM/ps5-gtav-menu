// Spooner-lite scene save/load: persist the spawned-entity scene to a file and re-spawn it.
//
// Source fragment of features.cpp, textually included AFTER spawned_entities.inc (rosters +
// track_spawned_*), spawn_skin.inc (GTAV_MODEL_STREAM_WAIT_SECONDS) and the create/getter natives.
//
// SAVE_MAP is worker-direct: it reads each live roster handle's model + transform (plain getters)
// and writes /data/GTAVMenu/map.cfg via the rootdir guard (same IO lane as the profile). LOAD_MAP
// is worker-direct too -- it parses the file into an in-memory scene table and enqueues the first
// LOAD_MAP_STEP. LOAD_MAP_STEP is the game-thread job: it streams one entity's model (self-requeue
// while loading, like the spawn jobs), CREATE_*s it at the saved transform, tracks it, then chains
// to the next entity. One entity is in flight at a time, so the 32-slot job queue never overflows.

#define GTAV_MAP_PATH "/data/GTAVMenu/map.cfg"
// 32 each of vehicle/ped/object (the roster caps) -- a save can never exceed the rosters it reads.
enum { GTAV_SCENE_MAX = 3 * GTAV_SPAWNED_ROSTER_MAX };
enum { GTAV_SCENE_VEHICLE = 0, GTAV_SCENE_PED = 1, GTAV_SCENE_OBJECT = 2 };

struct SceneEntry {
  int kind;
  uint32_t model;
  float x, y, z;     // world position
  float rx, ry, rz;  // rotation (GET/SET_ENTITY_ROTATION order 2); rz doubles as heading
  time_t deadline;   // per-entity model-stream deadline, set on the first load step
};
static SceneEntry g_scene[GTAV_SCENE_MAX];
static int g_scene_count = 0;

// Append the live, still-alive handles of one roster to the scene table. `model_arr` (parallel to
// the roster) supplies object model hashes; for vehicles/peds it is null and GET_ENTITY_MODEL is
// read instead. Worker-safe: coords/rotation/model are plain getters.
static void scene_capture_roster(int kind, const int* roster, int count, const int* model_arr) {
  for (int i = 0; i < count && g_scene_count < GTAV_SCENE_MAX; ++i) {
    int h = roster[i];
    if (!roster_handle_alive(h)) continue;
    uint32_t model = 0;
    if (model_arr) {
      model = (uint32_t)model_arr[i];
    } else if (g_n.get_entity_model) {
      model = invoke_return<uint32_t>(g_n.get_entity_model, h);
    }
    if (!model) continue;
    SceneEntry* e = &g_scene[g_scene_count];
    e->kind = kind;
    e->model = model;
    Vec3 c = invoke_vector(g_n.get_entity_coords, h, 1);
    e->x = c.x;
    e->y = c.y;
    e->z = c.z;
    Vec3 r = invoke_vector(g_n.get_entity_rotation, h, 2);
    e->rx = r.x;
    e->ry = r.y;
    e->rz = r.z;
    e->deadline = 0;
    ++g_scene_count;
  }
}

static uint32_t save_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_MAP;
  if (!g_n.get_entity_coords || !g_n.get_entity_rotation || !g_n.get_entity_model) {
    return unavailable(action, "missing entity getter natives");
  }
  g_scene_count = 0;
  scene_capture_roster(GTAV_SCENE_VEHICLE, g_spawned_vehicle_roster, g_spawned_vehicle_count, NULL);
  scene_capture_roster(GTAV_SCENE_PED, g_spawned_ped_roster, g_spawned_ped_count, NULL);
  scene_capture_roster(GTAV_SCENE_OBJECT, g_spawned_object_roster, g_spawned_object_count,
                       g_spawned_object_model);
  if (g_scene_count == 0) return unavailable(action, "no spawned entities to save");

  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  FILE* fp = fopen(GTAV_MAP_PATH, "w");
  if (rooted) gtav_rootdir_leave(&guard);
  if (!fp) return failed(action, "could not open map file");
  for (int i = 0; i < g_scene_count; ++i) {
    const SceneEntry* e = &g_scene[i];
    fprintf(fp, "%d,0x%08X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", e->kind, e->model, e->x, e->y, e->z,
            e->rx, e->ry, e->rz);
  }
  fclose(fp);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "saved %d entities", g_scene_count);
  return ok(action, msg);
}

// CREATE the scene entity at its saved transform (game-thread context). Tracks it on the matching
// roster so it participates in the Clear actions / a later save.
static void create_scene_entity(const SceneEntry* e) {
  int handle = 0;
  if (e->kind == GTAV_SCENE_VEHICLE && g_n.create_vehicle) {
    handle =
        invoke_return_ctx<int>(0, g_n.create_vehicle, (int)e->model, e->x, e->y, e->z, e->rz, 0, 0);
  } else if (e->kind == GTAV_SCENE_PED && g_n.create_ped) {
    handle = invoke_return_ctx<int>(0, g_n.create_ped, 4, (int)e->model, e->x, e->y, e->z, e->rz, 0,
                                    0);
  } else if (e->kind == GTAV_SCENE_OBJECT && g_n.create_object) {
    handle = invoke_return_ctx<int>(0, g_n.create_object, (int)e->model, e->x, e->y, e->z, 0, 1, 1);
  }
  if (handle > 0) {
    // Restore the full 3-axis rotation (CREATE_* only takes a heading; objects take none).
    if (g_n.set_entity_rotation)
      invoke_void_ctx(0, g_n.set_entity_rotation, handle, e->rx, e->ry, e->rz, 2, 1);
    if (e->kind == GTAV_SCENE_VEHICLE) {
      track_spawned_vehicle(handle);
    } else if (e->kind == GTAV_SCENE_PED) {
      track_spawned_ped(handle);
    } else {
      track_spawned_object_model(handle, (int)e->model);
    }
  }
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, (int)e->model);
}

// Chain to the next scene entity (game-thread). No-op past the end -- the load is complete.
static void load_map_chain_next(uint32_t idx) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if ((int)(idx + 1u) < g_scene_count)
    gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, idx + 1u);
#else
  (void)idx;
#endif
}

// Game-thread per-entity load step (run from run_gated_action): stream the model, then create at
// the saved transform and chain on. Self-requeues this same index while the model streams.
static uint32_t load_map_step(uint32_t idx) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP;
  if ((int)idx >= g_scene_count) return ok(action, "map loaded");
  SceneEntry* e = &g_scene[idx];
  if (!g_n.request_model || !g_n.has_model_loaded) {
    return unavailable(action, "missing model streaming natives");
  }
  if (e->deadline == 0) e->deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
  invoke_void_ctx(0, g_n.request_model, (int)e->model);
  if (!invoke_return_ctx<int>(0, g_n.has_model_loaded, (int)e->model)) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (time(NULL) < e->deadline && gtav_frame_hook_enqueue(action, idx)) {
      return ok(action, "loading map...");
    }
#endif
    // Timed out (or could not requeue): skip this entity and keep loading the rest.
    load_map_chain_next(idx);
    return failed(action, "entity model timed out");
  }
  create_scene_entity(e);
  load_map_chain_next(idx);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "loaded entity %d/%d", (int)idx + 1, g_scene_count);
  return ok(action, msg);
}

// Worker-direct: parse the map file into the scene table and kick off the streaming load.
static uint32_t load_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP;
  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  FILE* fp = fopen(GTAV_MAP_PATH, "r");
  if (rooted) gtav_rootdir_leave(&guard);
  if (!fp) return unavailable(action, "no saved map");
  g_scene_count = 0;
  char line[128];
  while (fgets(line, sizeof(line), fp) && g_scene_count < GTAV_SCENE_MAX) {
    SceneEntry* e = &g_scene[g_scene_count];
    unsigned model = 0;
    if (sscanf(line, "%d,0x%x,%f,%f,%f,%f,%f,%f", &e->kind, &model, &e->x, &e->y, &e->z, &e->rx,
               &e->ry, &e->rz) == 8) {
      e->model = model;
      e->deadline = 0;
      ++g_scene_count;
    }
  }
  fclose(fp);
  if (g_scene_count == 0) return unavailable(action, "saved map is empty");
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  if (!gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, 0)) {
    return failed(action, "main-thread job queue full");
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "loading %d entities...", g_scene_count);
  return ok(action, msg);
#else
  return unavailable(action, "needs main-thread hook");
#endif
}
