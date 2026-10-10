// Streaming-aware teleport, saved location, and presets
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

// Begin a streaming-aware teleport of the player (or their vehicle when driving, so
// car + player move together) to x,y,z. A naive SET_ENTITY_COORDS to a far, un-
// streamed spot crashes once the game thread trips over the player sitting in void;
// the fix is to FREEZE the entity first (so physics can't fault), request collision
// at the destination, and hand off to the worker tick to poll until the cell streams
// in -- then release. Non-blocking: returns immediately, the tick does the rest.
static uint32_t start_teleport(uint32_t action, float x, float y, float z, const char* dest,
                               int clamp) {
  if (!g_n.set_entity_coords || !g_n.player_ped_id || !g_n.freeze_entity_position ||
      !g_n.request_collision_at_coord || !g_n.has_collision_loaded_around_entity) {
    return unavailable(action, "missing teleport natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  int entity = ped;
  if (g_n.is_ped_in_any_vehicle && g_n.get_vehicle_ped_is_in &&
      invoke_return<int>(g_n.is_ped_in_any_vehicle, ped, 0)) {
    int v = current_vehicle();
    if (v) entity = v;
  }
  // Freeze, then move; lift slightly so the entity settles down onto the streamed
  // ground when we unfreeze. SET_ENTITY_COORDS(entity, x,y,z, xAxis,yAxis,zAxis,
  // clearArea); clearArea=0 -- the freeze + collision request handle streaming.
  invoke_void(g_n.freeze_entity_position, entity, 1);
  const float lift = clamp == GTAV_TP_CLAMP_INTERIOR ? 0.0f : 2.0f;
  invoke_void(g_n.set_entity_coords, entity, x, y, z + lift, 0, 0, 0, 0);
  invoke_void(g_n.request_collision_at_coord, x, y, z);
  g_tp_entity = entity;
  g_tp_is_vehicle = (entity != ped) ? 1 : 0;
  g_tp_clamp = clamp;
  g_tp_x = x;
  g_tp_y = y;
  g_tp_z = z;
  g_tp_active = 1;
  g_tp_ticks_left = GTAV_TELEPORT_TIMEOUT_TICKS;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "teleporting to %s...", dest ? dest : "destination");
  return ok(action, msg);
}

static uint32_t teleport_to_waypoint() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT;
  if (!g_n.get_first_blip_info_id || !g_n.get_blip_coords || !g_n.get_entity_coords ||
      !g_n.player_ped_id) {
    return unavailable(action, "missing teleport native");
  }
  // Waypoint blips are registered under blip type/sprite id 8 (BLIP_WAYPOINT).
  int blip = invoke_return<int>(g_n.get_first_blip_info_id, 8);
  // A 0 id (or a 0 type, when that getter is available) means no waypoint is set.
  if (blip == 0 ||
      (g_n.get_blip_info_id_type && invoke_return<int>(g_n.get_blip_info_id_type, blip) == 0)) {
    return unavailable(action, "no waypoint set");
  }
  Vec3 wp = invoke_vector_blip(g_n.get_blip_coords, blip);
  // A waypoint only carries x/y. Keep the player's current z as the target height;
  // the freeze/stream/settle then drops them onto the destination ground.
  Vec3 cur = invoke_vector(g_n.get_entity_coords, native_player_ped_id(), 1);
  // SNAP: the waypoint carries no Z, so resolve the real ground at its XY rather than reusing
  // the player's current height (which lands them in the sky or under the destination terrain).
  return start_teleport(action, wp.x, wp.y, cur.z, "waypoint", GTAV_TP_CLAMP_SNAP);
}

