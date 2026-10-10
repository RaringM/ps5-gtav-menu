// Spooner-lite scene save/load: persist a spawned scene or load a prepared custom map.
//
// Source fragment of features.cpp, textually included AFTER spawned_entities.inc (rosters +
// track_spawned_*), spawn_skin.inc (GTAV_MODEL_STREAM_WAIT_SECONDS), spawn_entity.inc
// (scene_pack_model, placement_note) and the create/getter natives.
//
// File format: include/gtavmenu/scene_map.h (src/common/scene_map.c parses and formats it; the file
// is untrusted). Save Map writes `GTAVMAP,2,absolute`; a row whose model is a loaded pack's spawn
// row (Custom Packs) also names that pack and the model, so a later Load Map in a new process
// refuses with the pack's name until Custom Packs -> Load has registered it, instead of skipping
// the entity as an unknown model. LOAD_MAP parses the complete file before publishing it to the
// game-thread consumer; LOAD_MAP_STEP then validates, streams and creates one entity per step.

// 32 each of vehicle/ped/object (the roster caps). Enforcing the same per-kind bounds keeps every
// loaded entity owned by a cleanup roster instead of silently dropping old handles.
enum { GTAV_SCENE_MAX = GTAV_SCENE_MAP_MAX };
static_assert(GTAV_SPAWNED_ROSTER_MAX == GTAV_SCENE_MAP_KIND_MAX, "map rows per kind = roster cap");
typedef GtavSceneMapEntry SceneEntry;

static SceneEntry g_scene[GTAV_SCENE_MAX];
static time_t g_scene_deadline[GTAV_SCENE_MAX];  // per-entity model-stream deadline (first step)
static int g_scene_count = 0;
static int g_scene_mode = GTAV_SCENE_MAP_ABSOLUTE;
static volatile uint32_t g_scene_load_active = 0;
static int g_scene_anchor_ready = 0;
static Vec3 g_scene_anchor = {};
static float g_scene_anchor_heading = 0.0f;
static int g_scene_loaded_count = 0;
static int g_scene_skipped_count = 0;
static int g_scene_pack_loaded_count = 0;  // created entities whose row names a pack

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
    memset(e, 0, sizeof(*e));
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
    // A getter that returned garbage (a dying entity) is not saved: the file stays loadable.
    if (gtav_scene_map_entry_valid(e, GTAV_SCENE_MAP_ABSOLUTE)) ++g_scene_count;
  }
}

static void scene_remove_temp_file(const char* temp) {
  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  remove(temp);
  if (rooted) gtav_rootdir_leave(&guard);
}

// Shared /data is the sole location for saved and uploaded custom maps.
static void scene_map_dir(char* dir, size_t size) {
  snprintf(dir, size, "%s/maps", gtav_custom_game_root());
}

