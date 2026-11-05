// Source fragment of features.cpp, textually included by custom_rpf.inc in place of
// custom_pack_data.inc and custom_pack_world.inc when GTAV_MENU_CUSTOM_PACK_ENGINE is 0.
//
// Engine-less pack lane (a target without a custom lane): no GTAV_LANE_* address exists, no
// engine object is read and no SDK call is made. The descriptors and the selection use platform
// file IO (custom_rpf.inc pack_read_local, custom_pack_select.inc). The rows that work are natives
// only: `place` teleports, stock `hide` rows, and `spawn timecycle` / `spawn ptfx` rows that name
// stock assets. Every step that registers or loads into the engine refuses with
// GTAV_CUSTOM_PACK_ENGINE_REASON before it touches anything (custom_rpf.inc, after the sections).

// A pack event for the status ring and the klog (custom_pack_world.inc's helper).
static void pack_world_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void pack_world_note(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, text);
  char line[sizeof(text) + 16];
  snprintf(line, sizeof(line), "GTAVMenu %s", text);
  gtav_worker_klog(line);
}

// No pack interiors: teleports never wait for interior collision.
__attribute__((unused)) static void pack_interior_diag_tick() {}

__attribute__((unused)) static int pack_interior_collision_ready() {
  return 1;
}

// ---- Stock screen effects ----

// The modifier this lane applied last (joaat of its name), 0 when none or after it was cleared.
static uint32_t g_pack_tc_applied;

// Custom Packs `spawn timecycle` row (param = joaat of the modifier name), main thread: apply the
// named modifier at full strength, or clear it when this row applied it last. Without the engine
// the modifier table is not read, so only stock modifiers apply, and the toggle follows the last
// row applied here rather than the game's current index.
static uint32_t apply_pack_timecycle(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_APPLY_PACK_TIMECYCLE;
  if (!g_n.set_timecycle_modifier || !g_n.clear_timecycle_modifier)
    return unavailable(action, "missing timecycle natives");
  const char* name = nullptr;
  if (__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE))
    for (int i = 0; i < g_pack.spawn_count && !name; ++i)
      if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE &&
          pack_joaat(g_pack.spawns[i].model) == param)
        name = g_pack.spawns[i].model;
  if (!name) return unavailable(action, "not a screen effect of the loaded packs");
  const int clear = g_pack_tc_applied == param;
  if (clear) {
    invoke_void(g_n.clear_timecycle_modifier);
    g_pack_tc_applied = 0;
  } else {
    invoke_void(g_n.set_timecycle_modifier, name);
    if (g_n.set_timecycle_modifier_strength) invoke_void(g_n.set_timecycle_modifier_strength, 1.0f);
    g_pack_tc_applied = param;
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "screen effect %.32s %s", name, clear ? "off" : "on");
  pack_world_note("pack tc %s 0x%08x", message, (unsigned)param);
  return ok(action, message);
}

// ---- Stock particle effects ----

// `spawn ptfx <asset>:<effect>` row whose model text hashes to `param`: copies the two names.
static int pack_ptfx_row(uint32_t param, char* asset, char* effect) {
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.spawn_count; ++i) {
    const char* model = g_pack.spawns[i].model;
    const char* colon = strchr(model, ':');
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_PTFX || !colon ||
        pack_joaat(model) != param)
      continue;
    snprintf(asset, GTAV_CUSTOM_PACK_NAME_MAX, "%.*s", (int)(colon - model), model);
    snprintf(effect, GTAV_CUSTOM_PACK_NAME_MAX, "%s", colon + 1);
    return 1;
  }
  return 0;
}

// Custom Packs `spawn ptfx` row (param = joaat of "<asset>:<effect>"), main thread: request the
// particle dictionary, re-queue on the game thread until it is loaded (bounded), then play the
// effect once at the player's feet. The Fireworks job's natives; without the engine the store is
// not read, so only stock dictionaries load and a wrong name ends in the load timeout.
static uint32_t play_pack_ptfx(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PLAY_PACK_PTFX;
  static time_t deadline = 0;
  static uint32_t pending = 0;
  if (!g_n.request_named_ptfx_asset || !g_n.has_named_ptfx_asset_loaded ||
      !g_n.use_particle_fx_asset || !g_n.start_particle_fx_non_looped_at_coord ||
      !g_n.get_entity_coords || !g_n.player_ped_id) {
    deadline = 0;
    return unavailable(action, "missing particle natives");
  }
  char asset[GTAV_CUSTOM_PACK_NAME_MAX], effect[GTAV_CUSTOM_PACK_NAME_MAX];
  if (!pack_ptfx_row(param, asset, effect)) {
    deadline = 0;
    return unavailable(action, "not a particle effect of the loaded packs");
  }
  const int first = deadline == 0 || pending != param;
  invoke_void(g_n.request_named_ptfx_asset, asset);
  if (!invoke_return<int>(g_n.has_named_ptfx_asset_loaded, asset)) {
    if (first) {
      pending = param;
      deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
    }
    if (time(NULL) < deadline && gtav_frame_hook_enqueue(action, param))
      return ok(action, "pack effect loading");
    pack_world_note("pack ptfx load timeout %s", asset);
    deadline = 0;
    return failed(action, "pack effect did not load (see klog)");
  }
  deadline = 0;
  const int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  const Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.use_particle_fx_asset, asset);
  // White tint: some effects render near-black without one (custom_pack_data.inc).
  if (g_n.set_particle_fx_non_looped_colour)
    invoke_void(g_n.set_particle_fx_non_looped_colour, 1.0f, 1.0f, 1.0f);
  // Ped coordinates are about 1 m above the ground; the effect starts at the feet.
  const int started = invoke_return<int>(g_n.start_particle_fx_non_looped_at_coord, effect, c.x,
                                         c.y, c.z - 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "effect %.40s %s", effect,
           started ? "started" : "start returned 0");
  return started ? ok(action, message) : failed(action, message);
}