// Teleport to the current mission objective / destination marker. Story-mode
// objectives are script-created blips (blip type 1 -- the "custom"/script category,
// distinct from the waypoint's type 8). We scan a short, tunable candidate list and
// take the first blip that resolves to real coordinates. Same streaming-aware,
// worker-safe path as teleport_to_waypoint(); the exact objective blip type(s) may
// need live tuning, so they live in one small array.
static uint32_t teleport_to_objective() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE;
  if (!g_n.get_first_blip_info_id || !g_n.get_blip_coords || !g_n.get_entity_coords ||
      !g_n.player_ped_id) {
    return unavailable(action, "missing teleport native");
  }
  // GET_FIRST_BLIP_INFO_ID filters by blip type. Type 1 carries script-created blips
  // (the yellow objective / route destination markers); skip the waypoint type (8) --
  // that is the separate "To Waypoint" row. Add fallbacks here if live testing needs them.
  static const int kObjectiveBlipTypes[] = {1};
  Vec3 cur = invoke_vector(g_n.get_entity_coords, native_player_ped_id(), 1);
  for (unsigned i = 0; i < sizeof(kObjectiveBlipTypes) / sizeof(kObjectiveBlipTypes[0]); ++i) {
    int blip = invoke_return<int>(g_n.get_first_blip_info_id, kObjectiveBlipTypes[i]);
    if (blip == 0) continue;
    if (g_n.get_blip_info_id_type && invoke_return<int>(g_n.get_blip_info_id_type, blip) == 0) {
      continue;
    }
    Vec3 b = invoke_vector_blip(g_n.get_blip_coords, blip);
    // Reject a (0,0) blip (unset / off-map) so we never fling the player to the map origin.
    // Objective blips carry x/y but often no z, so keep the player's current height and let
    // the freeze/stream/settle drop them onto the destination ground.
    if (b.x == 0.0f && b.y == 0.0f) continue;
    // SNAP: objective blips carry X/Y but no usable Z (see waypoint above) -> resolve ground.
    return start_teleport(action, b.x, b.y, cur.z, "objective", GTAV_TP_CLAMP_SNAP);
  }
  return unavailable(action, "no objective set");
}

// Warp the player into their last-driven vehicle. GET_LAST_DRIVEN_VEHICLE returns the handle;
// SET_PED_INTO_VEHICLE(ped, veh, -1) drops the player into the driver seat. Both walk the
// ped/vehicle managers -> game-thread only, so this runs from run_gated_action (frame-hook drain),
// never worker-direct. Listed in GAME_THREAD_FUNCTIONS (it calls set_ped_into_vehicle).
static uint32_t teleport_last_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TELEPORT_LAST_VEHICLE;
  if (!g_n.get_last_driven_vehicle || !g_n.set_ped_into_vehicle || !g_n.player_ped_id) {
    return unavailable(action, "missing teleport-to-vehicle natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  int veh = invoke_return<int>(g_n.get_last_driven_vehicle);
  if (!veh || (g_n.does_entity_exist && !invoke_return<int>(g_n.does_entity_exist, veh))) {
    return unavailable(action, "no last vehicle");
  }
  // SET_PED_INTO_VEHICLE(ped, vehicle, seat): seat -1 = driver.
  invoke_void(g_n.set_ped_into_vehicle, ped, veh, -1);
  return ok(action, "warped into last vehicle");
}

static uint32_t save_location() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_LOCATION;
  if (!g_n.get_entity_coords || !g_n.player_ped_id) {
    return unavailable(action, "missing location natives");
  }
  int entity = current_player_entity();
  if (!entity) return failed(action, "player entity unavailable");
  g_saved_location = invoke_vector(g_n.get_entity_coords, entity, 1);
  g_saved_location_valid = 1;
  return ok(action, "location saved");
}

static uint32_t return_saved_location() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION;
  if (!g_saved_location_valid) {
    return unavailable(action, "no saved location");
  }
  // FLOOR: keep the exact saved Z (it may be a rooftop / interior), but rescue it onto the
  // ground if it drifted below the terrain -- never lowers an intentional height.
  return start_teleport(action, g_saved_location.x, g_saved_location.y, g_saved_location.z,
                        "saved location", GTAV_TP_CLAMP_FLOOR);
}