static uint32_t save_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_MAP;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE))
    return unavailable(action, "cancel the active map load first");
  if (!g_n.get_entity_coords || !g_n.get_entity_rotation || !g_n.get_entity_model) {
    return unavailable(action, "missing entity getter natives");
  }
  g_scene_count = 0;
  scene_capture_roster(GTAV_SCENE_MAP_VEHICLE, g_spawned_vehicle_roster, g_spawned_vehicle_count,
                       NULL);
  scene_capture_roster(GTAV_SCENE_MAP_PED, g_spawned_ped_roster, g_spawned_ped_count, NULL);
  scene_capture_roster(GTAV_SCENE_MAP_OBJECT, g_spawned_object_roster, g_spawned_object_count,
                       g_spawned_object_model);
  if (g_scene_count == 0) return unavailable(action, "no spawned entities to save");

  char dir[96];
  char path[112];
  char temp[120];
  scene_map_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/active.map.cfg", dir);
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  GtavRootdirGuard guard;
  int rooted = gtav_rootdir_enter(&guard) == 0;
  // Only children of an already accessible /data may be created.
  mkdir("/data/gtavmenu", 0777);
  mkdir(GTAV_CUSTOM_GAME_ROOT, 0777);
  mkdir(dir, 0777);
  FILE* fp = fopen(temp, "w");
  if (rooted) gtav_rootdir_leave(&guard);
  if (!fp) return failed(action, "could not open temporary map file");
  // Per pack: entities saved and the first model name (pack-notes lines below).
  const char* packs[GTAV_SCENE_MAP_PACK_MAX] = {};
  const char* firsts[GTAV_SCENE_MAP_PACK_MAX] = {};
  int pack_rows[GTAV_SCENE_MAP_PACK_MAX] = {};
  int pack_count = 0;
  int pack_entities = 0;
  int write_failed = fprintf(fp, "%s\n", GTAV_SCENE_MAP_HEADER_ABSOLUTE) < 0;
  for (int i = 0; i < g_scene_count && !write_failed; ++i) {
    const SceneEntry* e = &g_scene[i];
    const char* pack_id = nullptr;
    const char* name = nullptr;
    char row[GTAV_SCENE_MAP_LINE_MAX];
    int length = -1;
    if (scene_pack_model(e->model, &pack_id, &name, nullptr)) {
      length = gtav_scene_map_format_row(row, sizeof(row), e, pack_id, name);
      int p = 0;
      while (p < pack_count && strcmp(packs[p], pack_id) != 0) ++p;
      if (length > 0 && p < GTAV_SCENE_MAP_PACK_MAX) {
        if (p == pack_count) {
          packs[p] = pack_id;
          firsts[p] = name;
          ++pack_count;
        }
        ++pack_rows[p];
        ++pack_entities;
      } else {
        length = -1;  // more packs than a map may name (cannot happen: 8 packs load at most)
      }
    } else {
      length = gtav_scene_map_format_row(row, sizeof(row), e, nullptr, nullptr);
    }
    write_failed = length <= 0 || fputs(row, fp) < 0;
  }
  if (fflush(fp) != 0 || ferror(fp)) write_failed = 1;
  if (fclose(fp) != 0) write_failed = 1;
  if (write_failed) {
    scene_remove_temp_file(temp);
    return failed(action, "map write failed");
  }
  rooted = gtav_rootdir_enter(&guard) == 0;
  int renamed = rename(temp, path);
  if (rooted) gtav_rootdir_leave(&guard);
  if (renamed != 0) {
    scene_remove_temp_file(temp);
    return failed(action, "could not publish map file");
  }
  placement_note("pack map saved entities=%d pack_entities=%d packs=%d file=%s", g_scene_count,
                 pack_entities, pack_count, path);
  for (int p = 0; p < pack_count; ++p)
    placement_note("pack map saved pack=%s entities=%d first=%s", packs[p], pack_rows[p],
                   firsts[p]);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "saved %d entities (%d from packs)", g_scene_count, pack_entities);
  return ok(action, msg);
}

