// Spooner-lite scene save/load: persist a spawned scene or load a prepared custom map.
//
// Source fragment of features.cpp, textually included AFTER spawned_entities.inc (rosters +
// track_spawned_*), spawn_skin.inc (GTAV_MODEL_STREAM_WAIT_SECONDS) and the create/getter natives.
//
// Versioned map.cfg files begin with `GTAVMAP,1,absolute` or `GTAVMAP,1,relative`. Relative maps
// use player-local coordinates (x right, y forward, z up) and rotate with the player's heading,
// making a prepared map portable between locations. Legacy headerless absolute rows remain
// readable. LOAD_MAP parses the complete file before publishing it to the game-thread consumer;
// LOAD_MAP_STEP then validates, streams and creates one entity per frame-chain step.

// 32 each of vehicle/ped/object (the roster caps). Enforcing the same per-kind bounds keeps every
// loaded entity owned by a cleanup roster instead of silently dropping old handles.
enum { GTAV_SCENE_MAX = 3 * GTAV_SPAWNED_ROSTER_MAX };
enum { GTAV_SCENE_VEHICLE = 0, GTAV_SCENE_PED = 1, GTAV_SCENE_OBJECT = 2 };
enum { GTAV_SCENE_ABSOLUTE = 0, GTAV_SCENE_RELATIVE = 1 };
enum { GTAV_SCENE_FLAG_FROZEN = 1u << 0, GTAV_SCENE_FLAG_GROUND = 1u << 1 };
enum { GTAV_SCENE_FLAG_MASK = GTAV_SCENE_FLAG_FROZEN | GTAV_SCENE_FLAG_GROUND };

struct SceneEntry {
  int kind;
  uint32_t model;
  float x, y, z;
  float rx, ry, rz;  // GET/SET_ENTITY_ROTATION order 2; rz doubles as creation heading
  uint32_t flags;
  time_t deadline;  // per-entity model-stream deadline, set on the first load step
};

static SceneEntry g_scene[GTAV_SCENE_MAX];
static int g_scene_count = 0;
static int g_scene_mode = GTAV_SCENE_ABSOLUTE;
static volatile uint32_t g_scene_load_active = 0;
static int g_scene_anchor_ready = 0;
static Vec3 g_scene_anchor = {};
static float g_scene_anchor_heading = 0.0f;
static int g_scene_loaded_count = 0;
static int g_scene_skipped_count = 0;

static int scene_kind_valid(int kind) {
  return kind == GTAV_SCENE_VEHICLE || kind == GTAV_SCENE_PED || kind == GTAV_SCENE_OBJECT;
}

static int scene_float_valid(float value) { return __builtin_isfinite(value); }

static int scene_entry_valid(const SceneEntry* e, int mode) {
  if (!e || !scene_kind_valid(e->kind) || e->model == 0 || (e->flags & ~GTAV_SCENE_FLAG_MASK))
    return 0;
  if (!scene_float_valid(e->x) || !scene_float_valid(e->y) || !scene_float_valid(e->z) ||
      !scene_float_valid(e->rx) || !scene_float_valid(e->ry) || !scene_float_valid(e->rz))
    return 0;
  const float position_limit = mode == GTAV_SCENE_RELATIVE ? 10000.0f : 100000.0f;
  return fabsf(e->x) <= position_limit && fabsf(e->y) <= position_limit &&
         fabsf(e->z) <= position_limit && fabsf(e->rx) <= 100000.0f &&
         fabsf(e->ry) <= 100000.0f && fabsf(e->rz) <= 100000.0f;
}

static char* scene_trim(char* text) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
  size_t length = strlen(text);
  while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                    text[length - 1] == '\r' || text[length - 1] == '\n'))
    text[--length] = '\0';
  return text;
}

static int scene_parse_row(const char* text, int versioned, int mode, SceneEntry* e) {
  unsigned model = 0;
  unsigned flags = 0;
  int consumed = 0;
  int fields = versioned
                   ? sscanf(text, "%d,0x%x,%f,%f,%f,%f,%f,%f,%u%n", &e->kind, &model, &e->x,
                            &e->y, &e->z, &e->rx, &e->ry, &e->rz, &flags, &consumed)
                   : sscanf(text, "%d,0x%x,%f,%f,%f,%f,%f,%f%n", &e->kind, &model, &e->x,
                            &e->y, &e->z, &e->rx, &e->ry, &e->rz, &consumed);
  if (fields != (versioned ? 9 : 8)) return 0;
  while (text[consumed] == ' ' || text[consumed] == '\t') ++consumed;
  if (text[consumed] != '\0') return 0;
  e->model = model;
  e->flags = flags;
  e->deadline = 0;
  return scene_entry_valid(e, mode);
}

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
    e->flags = 0;
    e->deadline = 0;
    ++g_scene_count;
  }
}