// Panic / reset: clear every toggle flag and actively restore the game state the
// per-tick toggles drive, so one press returns the player and world to defaults.
// All restores are modify-existing / global setters -- worker-safe like the toggles
// themselves. Each native call is guarded so a partially-resolved table is safe.
static uint32_t disable_all_features() {
  crowd_request_stop();
  g_population_density_index = 1u;
  g_thin_population_enabled = 0;
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL;

  // Capture whether weapon-damage was actually active BEFORE the flag reset below. The
  // active reset calls SET_PLAYER_WEAPON_DAMAGE_MODIFIER, which is a single-signal
  // (hardware-unverified) native -- it must only run if the user actually enabled the
  // feature, never unconditionally. disable_all_features() runs at startup via
  // import_profile(), where the index is already 0, so this stays 0 there and the native
  // is not touched on the worker thread at init.
  const int had_weapon_damage = (g_weapon_damage_index != 0);

  g_state.god_mode_enabled = 0;
  g_state.super_run_enabled = 0;
  g_state.fast_swim_enabled = 0;
  g_state.infinite_ammo_enabled = 0;
  g_super_jump_enabled = 0;
  g_invisible_enabled = 0;
  g_never_wanted_enabled = 0;
  g_vehicle_god_enabled = 0;
  g_nitro_enabled = 0;
  g_engine_on_enabled = 0;
  g_fast_move_enabled = 0;
  g_no_ragdoll_enabled = 0;
  g_seatbelt_enabled = 0;
  g_keep_clean_enabled = 0;
  g_super_proofs_enabled = 0;
  g_no_crit_enabled = 0;
  g_inf_stamina_enabled = 0;
  g_inf_special_enabled = 0;
  g_low_gravity_enabled = 0;
  g_slow_motion_enabled = 0;
  g_freeze_time_enabled = 0;
  g_auto_heal_enabled = 0;
  g_wanted_lock_enabled = 0;
  g_keep_repaired_enabled = 0;
  g_stick_to_ground_enabled = 0;
  g_drift_mode_enabled = 0;
  g_xenon_enabled = 0;
  g_rainbow_paint_enabled = 0;
  g_explosive_ammo_enabled = 0;
  g_fire_ammo_enabled = 0;
  // Newer toggles re-asserted on the game-thread tick; without these "Disable All" leaves slippery
  // roads / no-reload / explosive melee active. Slippery's reduced grip is restored by the
  // set_vehicle_reduce_grip(v, 0) reset further below.
  g_slippery_enabled = 0;
  g_no_reload_enabled = 0;
  g_explosive_melee_enabled = 0;
  g_weapon_damage_index = 0;
  g_hud_enabled = 0;

  if (has_core_player_natives()) {
    int player = native_player_id();
    int ped = native_player_ped_id();
    if (g_n.set_player_invincible) invoke_void(g_n.set_player_invincible, player, 0);
    if (g_n.set_entity_invincible && ped) invoke_void(g_n.set_entity_invincible, ped, 0);
    if (g_n.set_run_sprint_multiplier) invoke_void(g_n.set_run_sprint_multiplier, player, 1.0f);
    if (g_n.set_swim_multiplier) invoke_void(g_n.set_swim_multiplier, player, 1.0f);
    if (g_n.set_ped_move_rate_override && ped)
      invoke_void(g_n.set_ped_move_rate_override, ped, 1.0f);
    if (g_n.set_entity_visible && ped) invoke_void(g_n.set_entity_visible, ped, 1, 0);
    if (g_n.set_ped_can_ragdoll && ped) invoke_void(g_n.set_ped_can_ragdoll, ped, 1);
    if (g_n.set_ped_can_be_knocked_off_vehicle && ped)
      invoke_void(g_n.set_ped_can_be_knocked_off_vehicle, ped, 0);
    if (g_n.set_ped_suffers_critical_hits && ped)
      invoke_void(g_n.set_ped_suffers_critical_hits, ped, 1);
    if (g_n.set_entity_proofs && ped)
      invoke_void(g_n.set_entity_proofs, ped, 0, 0, 0, 0, 0, 0, 0, 0);
    // Clear vehicle invincibility / grip reduction if the player is in one.
    int v = current_vehicle();
    if (v && g_n.set_entity_invincible) invoke_void(g_n.set_entity_invincible, v, 0);
    if (v && g_n.set_vehicle_reduce_grip) invoke_void(g_n.set_vehicle_reduce_grip, v, 0);
    // Reset the weapon-damage multiplier back to the engine default -- only if it was
    // actually changed (never at init; see had_weapon_damage above).
    if (had_weapon_damage && g_n.set_player_weapon_damage_modifier)
      invoke_void(g_n.set_player_weapon_damage_modifier, player, 1.0f);
  }
  // World globals back to defaults.
  if (g_n.set_gravity_level) invoke_void(g_n.set_gravity_level, 0);
  if (g_n.set_time_scale) invoke_void(g_n.set_time_scale, 1.0f);

  // Cancel any in-progress teleport so the frozen entity is released, not stranded.
  if (g_tp_active && g_n.freeze_entity_position) {
    invoke_void(g_n.freeze_entity_position, g_tp_entity, 0);
    g_tp_active = 0;
  }
  // Release the noclip-frozen entity so it isn't left frozen mid-air.
  if (g_noclip_entity && g_n.freeze_entity_position) {
    invoke_void(g_n.freeze_entity_position, g_noclip_entity, 0);
  }
  g_noclip_enabled = 0;
  g_noclip_entity = 0;

  // Clear any active minigame mode. Flag-only here (safe at init via import_profile); the
  // game-thread driver sees the active id drop to NONE on its next fire and runs that mode's
  // world restore (calm peds, weather back to EXTRASUNNY, lights back on) in valid context.
  minigame_stop_all();

  // Stop the volatile per-tick driver modes (Fly, Vehicle Fly, Gun toolkit, Free Camera, Autopilot)
  // and the world-spectacle effects (Wind, Camera Shake). These are NOT g_state toggle bits, so the
  // flag resets above never touched them -- without this, "Disable All" leaves the player flying,
  // the toolkit gun firing on R2, the screen shaking, or stuck in the detached free camera with no
  // obvious off switch. Flag-only writes here are worker-safe + init-safe; the game-thread drivers
  // finish the restore on their next fire once they observe the mode cleared: fly_game_thread_tick
  // restores the entity's gravity + clears g_fly_entity when both fly modes are Off,
  // free_cam_game_thread_tick tears the scripted cam down, the autopilot tick clears the vehicle
  // drive task, and the cam-shake re-assert simply stops (SHAKE_GAMEPLAY_CAM decays). g_fly_entity /
  // fly_release live behind GTAV_MENU_ENABLE_FRAME_HOOK, so the gravity restore is left to the
  // driver rather than referenced here (keeps disable_all building in the no-hook config).
  g_fly_mode = 0;
  g_vehicle_fly_mode = 0;
  g_active_gun = 0;
  g_free_cam_active = 0;
  g_autopilot_mode = GTAV_AP_MODE_OFF;
  g_autopilot_dirty = 1;
  g_cam_shake_index = 0;
  const int was_windy = (g_wind_index != 0);
  g_wind_index = 0;
  // Wind back to calm -- guarded so the host-tested-only set_wind_speed never runs at init (where
  // g_wind_index is already 0), mirroring the had_weapon_damage discipline above.
  if (was_windy && g_n.set_wind_speed) invoke_void(g_n.set_wind_speed, 0.0f);

  return ok(action, "all features disabled");
}

