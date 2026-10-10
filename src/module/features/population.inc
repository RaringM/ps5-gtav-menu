// Ambient budgets and separately owned crowd peds. All native work runs on the game thread.
static const char kPopulationLabels[][8] = {"Thin", "Normal", "Dense", "Max"};
enum { kCrowdLimit = 40, kCrowdBatch = 16, kCrowdStreamMs = 8000 };
// Small reviewed set from the fork's ambient crowd implementation. Keeping this
// independent of the full ped browser avoids selecting story or special models.
static const uint32_t kCrowdModels[] = {
    0xc79f6928u, 0x445ac854u, 0x2799efd8u, 0x5c2cf7f8u, 0xd1feb884u,
    0x76284640u, 0xc99f21c4u, 0x2307a353u, 0x14d506eeu, 0x8674d5fcu,
};
struct CrowdPed {
  int handle;
  int automatic;
};
static CrowdPed g_crowd[kCrowdLimit];
static uint32_t g_crowd_count;
static uint32_t g_crowd_pending;
static uint32_t g_crowd_model;
static uint32_t g_crowd_deadline;
static uint32_t g_crowd_serial;
static uint32_t g_crowd_next_auto;
static uint32_t g_crowd_failures;
static int g_maintain_crowd;
static int g_crowd_stop;
static int g_crowd_busy;

static void crowd_release_model() {
  if (g_crowd_model && g_n.set_model_as_no_longer_needed)
    invoke_void(g_n.set_model_as_no_longer_needed, g_crowd_model);
  g_crowd_model = 0;
}

// Worker-side cancellation publishes intent only; the game thread performs teardown.
static void crowd_request_stop(int all = 0) {
  __atomic_store_n(&g_maintain_crowd, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_crowd_stop, all ? 2 : 1, __ATOMIC_RELEASE);
}

static int crowd_clear(int automatic_only) {
  __atomic_store_n(&g_maintain_crowd, 0, __ATOMIC_RELEASE);
  g_crowd_pending = 0;
  g_crowd_failures = 0;
  crowd_release_model();
  int deleted = 0;
  uint32_t out = 0;
  for (uint32_t i = 0; i < g_crowd_count; ++i) {
    CrowdPed entry = g_crowd[i];
    const int alive = entry.handle > 0 &&
        (!g_n.does_entity_exist || invoke_return<int>(g_n.does_entity_exist, entry.handle));
    if (!alive) continue;
    if ((!automatic_only || entry.automatic) && g_n.delete_ped) {
      invoke_void(g_n.delete_ped, &entry.handle);
      ++deleted;
    } else {
      g_crowd[out++] = entry;
    }
  }
  g_crowd_count = out;
  __atomic_store_n(&g_crowd_busy, out != 0, __ATOMIC_RELEASE);
  return deleted;
}

static int crowd_natives_ready() {
  return g_n.player_ped_id && g_n.get_entity_coords && g_n.get_frame_count && g_n.get_game_timer &&
      g_n.request_model && g_n.has_model_loaded && g_n.set_model_as_no_longer_needed &&
      g_n.create_ped && g_n.delete_ped && g_n.does_entity_exist && g_n.get_ground_z_for_3d_coord;
}

static uint32_t start_crowd() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_CROWD;
  if (!crowd_natives_ready()) return unavailable(action, "missing crowd natives");
  if (native_player_ped_id() <= 0) return failed(action, "no valid player ped");
  uint32_t room = kCrowdLimit - g_crowd_count;
  if (g_crowd_pending >= room) return failed(action, "crowd limit reached");
  uint32_t add = room - g_crowd_pending;
  if (add > kCrowdBatch) add = kCrowdBatch;
  g_crowd_pending += add;
  g_crowd_failures = 0;
  __atomic_store_n(&g_crowd_stop, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_crowd_busy, 1, __ATOMIC_RELEASE);
  return ok(action, "crowd queued");
}

static uint32_t toggle_maintain_crowd() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_MAINTAIN_CROWD;
  if (!crowd_natives_ready()) return unavailable(action, "missing crowd natives");
  if (__atomic_load_n(&g_maintain_crowd, __ATOMIC_ACQUIRE)) {
    crowd_clear(1);
    return ok(action, "maintain crowd disabled");
  }
  __atomic_store_n(&g_maintain_crowd, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_crowd_stop, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_crowd_busy, 1, __ATOMIC_RELEASE);
  g_crowd_next_auto = 0;
  g_crowd_failures = 0;
  return ok(action, "maintain crowd enabled");
}