static SceneEntry scene_world_entry(const SceneEntry* source) {
  SceneEntry world = *source;
  if (g_scene_mode == GTAV_SCENE_MAP_RELATIVE) {
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
  if (world.kind == GTAV_SCENE_MAP_VEHICLE && g_n.create_vehicle) {
    handle = invoke_return_ctx<int>(0, g_n.create_vehicle, (int)world.model, world.x, world.y,
                                    world.z, world.rz, 0, 0);
  } else if (world.kind == GTAV_SCENE_MAP_PED && g_n.create_ped) {
    handle = invoke_return_ctx<int>(0, g_n.create_ped, 4, (int)world.model, world.x, world.y,
                                    world.z, world.rz, 0, 0);
  } else if (world.kind == GTAV_SCENE_MAP_OBJECT && g_n.create_object) {
    handle = invoke_return_ctx<int>(0, g_n.create_object, (int)world.model, world.x, world.y,
                                    world.z, 0, 1, 1);
  }
  if (handle <= 0) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(0, g_n.set_model_as_no_longer_needed, (int)world.model);
    return 0;
  }
  if ((world.flags & GTAV_SCENE_MAP_GROUND) && world.kind == GTAV_SCENE_MAP_OBJECT &&
      g_n.place_object_on_ground_properly)
    invoke_void_ctx(0, g_n.place_object_on_ground_properly, handle);
  // Restore the full 3-axis rotation (CREATE_* only takes a heading; objects take none).
  if (g_n.set_entity_rotation)
    invoke_void_ctx(0, g_n.set_entity_rotation, handle, world.rx, world.ry, world.rz, 2, 1);
  if ((world.flags & GTAV_SCENE_MAP_FROZEN) && g_n.freeze_entity_position)
    invoke_void_ctx(0, g_n.freeze_entity_position, handle, 1);
  if (g_n.set_entity_as_mission_entity)
    invoke_void_ctx(0, g_n.set_entity_as_mission_entity, handle, 1, 1);
  if (world.kind == GTAV_SCENE_MAP_VEHICLE) {
    track_spawned_vehicle(handle);
  } else if (world.kind == GTAV_SCENE_MAP_PED) {
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
  placement_note("pack map loaded ok=%d skipped=%d pack_ok=%d", g_scene_loaded_count,
                 g_scene_skipped_count, g_scene_pack_loaded_count);
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
  placement_note("pack map entry %u skipped: %s model=0x%08x", idx, reason,
                 (unsigned)g_scene[idx].model);
  return load_map_chain_next(idx);
}

// Game-thread per-entity load step: validate and stream the model, create at the resolved
// transform, and chain on. Self-requeues this same index while the model streams.
static uint32_t load_map_step(uint32_t idx) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (!__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE)) return GTAV_FEATURE_RESULT_NONE;
  if ((int)idx >= g_scene_count) return scene_finish_load();
  if (g_scene_mode == GTAV_SCENE_MAP_RELATIVE && !g_scene_anchor_ready) {
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
  if (g_scene_deadline[idx] == 0)
    g_scene_deadline[idx] = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
  invoke_void_ctx(0, g_n.request_model, (int)e->model);
  if (!invoke_return_ctx<int>(0, g_n.has_model_loaded, (int)e->model)) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (time(NULL) < g_scene_deadline[idx] &&
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
    if (e->pack) ++g_scene_pack_loaded_count;
  } else {
    ++g_scene_skipped_count;
    placement_note("pack map entry %u skipped: create failed model=0x%08x", idx,
                   (unsigned)e->model);
  }
  return load_map_chain_next(idx);
}

static uint32_t cancel_map_load() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CANCEL_MAP_LOAD;
  if (!__atomic_exchange_n(&g_scene_load_active, 0u, __ATOMIC_ACQ_REL))
    return unavailable(action, "no active map load");
  return ok(action, "map load cancelled");
}

// The map file Load Map reads: the newer of the in-game save (<custom root>/maps, scene_map_dir)
// and `menu-ctl.sh map-upload`'s console copy (GTAV_CUSTOM_MAP_PATH; a different directory on the
// case-sensitive /data), else the legacy path. Opens it for reading; its path goes to `path`.
static FILE* scene_open_map(char* path, size_t size) {
  char saved[112];
  char dir[96];
  scene_map_dir(dir, sizeof(dir));
  snprintf(saved, sizeof(saved), "%s/active.map.cfg", dir);
  const char* candidates[3] = {saved, GTAV_CUSTOM_MAP_PATH, GTAV_LEGACY_MAP_PATH};
  GtavRootdirGuard guard;
  const int rooted = gtav_rootdir_enter(&guard) == 0;
  int pick = -1;
  time_t newest = 0;
  for (int i = 0; i < 2; ++i) {
    struct stat st;
    if (stat(candidates[i], &st) != 0 || !S_ISREG(st.st_mode)) continue;
    if (pick < 0 || st.st_mtime > newest) {
      pick = i;
      newest = st.st_mtime;
    }
  }
  if (pick < 0) pick = 2;
  FILE* fp = fopen(candidates[pick], "r");
  if (rooted) gtav_rootdir_leave(&guard);
  snprintf(path, size, "%s", candidates[pick]);
  return fp;
}