// Teleport preset destinations (single-player landmarks). The row's param is the
// index here. Goes through the
// streaming-aware start_teleport() so a far jump streams the area in before the
// player is released, instead of crashing in unstreamed void.
static const struct GtavTeleportPreset {
  char name[28];  // inline (no relocation; longest observed 21)
  float x, y, z;
} kTeleportPresets[] = {
    // 0..4: original presets -- DO NOT reorder; the airfield/country rows below carry
    // these indices in their ShellItem .param. New presets append at the end.
    {"LS Airport", -1034.6f, -2733.6f, 20.2f},           // 0
    {"Mount Chiliad", 501.2f, 5604.5f, 797.9f},          // 1
    {"Fort Zancudo", -2354.0f, 3250.0f, 32.8f},          // 2
    {"Maze Bank Roof", -72.3f, -818.6f, 326.1f},         // 3
    {"Sandy Shores Airfield", 1747.0f, 3273.0f, 41.1f},  // 4
    // City landmarks (5..11).
    {"Maze Bank Arena", -324.0f, -1968.0f, 25.0f},  // 5
    {"Casino", 925.0f, 46.0f, 81.1f},               // 6
    {"Vinewood Sign", 711.0f, 1198.0f, 348.5f},     // 7
    {"Del Perro Pier", -1850.0f, -1231.6f, 13.0f},  // 8
    {"Legion Square", 195.0f, -934.0f, 30.7f},      // 9
    {"Vespucci Beach", -1223.0f, -1592.0f, 4.4f},   // 10
    {"Mirror Park", 1140.0f, -646.0f, 56.6f},       // 11
    // North / country landmarks (12..16).
    {"Paleto Bay", -148.0f, 6326.0f, 31.5f},            // 12
    {"Sandy Shores Town", 1960.0f, 3740.0f, 32.3f},     // 13
    {"Mount Gordo", 2877.0f, 5911.0f, 369.5f},          // 14
    {"Galileo Observatory", -438.0f, 1075.0f, 352.4f},  // 15
    {"Alamo Sea Marina", 1300.0f, 4216.0f, 33.9f},      // 16
    // Airfields (17..18).
    {"McKenzie Airfield", 2121.0f, 4796.0f, 41.1f},  // 17
    {"LSIA Helipad", -1230.0f, -2253.0f, 13.9f},     // 18
};