static void scene_remove_temp_file() {
  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  remove(GTAV_CUSTOM_MAP_TEMP_PATH);
  if (rooted) gtav_rootdir_leave(&guard);
}

static uint32_t save_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_MAP;
  if (__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE))
    return unavailable(action, "cancel the active map load first");
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
  // The loader creates /data/GTAVMenu. Create only our two bounded child directories here so
  // in-menu Save Map works even before a host upload has populated the custom namespace.
  mkdir(GTAV_CUSTOM_ASSET_ROOT, 0777);
  mkdir(GTAV_CUSTOM_MAP_ROOT, 0777);
  FILE* fp = fopen(GTAV_CUSTOM_MAP_TEMP_PATH, "w");
  if (rooted) gtav_rootdir_leave(&guard);
  if (!fp) return failed(action, "could not open temporary map file");
  int write_failed = fprintf(fp, "GTAVMAP,1,absolute\n") < 0;
  for (int i = 0; i < g_scene_count && !write_failed; ++i) {
    const SceneEntry* e = &g_scene[i];
    write_failed =
        fprintf(fp, "%d,0x%08X,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u\n", e->kind, e->model,
                e->x, e->y, e->z, e->rx, e->ry, e->rz, e->flags) < 0;
  }
  if (fflush(fp) != 0 || ferror(fp)) write_failed = 1;
  if (fclose(fp) != 0) write_failed = 1;
  if (write_failed) {
    scene_remove_temp_file();
    return failed(action, "map write failed");
  }
  rooted = gtav_rootdir_enter(&guard) == 0;
  int renamed = rename(GTAV_CUSTOM_MAP_TEMP_PATH, GTAV_CUSTOM_MAP_PATH);
  if (rooted) gtav_rootdir_leave(&guard);
  if (renamed != 0) {
    scene_remove_temp_file();
    return failed(action, "could not publish map file");
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "saved %d entities", g_scene_count);
  return ok(action, msg);
}

static SceneEntry scene_world_entry(const SceneEntry* source) {
  SceneEntry world = *source;
  if (g_scene_mode == GTAV_SCENE_RELATIVE) {
    const float radians = g_scene_anchor_heading * GTAV_FEAT_DEG2RAD;
    const float sine = feat_sinf(radians);
    const float cosine = feat_cosf(radians);
    world.x = g_scene_anchor.x + source->x * cosine - source->y * sine;
    world.y = g_scene_anchor.y + source->x * sine + source->y * cosine;
    world.z = g_scene_anchor.z + source->z;
    world.rz = source->rz + g_scene_anchor_heading;
    while (world.rz >= 360.0f) world.rz -= 360.0f;
    while (world.rz < 0.0f) world.rz += 360.0f;
  }
  return world;
}

// CREATE the scene entity at its resolved transform (game-thread context). Tracks it on the
// matching roster so it participates in the Clear actions / a later save.
static int create_scene_entity(const SceneEntry* source) {
  SceneEntry world = scene_world_entry(source);
  int handle = 0;
  if (world.kind == GTAV_SCENE_VEHICLE && g_n.create_vehicle) {
    handle = invoke_return_ctx<int>(0, g_n.create_vehicle, (int)world.model, world.x, world.y,
                                    world.z, world.rz, 0, 0);
  } else if (world.kind == GTAV_SCENE_PED && g_n.create_ped) {
    handle = invoke_return_ctx<int>(0, g_n.create_ped, 4, (int)world.model, world.x, world.y,
                                    world.z, world.rz, 0, 0);
  } else if (world.kind == GTAV_SCENE_OBJECT && g_n.create_object) {
    handle = invoke_return_ctx<int>(0, g_n.create_object, (int)world.model, world.x, world.y,
                                    world.z, 0, 1, 1);
  }
  if (handle <= 0) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, (int)world.model);
    return 0;
  }
  if ((world.flags & GTAV_SCENE_FLAG_GROUND) && world.kind == GTAV_SCENE_OBJECT &&
      g_n.place_object_on_ground_properly)
    invoke_void_ctx(0, g_n.place_object_on_ground_properly, handle);
  // Restore the full 3-axis rotation (CREATE_* only takes a heading; objects take none).
  if (g_n.set_entity_rotation)
    invoke_void_ctx(0, g_n.set_entity_rotation, handle, world.rx, world.ry, world.rz, 2, 1);
  if ((world.flags & GTAV_SCENE_FLAG_FROZEN) && g_n.freeze_entity_position)
    invoke_void_ctx(0, g_n.freeze_entity_position, handle, 1);
  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(0, g_n.set_entity_as_mission_entity, handle, 1, 1);
  if (world.kind == GTAV_SCENE_VEHICLE) {
    track_spawned_vehicle(handle);
  } else if (world.kind == GTAV_SCENE_PED) {
    track_spawned_ped(handle);
  } else {
    track_spawned_object_model(handle, (int)world.model);
    g_last_spawned_object = handle;
  }
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, (int)world.model);
  return 1;
}

