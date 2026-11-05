// Tracked spawned-entity cleanup.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace.
// Tracks only entities created by this menu and deletes them from game-thread actions.

enum { GTAV_SPAWNED_ROSTER_MAX = 32 };

static int g_spawned_vehicle_roster[GTAV_SPAWNED_ROSTER_MAX];
static int g_spawned_ped_roster[GTAV_SPAWNED_ROSTER_MAX];
static int g_spawned_object_roster[GTAV_SPAWNED_ROSTER_MAX];
// Model hash parallel to g_spawned_object_roster (same index), so the interactive move mode can
// duplicate a placed prop (CREATE_OBJECT needs the hash; GET_ENTITY_MODEL is not resolved). Kept
// in lockstep by track_spawned_object_model() below.
static int g_spawned_object_model[GTAV_SPAWNED_ROSTER_MAX];
static int g_spawned_vehicle_count = 0;
static int g_spawned_ped_count = 0;
static int g_spawned_object_count = 0;

static int roster_handle_alive(int handle) {
  if (handle <= 0) return 0;
  if (g_n.does_entity_exist && !invoke_return<int>(g_n.does_entity_exist, handle)) return 0;
  return 1;
}

// Drops handles the game no longer has (keeping `parallel`, when given, in lockstep) and returns
// the free slots left.
static int spawned_roster_compact(int* roster, int* parallel, int* count) {
  int out = 0;
  for (int i = 0; i < *count; ++i) {
    if (!roster_handle_alive(roster[i])) continue;
    if (parallel) parallel[out] = parallel[i];
    roster[out++] = roster[i];
  }
  *count = out;
  return GTAV_SPAWNED_ROSTER_MAX - out;
}

static const char kSpawnedVehiclesFull[] = "Spawned vehicle limit reached; clear spawned vehicles";
static const char kSpawnedPedsFull[] = "Spawned ped limit reached; clear spawned peds";
static const char kSpawnedObjectsFull[] = "Spawned object limit reached; clear spawned objects";

// Free slots per kind. Every spawn path asks before it creates anything, so a full roster refuses
// the spawn instead of forgetting a live entity that Clear could then never delete.
static int spawned_vehicle_room() {
  return spawned_roster_compact(g_spawned_vehicle_roster, nullptr, &g_spawned_vehicle_count);
}
static int spawned_ped_room() {
  return spawned_roster_compact(g_spawned_ped_roster, nullptr, &g_spawned_ped_count);
}
static int spawned_object_room() {
  return spawned_roster_compact(g_spawned_object_roster, g_spawned_object_model,
                                &g_spawned_object_count);
}

// Tracks `handle` (and its model, for objects). A tracked entity is never displaced: on a full
// roster the new handle stays untracked, which a caller that checked the room never meets.
static void spawned_roster_add(int* roster, int* parallel, int* count, int handle, int value) {
  if (handle <= 0 || !roster || !count) return;
  for (int i = 0; i < *count; ++i) {
    if (roster[i] != handle) continue;
    if (parallel) parallel[i] = value;
    return;
  }
  if (spawned_roster_compact(roster, parallel, count) <= 0) return;
  if (parallel) parallel[*count] = value;
  roster[(*count)++] = handle;
}

static void track_spawned_vehicle(int handle) {
  spawned_roster_add(g_spawned_vehicle_roster, nullptr, &g_spawned_vehicle_count, handle, 0);
}
static void track_spawned_ped(int handle) {
  spawned_roster_add(g_spawned_ped_roster, nullptr, &g_spawned_ped_count, handle, 0);
}
// Objects also record the model hash (parallel array) so the move mode can duplicate them.
static void track_spawned_object_model(int handle, int model) {
  spawned_roster_add(g_spawned_object_roster, g_spawned_object_model, &g_spawned_object_count,
                     handle, model);
}

// Model hash recorded for a live object handle (0 if unknown / not menu-spawned). Used only by
// the frame-hook move driver (duplicate), so unused in non-frame-hook builds.
[[maybe_unused]] static int spawned_object_model_of(int handle) {
  if (handle <= 0) return 0;
  for (int i = 0; i < g_spawned_object_count; ++i) {
    if (g_spawned_object_roster[i] == handle) return g_spawned_object_model[i];
  }
  return 0;
}

static int delete_spawned_roster(int* roster, int* count, int use_delete_ped) {
  if (!roster || !count) return 0;
  if (use_delete_ped) {
    if (!g_n.delete_ped) return -1;
  } else if (!g_n.delete_entity) {
    return -1;
  }

  int deleted = 0;
  for (int i = 0; i < *count; ++i) {
    int handle = roster[i];
    if (!roster_handle_alive(handle)) continue;
    if (g_n.set_entity_as_mission_entity) invoke_void(g_n.set_entity_as_mission_entity, handle, 1, 1);
    if (use_delete_ped) {
      invoke_void(g_n.delete_ped, (void*)&handle);
    } else {
      invoke_void(g_n.delete_entity, &handle);
    }
    ++deleted;
  }
  memset(roster, 0, sizeof(roster[0]) * GTAV_SPAWNED_ROSTER_MAX);
  *count = 0;
  return deleted;
}

static uint32_t clear_spawned_vehicles() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES;
  int deleted = delete_spawned_roster(g_spawned_vehicle_roster, &g_spawned_vehicle_count, 0);
  if (deleted < 0) return unavailable(action, "missing delete native");
  if (g_last_spawned_vehicle && !roster_handle_alive(g_last_spawned_vehicle)) g_last_spawned_vehicle = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "deleted %d spawned vehicles", deleted);
  return ok(action, msg);
}

static uint32_t clear_spawned_peds() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS;
  int deleted = delete_spawned_roster(g_spawned_ped_roster, &g_spawned_ped_count, 1);
  if (deleted < 0) return unavailable(action, "missing delete ped native");
  deleted += crowd_clear(0);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "deleted %d spawned peds", deleted);
  return ok(action, msg);
}

static uint32_t clear_spawned_objects() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS;
  int deleted = delete_spawned_roster(g_spawned_object_roster, &g_spawned_object_count, 0);
  if (deleted < 0) return unavailable(action, "missing delete native");
  if (g_last_spawned_object && !roster_handle_alive(g_last_spawned_object)) g_last_spawned_object = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "deleted %d spawned objects", deleted);
  return ok(action, msg);
}

[[maybe_unused]] static uint32_t clear_spawned_all() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL;
  int vehicles = delete_spawned_roster(g_spawned_vehicle_roster, &g_spawned_vehicle_count, 0);
  int peds = delete_spawned_roster(g_spawned_ped_roster, &g_spawned_ped_count, 1);
  int objects = delete_spawned_roster(g_spawned_object_roster, &g_spawned_object_count, 0);
  if (vehicles < 0 || objects < 0) return unavailable(action, "missing delete native");
  if (peds < 0) return unavailable(action, "missing delete ped native");
  peds += crowd_clear(0);
  g_last_spawned_vehicle = 0;
  g_last_spawned_object = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "deleted %d spawned entities", vehicles + peds + objects);
  return ok(action, msg);
}