static uint32_t teleport_preset(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET;
  const uint32_t count = (uint32_t)(sizeof(kTeleportPresets) / sizeof(kTeleportPresets[0]));
  if (param >= count) return failed(action, "bad teleport index");
  const GtavTeleportPreset& d = kTeleportPresets[param];
  // FLOOR: presets carry a curated Z (some are rooftops / mountains), so keep it but rescue any
  // that would land below the streamed-in ground -- the max() never pulls a rooftop down.
  return start_teleport(action, d.x, d.y, d.z, d.name, GTAV_TP_CLAMP_FLOOR);
}

// Custom Packs: a pack map row's declared place (pack.cfg map row x/y/z, whole metres). An
// interior's place is inside it: exact (the ground probe finds the roof above it), not
// lifted, and released once the interior's collision is resident.
static uint32_t teleport_pack_map(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TELEPORT_PACK_MAP;
  int32_t x = 0, y = 0, z = 0;
  const char* text = nullptr;
  int interior = 0;
  if (!pack_map_place(param, &x, &y, &z, &text, &interior))
    return unavailable(action, "pack map has no place");
  // Custom Packs > Locations: which row was taken, for the hardware check (F2.4).
  placement_note("pack teleport row=%u at (%d,%d,%d) interior=%d text=%.39s", (unsigned)param,
                 (int)x, (int)y, (int)z, interior, text);
  return start_teleport(action, (float)x, (float)y, (float)z, text,
                        interior ? GTAV_TP_CLAMP_INTERIOR : GTAV_TP_CLAMP_FLOOR);
}

static uint32_t preset_stealth() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PRESET_STEALTH;
  if (!has_core_player_natives() || !g_n.set_player_wanted_level ||
      !g_n.set_player_wanted_level_now || !g_n.set_entity_visible || !g_n.set_ped_can_ragdoll) {
    return unavailable(action, "missing stealth preset natives");
  }
  int player = native_player_id();
  int ped = native_player_ped_id();
  g_never_wanted_enabled = 1;
  g_invisible_enabled = 1;
  g_no_ragdoll_enabled = 1;
  invoke_void(g_n.set_player_wanted_level, player, 0, 0);
  invoke_void(g_n.set_player_wanted_level_now, player, 0);
  invoke_void(g_n.set_entity_visible, ped, 0, 0);
  invoke_void(g_n.set_ped_can_ragdoll, ped, 0);
  return ok(action, "stealth preset enabled");
}