static uint32_t scene_finish_load() {
  __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "map loaded: %d ok, %d skipped", g_scene_loaded_count,
           g_scene_skipped_count);
  return g_scene_loaded_count ? ok(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP, msg)
                              : failed(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP, msg);
}

// Chain to the next scene entity (game-thread), or publish the final result. A queue failure stops
// the load and clears the active claim so another load can be attempted safely.
static uint32_t load_map_chain_next(uint32_t idx) {
  if (!__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE)) return GTAV_FEATURE_RESULT_NONE;
  if ((int)(idx + 1u) >= g_scene_count) return scene_finish_load();
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, idx + 1u)) {
    char msg[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(msg, sizeof(msg), "loading map %d/%d...", (int)idx + 1, g_scene_count);
    return ok(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP, msg);
  }
#endif
  __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
  return failed(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP, "map load queue failed");
}

static uint32_t scene_skip(uint32_t idx, const char* reason) {
  ++g_scene_skipped_count;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "map entry %u skipped: %s", idx, reason);
  return load_map_chain_next(idx);
}

// Game-thread per-entity load step: validate and stream the model, create at the resolved
// transform, and chain on. Self-requeues this same index while the model streams.
static uint32_t load_map_step(uint32_t idx) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP;
  if (!__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE)) return GTAV_FEATURE_RESULT_NONE;
  if ((int)idx >= g_scene_count) return scene_finish_load();
  if (g_scene_mode == GTAV_SCENE_RELATIVE && !g_scene_anchor_ready) {
    if (!g_n.player_ped_id || !g_n.get_entity_coords || !g_n.get_entity_heading) {
      __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
      return unavailable(action, "missing relative-map anchor natives");
    }
    int ped = native_player_ped_id();
    if (ped <= 0) {
      __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
      return failed(action, "no valid player ped");
    }
    g_scene_anchor = invoke_vector(g_n.get_entity_coords, ped, 1);
    g_scene_anchor_heading = invoke_return<float>(g_n.get_entity_heading, ped);
    g_scene_anchor_ready = 1;
  }
  SceneEntry* e = &g_scene[idx];
  if (!g_n.request_model || !g_n.has_model_loaded) {
    __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
    return unavailable(action, "missing model streaming natives");
  }
  if (g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, e->model))
    return scene_skip(idx, "model invalid");
  if (g_n.is_model_in_cdimage && !invoke_return_ctx<int>(0, g_n.is_model_in_cdimage, e->model))
    return scene_skip(idx, "model absent from cdimage");
  if (e->deadline == 0) e->deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
  invoke_void_ctx(0, g_n.request_model, (int)e->model);
  if (!invoke_return_ctx<int>(0, g_n.has_model_loaded, (int)e->model)) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (time(NULL) < e->deadline &&
        gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, idx)) {
      return ok(action, "loading map model...");
    }
#endif
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, (int)e->model);
    return scene_skip(idx, "model stream timed out");
  }
  if (create_scene_entity(e)) {
    ++g_scene_loaded_count;
  } else {
    ++g_scene_skipped_count;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "map entry %u skipped: create failed", idx);
  }
  return load_map_chain_next(idx);
}

static uint32_t cancel_map_load() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CANCEL_MAP_LOAD;
  if (!__atomic_exchange_n(&g_scene_load_active, 0u, __ATOMIC_ACQ_REL))
    return unavailable(action, "no active map load");
  return ok(action, "map load cancelled");
}

// Read up to size-1 bytes of a small text file as one line. Returns the byte count, or -errno.
static int read_probe_text(const char* path, char* text, size_t size) {
  text[0] = '\0';
  FILE* fp = fopen(path, "r");
  if (!fp) return errno ? -errno : -1;
  size_t n = fread(text, 1, size - 1, fp);
  fclose(fp);
  text[n] = '\0';
  for (size_t i = 0; i < n; ++i) {
    if (text[i] == '\n' || text[i] == '\r') {
      text[i] = '\0';
      break;
    }
  }
  return (int)n;
}