// Worker-direct: parse the complete map into a private staging map, check that every pack its rows
// name is loaded, then atomically publish the scene and enqueue its first game-thread step.
// Malformed files never partially replace g_scene; a missing pack refuses before anything spawns.
static uint32_t load_map() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_MAP;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (__atomic_load_n(&g_scene_load_active, __ATOMIC_ACQUIRE))
    return unavailable(action, "map load already active");
  char path[112];
  FILE* fp = scene_open_map(path, sizeof(path));
  if (!fp) return unavailable(action, "no saved/custom map");

  GtavSceneMap parsed;  // ~4.6 KiB of worker stack, as the staging array before it
  gtav_scene_map_init(&parsed);
  int invalid_line = 0;
  int line_number = 0;
  char line[GTAV_SCENE_MAP_LINE_MAX];
  while (fgets(line, sizeof(line), fp)) {
    ++line_number;
    // A line longer than the buffer (no newline before EOF) is invalid, never split in two.
    if ((!strchr(line, '\n') && !feof(fp)) || gtav_scene_map_feed(&parsed, line) != 0) {
      invalid_line = line_number;
      break;
    }
  }
  int read_failed = ferror(fp);
  fclose(fp);
  if (read_failed || invalid_line) {
    char msg[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(msg, sizeof(msg), "invalid map at line %d", invalid_line ? invalid_line : line_number);
    placement_note("pack map load invalid line=%d file=%s",
                   invalid_line ? invalid_line : line_number, path);
    return failed(action, msg);
  }
  const int parsed_count = parsed.count;
  if (parsed_count == 0) return unavailable(action, "map is empty");
  // Pack rows: the model must be a spawn row of a loaded pack (any pack: a renamed pack version
  // with the same model still serves). Otherwise refuse, naming the pack the save came from.
  for (int i = 0; i < parsed_count; ++i) {
    const SceneEntry* e = &parsed.entries[i];
    if (!e->pack || e->pack > parsed.pack_count) continue;
    int ready = 0;
    if (scene_pack_model(e->model, nullptr, nullptr, &ready) && ready) continue;
    const int p = e->pack - 1;
    placement_note("pack map load refused pack=%s model=%s entities=%d: load the pack first",
                   parsed.pack_ids[p], parsed.pack_models[p], parsed.pack_rows[p]);
    char msg[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(msg, sizeof(msg), "Load pack %.44s first", parsed.pack_ids[p]);
    return unavailable(action, msg);
  }
  const int* kind_counts = parsed.kind_counts;
  if ((kind_counts[GTAV_SCENE_MAP_VEHICLE] && !g_n.create_vehicle) ||
      (kind_counts[GTAV_SCENE_MAP_PED] && !g_n.create_ped) ||
      (kind_counts[GTAV_SCENE_MAP_OBJECT] && !g_n.create_object))
    return unavailable(action, "missing map entity creation native");
  if (!g_n.request_model || !g_n.has_model_loaded || !g_n.is_model_valid ||
      !g_n.is_model_in_cdimage)
    return unavailable(action, "missing map validation/streaming native");
  if (!g_n.set_entity_rotation || !g_n.set_entity_as_mission_entity)
    return unavailable(action, "missing map transform/ownership native");
  int pack_entities = 0;
  for (int i = 0; i < parsed_count; ++i) {
    pack_entities += parsed.entries[i].pack != 0;
    if ((parsed.entries[i].flags & GTAV_SCENE_MAP_FROZEN) && !g_n.freeze_entity_position)
      return unavailable(action, "map needs freeze native");
    if ((parsed.entries[i].flags & GTAV_SCENE_MAP_GROUND) && !g_n.place_object_on_ground_properly)
      return unavailable(action, "map needs ground-placement native");
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  for (int i = 0; i < parsed_count; ++i) {
    g_scene[i] = parsed.entries[i];
    g_scene_deadline[i] = 0;
  }
  g_scene_count = parsed_count;
  g_scene_mode = parsed.mode;
  g_scene_anchor_ready = 0;
  g_scene_loaded_count = 0;
  g_scene_skipped_count = 0;
  g_scene_pack_loaded_count = 0;
  __atomic_store_n(&g_scene_load_active, 1u, __ATOMIC_RELEASE);
  if (!gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP, 0)) {
    __atomic_store_n(&g_scene_load_active, 0u, __ATOMIC_RELEASE);
    return failed(action, "main-thread job queue full");
  }
  placement_note("pack map load entities=%d pack_entities=%d packs=%d version=%d file=%s",
                 parsed_count, pack_entities, parsed.pack_count, parsed.version, path);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "loading %d %s entities...", g_scene_count,
           parsed.mode == GTAV_SCENE_MAP_RELATIVE ? "relative" : "absolute");
  return ok(action, msg);
#else
  (void)pack_entities;
  return unavailable(action, "needs main-thread hook");
#endif
}