static uint32_t preset_stunt() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PRESET_STUNT;
  if (!has_core_player_natives() || !g_n.set_run_sprint_multiplier ||
      !g_n.set_ped_move_rate_override || !g_n.set_super_jump_this_frame ||
      !g_n.reset_player_stamina || !g_n.set_ped_can_be_knocked_off_vehicle) {
    return unavailable(action, "missing stunt preset natives");
  }
  int player = native_player_id();
  int ped = native_player_ped_id();
  g_state.super_run_enabled = 1;
  g_fast_move_enabled = 1;
  g_super_jump_enabled = 1;
  g_inf_stamina_enabled = 1;
  g_seatbelt_enabled = 1;
  invoke_void(g_n.set_run_sprint_multiplier, player, 1.49f);
  invoke_void(g_n.set_ped_move_rate_override, ped, 1.4f);
  invoke_void(g_n.set_super_jump_this_frame, player);
  invoke_void(g_n.reset_player_stamina, player);
  invoke_void(g_n.set_ped_can_be_knocked_off_vehicle, ped, 1);
  return ok(action, "stunt preset enabled");
}

static uint32_t preset_survival() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PRESET_SURVIVAL;
  if (!has_core_player_natives() || !g_n.set_player_invincible || !g_n.set_entity_invincible ||
      !g_n.set_entity_proofs || !g_n.set_ped_suffers_critical_hits || !g_n.reset_player_stamina ||
      !g_n.special_ability_fill_meter) {
    return unavailable(action, "missing survival preset natives");
  }
  int player = native_player_id();
  int ped = native_player_ped_id();
  g_state.god_mode_enabled = 1;
  g_super_proofs_enabled = 1;
  g_no_crit_enabled = 1;
  g_inf_stamina_enabled = 1;
  g_inf_special_enabled = 1;
  invoke_void(g_n.set_player_invincible, player, 1);
  invoke_void(g_n.set_entity_invincible, ped, 1);
  invoke_void(g_n.set_entity_proofs, ped, 1, 1, 1, 1, 1, 1, 1, 1);
  invoke_void(g_n.set_ped_suffers_critical_hits, ped, 0);
  invoke_void(g_n.reset_player_stamina, player);
  invoke_void(g_n.special_ability_fill_meter, player, 1);
  return ok(action, "survival preset enabled");
}

static uint32_t preset_driver() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER;
  if (!has_core_player_natives() || !g_n.set_ped_can_be_knocked_off_vehicle ||
      !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing driver preset natives");
  }
  g_seatbelt_enabled = 1;
  g_vehicle_god_enabled = 1;
  g_engine_on_enabled = 1;
  g_keep_clean_enabled = 1;
  invoke_void(g_n.set_ped_can_be_knocked_off_vehicle, native_player_ped_id(), 1);
  int v = current_vehicle();
  if (v) {
    if (g_n.set_entity_invincible) invoke_void(g_n.set_entity_invincible, v, 1);
    if (g_n.set_vehicle_fixed) invoke_void(g_n.set_vehicle_fixed, v);
    if (g_n.set_vehicle_dirt_level) invoke_void(g_n.set_vehicle_dirt_level, v, 0.0f);
    if (g_n.set_vehicle_engine_on) invoke_void(g_n.set_vehicle_engine_on, v, 1, 1, 0);
  }
  return ok(action, v ? "driver preset enabled" : "driver preset enabled for next vehicle");
}

// Parkour preset: super jump + no-ragdoll + fast movement, so the player can bound
// around the map without faceplanting. Pure combination of existing worker-safe toggles
// (the worker tick reasserts each), so it needs no natives the toggles don't already.
static uint32_t preset_parkour() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PRESET_PARKOUR;
  if (!has_core_player_natives() || !g_n.set_super_jump_this_frame || !g_n.set_ped_can_ragdoll ||
      !g_n.set_ped_move_rate_override) {
    return unavailable(action, "missing parkour preset natives");
  }
  int ped = native_player_ped_id();
  g_super_jump_enabled = 1;
  g_no_ragdoll_enabled = 1;
  g_fast_move_enabled = 1;
  invoke_void(g_n.set_super_jump_this_frame, native_player_id());
  invoke_void(g_n.set_ped_can_ragdoll, ped, 0);
  invoke_void(g_n.set_ped_move_rate_override, ped, current_move_rate());
  return ok(action, "parkour preset enabled");
}