// Worker-direct: report whether the custom-asset root is readable from inside GTA, which needs the
// loader's sandbox mount (custom_assets.h).
static uint32_t probe_custom_mount() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PROBE_CUSTOM_MOUNT;
  char text[40];
  int rc = read_probe_text(GTAV_CUSTOM_PROBE_PATH, text, sizeof(text));
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "custom probe %s rc=%d text=%s",
                     GTAV_CUSTOM_PROBE_PATH, rc, text);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "probe rc=%d", rc);
  if (rc < 0) return unavailable(action, msg);
  return ok(action, msg);
}

// Worker-direct: parse the complete map into a private staging array, then atomically publish the
// scene and enqueue its first game-thread step. Malformed files never partially replace g_scene.
static uint32_t load_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP;
  if (__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE))
    return unavailable(action, "map load already active");
  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  // GTA sees GTAV_CUSTOM_MAP_PATH only through the loader's sandbox mount (custom_assets.h).
  FILE* fp = fopen(GTAV_CUSTOM_MAP_PATH, "r");
  if (!fp) fp = fopen(GTAV_LEGACY_MAP_PATH, "r");
  if (rooted) gtav_rootdir_leave(&guard);
  if (!fp) return unavailable(action, "no saved/custom map");

  SceneEntry parsed[GTAV_SCENE_MAX] = {};
  int parsed_count = 0;
  int kind_counts[3] = {};
  int mode = GTAV_SCENE_ABSOLUTE;
  int versioned = 0;
  int saw_content = 0;
  int invalid_line = 0;
  int line_number = 0;
  char line[192];
  while (fgets(line, sizeof(line), fp)) {
    ++line_number;
    if (!strchr(line, '\n') && !feof(fp)) {
      invalid_line = line_number;
      break;
    }
    char* text = scene_trim(line);
    if (!*text || *text == '#') continue;
    if (!saw_content && !strcmp(text, "GTAVMAP,1,absolute")) {
      versioned = 1;
      mode = GTAV_SCENE_ABSOLUTE;
      saw_content = 1;
      continue;
    }
    if (!saw_content && !strcmp(text, "GTAVMAP,1,relative")) {
      versioned = 1;
      mode = GTAV_SCENE_RELATIVE;
      saw_content = 1;
      continue;
    }
    saw_content = 1;
    if (parsed_count >= GTAV_SCENE_MAX || !strncmp(text, "GTAVMAP,", 8) ||
        !scene_parse_row(text, versioned, mode, &parsed[parsed_count])) {
      invalid_line = line_number;
      break;
    }
    int kind = parsed[parsed_count].kind;
    if (++kind_counts[kind] > GTAV_SPAWNED_ROSTER_MAX) {
      invalid_line = line_number;
      break;
    }
    ++parsed_count;
  }
  int read_failed = ferror(fp);
  fclose(fp);
  if (read_failed || invalid_line) {
    char msg[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(msg, sizeof(msg), "invalid map at line %d", invalid_line ? invalid_line : line_number);
    return failed(action, msg);
  }
  if (parsed_count == 0) return unavailable(action, "map is empty");
  if ((kind_counts[GTAV_SCENE_VEHICLE] && !g_n.create_vehicle) ||
      (kind_counts[GTAV_SCENE_PED] && !g_n.create_ped) ||
      (kind_counts[GTAV_SCENE_OBJECT] && !g_n.create_object))
    return unavailable(action, "missing map entity creation native");
  if (!g_n.request_model || !g_n.has_model_loaded || !g_n.is_model_valid ||
      !g_n.is_model_in_cdimage)
    return unavailable(action, "missing map validation/streaming native");
  if (!g_n.set_entity_rotation || !g_n.set_entity_as_mission_entity)
    return unavailable(action, "missing map transform/ownership native");
  for (int i = 0; i < parsed_count; ++i) {
    if ((parsed[i].flags & GTAV_SCENE_FLAG_FROZEN) && !g_n.freeze_entity_position)
      return unavailable(action, "map needs freeze native");
    if ((parsed[i].flags & GTAV_SCENE_FLAG_GROUND) && !g_n.place_object_on_ground_properly)
      return unavailable(action, "map needs ground-placement native");
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  for (int i = 0; i < parsed_count; ++i) g_scene[i] = parsed[i];
  g_scene_count = parsed_count;
  g_scene_mode = mode;
  g_scene_anchor_ready = 0;
  g_scene_loaded_count = 0;
  g_scene_skipped_count = 0;
  __atomic_store_n(&g_scene_load_active, 1u, __ATOMIC_RELEASE);
  if (!gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, 0)) {
    __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
    return failed(action, "main-thread job queue full");
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "loading %d %s entities...", g_scene_count,
           mode == GTAV_SCENE_RELATIVE ? "relative" : "absolute");
  return ok(action, msg);
#else
  return unavailable(action, "needs main-thread hook");
#endif
}