[[maybe_unused]] static void crowd_game_thread_tick() {
  int stop = __atomic_load_n(&g_crowd_stop, __ATOMIC_ACQUIRE);
  if (stop) {
    crowd_clear(stop == 1);
    __atomic_store_n(&g_crowd_stop, 0, __ATOMIC_RELEASE);
  }
  const int maintain = __atomic_load_n(&g_maintain_crowd, __ATOMIC_ACQUIRE);
  if (!g_crowd_pending && !maintain) return;
  if (!crowd_natives_ready()) {
    crowd_clear(1);
    return;
  }
  static uint32_t last_frame = UINT32_MAX;
  uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
  if (frame == last_frame) return;
  last_frame = frame;
  uint32_t now = invoke_return<uint32_t>(g_n.get_game_timer);
  uint32_t out = 0;
  for (uint32_t i = 0; i < g_crowd_count; ++i) {
    if (invoke_return<int>(g_n.does_entity_exist, g_crowd[i].handle)) g_crowd[out++] = g_crowd[i];
  }
  g_crowd_count = out;
  if (out >= kCrowdLimit) {
    g_crowd_pending = 0;
    crowd_release_model();
    return;
  }
  const int automatic = g_crowd_pending == 0;
  if (automatic && (int32_t)(now - g_crowd_next_auto) < 0) return;
  int ped = native_player_ped_id();
  if (ped <= 0) {
    crowd_clear(1);
    return;
  }
  // Use reviewed hashes rather than runtime model-name hashing.
  if (!g_crowd_model) {
    const uint32_t model_count = (uint32_t)(sizeof(kCrowdModels) / sizeof(kCrowdModels[0]));
    g_crowd_model = kCrowdModels[g_crowd_serial % model_count];
    ++g_crowd_serial;
    if ((g_n.is_model_valid && !invoke_return<int>(g_n.is_model_valid, g_crowd_model)) ||
        (g_n.is_model_in_cdimage && !invoke_return<int>(g_n.is_model_in_cdimage, g_crowd_model))) {
      crowd_release_model();
      ++g_crowd_failures;
    } else {
      invoke_void(g_n.request_model, g_crowd_model);
      g_crowd_deadline = now + kCrowdStreamMs;
    }
  }
  if (g_crowd_failures >= 10u) {
    crowd_clear(1);
    ok(GTAV_NATIVE_SHELL_ACTION_SPAWN_CROWD, "crowd stopped: models or ground unavailable");
    return;
  }
  if (!g_crowd_model) { g_crowd_next_auto = now + 333u; return; }
  if (!invoke_return<int>(g_n.has_model_loaded, g_crowd_model)) {
    if ((int32_t)(now - g_crowd_deadline) >= 0) {
      crowd_release_model();
      ++g_crowd_failures;
      g_crowd_next_auto = now + 333u;
    }
    return;
  }
  Vec3 anchor = invoke_vector(g_n.get_entity_coords, ped, 1);
  // At most two creation attempts per rendered game frame. Failed ground probes consume attempts.
  for (uint32_t attempt = 0; attempt < 2 && g_crowd_count < kCrowdLimit; ++attempt) {
    if (!automatic && !g_crowd_pending) break;
    uint32_t slot = g_crowd_serial++;
    float angle = (float)(slot % 16u) * (6.2831853f / 16.0f);
    float radius = automatic ? 18.0f + (float)(slot % 3u) * 5.0f : 7.0f;
    float x = anchor.x + cosf(angle) * radius;
    float y = anchor.y + sinf(angle) * radius;
    float z = 0.0f;
    if (!invoke_return<int>(g_n.get_ground_z_for_3d_coord, x, y, anchor.z + 10.0f, &z, 0, 0) ||
        !__builtin_isfinite(z) || fabsf(z - anchor.z) > 10.0f) {
      ++g_crowd_failures;
      continue;
    }
    int handle = invoke_return<int>(g_n.create_ped, 4, g_crowd_model, x, y, z + 0.2f, 0.0f, 0, 0);
    if (!handle) { ++g_crowd_failures; continue; }
    if (g_n.set_entity_as_mission_entity) invoke_void(g_n.set_entity_as_mission_entity, handle, 1, 1);
    if (g_n.task_wander_standard) invoke_void(g_n.task_wander_standard, handle, 10.0f, 10);
    g_crowd[g_crowd_count++] = {handle, automatic};
    if (!automatic) --g_crowd_pending;
    g_crowd_failures = 0;
  }
  crowd_release_model();
  g_crowd_next_auto = now + 333u;
  __atomic_store_n(&g_crowd_busy, g_crowd_count != 0 || g_crowd_pending || maintain, __ATOMIC_RELEASE);
}
