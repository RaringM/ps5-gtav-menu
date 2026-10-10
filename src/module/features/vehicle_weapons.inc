// Vehicle maintenance one-shots + weapon give/remove
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

static int current_vehicle() {
  if (g_n.get_vehicle_ped_is_in && g_n.player_ped_id) {
    int v = invoke_return<int>(g_n.get_vehicle_ped_is_in, native_player_ped_id(), 0);
    if (v) return v;
  }
  return g_last_spawned_vehicle;
}

// Strict: the vehicle the player ped is currently in, or 0. Unlike
// current_vehicle(), this never falls back to g_last_spawned_vehicle, which is
// never cleared on despawn/exit and can therefore be a stale handle -- unsafe for
// per-tick toggles that re-assert state every worker tick.
static int current_vehicle_strict() {
  if (!g_n.get_vehicle_ped_is_in || !g_n.player_ped_id) return 0;
  const int ped = native_player_ped_id();
  if (!ped) return 0;
  // GET_VEHICLE_PED_IS_IN(ped, false) is *documented* to return 0 on foot, but on this build it
  // keeps returning the LAST vehicle for a while after you exit -- so a bare read would boost /
  // delete / autopilot a vehicle the player is no longer in. Gate on IS_PED_IN_ANY_VEHICLE first
  // (the reliable on-foot check, used the same way by current_player_entity / fly / teleport);
  // only fall back to the raw read if that native is unavailable.
  if (g_n.is_ped_in_any_vehicle && !invoke_return<int>(g_n.is_ped_in_any_vehicle, ped, 0)) {
    return 0;
  }
  return invoke_return<int>(g_n.get_vehicle_ped_is_in, ped, 0);
}

static int current_player_entity() {
  int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
  if (!ped) return 0;
  if (g_n.is_ped_in_any_vehicle && g_n.get_vehicle_ped_is_in &&
      invoke_return<int>(g_n.is_ped_in_any_vehicle, ped, 0)) {
    int v = invoke_return<int>(g_n.get_vehicle_ped_is_in, ped, 0);
    if (v) return v;
  }
  return ped;
}

static const NitroProfile& current_nitro_profile() {
  const uint32_t count = (uint32_t)(sizeof(kNitroProfiles) / sizeof(kNitroProfiles[0]));
  return kNitroProfiles[g_nitro_power_index % count];
}

// Rocket Boost: fling the current vehicle straight up with a one-shot impulse (complements the
// forward Launch Boost). APPLY_FORCE_TO_ENTITY touches the live CVehicle physics, so this runs on
// the game/script thread via run_gated_action, never worker-direct. Magnitude is a starting value
// to tune on hardware (impulse type is mass-scaled). forceType 1 = IMPULSE; world-space up vector.
static const float GTAV_ROCKET_BOOST_IMPULSE = 90.0f;
static uint32_t vehicle_rocket_boost() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_VEHICLE_ROCKET_BOOST;
  if (!g_n.apply_force_to_entity) return unavailable(action, "missing force native");
  int v = current_vehicle_strict();
  if (!v) return failed(action, "not in a vehicle");
  // APPLY_FORCE_TO_ENTITY(entity, forceType, x, y, z, offX, offY, offZ, boneIndex, isDirectionRel,
  // ignoreUpVec, isForceRel, p12, p13). World-space (+Z) impulse, applied at the centre of mass.
  invoke_void(g_n.apply_force_to_entity, v, 1, 0.0f, 0.0f, GTAV_ROCKET_BOOST_IMPULSE, 0.0f, 0.0f,
              0.0f, 0, 0, 1, 1, 0, 1);
  return ok(action, "rocket boost");
}

static uint32_t fix_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE;
  if (!g_n.set_vehicle_fixed) return unavailable(action, "missing vehicle fix native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_fixed, v);
  if (g_n.set_vehicle_engine_on) invoke_void(g_n.set_vehicle_engine_on, v, 1, 1, 0);
  return ok(action, "vehicle repaired");
}

static uint32_t clean_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAN_VEHICLE;
  if (!g_n.set_vehicle_dirt_level) return unavailable(action, "missing vehicle clean native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_dirt_level, v, 0.0f);
  return ok(action, "vehicle cleaned");
}

static uint32_t repair_clean_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE;
  if (!g_n.set_vehicle_fixed || !g_n.set_vehicle_dirt_level) {
    return unavailable(action, "missing vehicle repair/clean natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_fixed, v);
  invoke_void(g_n.set_vehicle_dirt_level, v, 0.0f);
  if (g_n.set_vehicle_engine_on) invoke_void(g_n.set_vehicle_engine_on, v, 1, 1, 0);
  return ok(action, "vehicle repaired and cleaned");
}

// Delete Current Vehicle: remove the vehicle the player is currently in. DELETE_ENTITY tears
// down the entity (allocation-class, like CREATE_VEHICLE), so this is a game-thread-gated action
// drained by the frame hook -- never worker-direct. Uses current_vehicle_strict() so it can only
// ever delete the car the player is actually in, never the g_last_spawned_vehicle fallback (which
// is never cleared on exit and could be a stale handle pointing at a long-gone car). Claims the
// vehicle as a mission entity first so the engine permits the delete, then DELETE_ENTITY(&handle)
// (the native zeroes the handle); the player drops to the ground as the car vanishes.
static uint32_t delete_current_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE;
  if (!g_n.delete_entity || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing delete native");
  }
  int v = current_vehicle_strict();
  if (!v) return failed(action, "not in a vehicle");
  const int handle = v;
  if (g_n.set_entity_as_mission_entity) invoke_void(g_n.set_entity_as_mission_entity, v, 1, 1);
  invoke_void(g_n.delete_entity, &v);
  // We just deleted the spawn handle, if it was one -- clear it so current_vehicle() can't return
  // a now-dangling handle.
  if (g_last_spawned_vehicle == handle) g_last_spawned_vehicle = 0;
  return ok(action, "vehicle deleted");
}

static uint32_t launch_boost() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LAUNCH_BOOST;
  if (!g_n.set_vehicle_forward_speed || !g_n.get_entity_speed || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing launch boost natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const NitroProfile& profile = current_nitro_profile();
  float speed = invoke_return<float>(g_n.get_entity_speed, v);
  float target = speed + (profile.step_mps * 2.0f);
  if (target > profile.max_mps) target = profile.max_mps;
  invoke_void(g_n.set_vehicle_forward_speed, v, target);
  return ok(action, "launch boost applied");
}

static uint32_t place_on_wheels() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PLACE_ON_WHEELS;
  if (!g_n.set_vehicle_on_ground_properly)
    return unavailable(action, "missing place-on-ground native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  // SET_VEHICLE_ON_GROUND_PROPERLY drops the vehicle onto its wheels at the ground
  // below it -- unsticks/flips a rolled or stuck car. Modify-existing, worker-safe.
  // It returns a BOOL: 0 means it couldn't find ground (airborne/over water), so
  // don't report a false success.
  int placed = invoke_return<int>(g_n.set_vehicle_on_ground_properly, v);
  if (!placed) return failed(action, "could not find ground below vehicle");
  return ok(action, "vehicle placed on wheels");
}

static uint32_t give_weapons() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS;
  gtav_weapon_stage = 1;
  // Weapon hashes are precomputed in the catalog, so GET_HASH_KEY is no longer needed.
  if (!g_n.give_weapon_to_ped || !g_n.player_ped_id) {
    gtav_weapon_stage = 0;
    return unavailable(action, "missing weapon natives");
  }
  int ped = native_player_ped_id();
  // GIVE_WEAPON_TO_PED dereferences the ped's weapon manager; an invalid ped (loading
  // screen, cutscene, before the world is up) faults the game thread. Bail gracefully,
  // mirroring the spawn path's ped guard, so the menu reports it instead of crashing.
  if (ped <= 0) {
    gtav_weapon_stage = 0;
    return failed(action, "no valid player ped (be in active gameplay)");
  }
  gtav_weapon_stage = 2;
  uint32_t given = 0;
  for (uint32_t i = 0; i < gtav_weapon_catalog_count; ++i) {
    uint32_t hash = gtav_weapon_catalog[i].hash;
    if (!hash) continue;
    gtav_weapon_stage = 100u + i;  // a crash read here localizes to this weapon
    invoke_void(g_n.give_weapon_to_ped, ped, hash, gtav_weapon_catalog[i].ammo, 0, i == 0 ? 1 : 0);
    ++given;
  }
  gtav_weapon_stage = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "gave %u weapons", given);
  return ok(action, msg);
}

// Give + equip a SINGLE weapon by hash (the weapon-browser rows carry the joaat hash
// in param). Same game-thread gating and ped guard as give_weapons(); the catalog is
// consulted only for a matching ammo count (defaulting otherwise), then
// SET_CURRENT_PED_WEAPON draws it so the player sees the change immediately.
static uint32_t give_weapon_single(uint32_t hash) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON;
  gtav_weapon_stage = 20;
  if (!g_n.give_weapon_to_ped || !g_n.player_ped_id) {
    gtav_weapon_stage = 0;
    return unavailable(action, "missing weapon natives");
  }
  if (!hash) return failed(action, "no weapon selected");
  int ped = native_player_ped_id();
  if (ped <= 0) {
    gtav_weapon_stage = 0;
    return failed(action, "no valid player ped (be in active gameplay)");
  }
  int ammo = 250;
  int stock = 0;
  for (uint32_t i = 0; i < gtav_weapon_catalog_count; ++i) {
    if (gtav_weapon_catalog[i].hash == hash) {
      ammo = gtav_weapon_catalog[i].ammo;
      stock = 1;
      break;
    }
  }
  gtav_weapon_stage = 21;
  // Last arg bForceInHand=1 equips on give; SET_CURRENT_PED_WEAPON makes it the active
  // weapon (force=1) so the swap is visible without scrolling the weapon wheel.
  invoke_void(g_n.give_weapon_to_ped, ped, hash, ammo, 0, 1);
  if (g_n.set_current_ped_weapon) invoke_void(g_n.set_current_ped_weapon, ped, hash, 1);
  const char* given = "weapon given";
#if GTAV_MENU_ENABLE_CUSTOM_PACKS
  if (!stock && g_n.get_current_ped_weapon) {
    // Pack weapon: klog what the ped now holds (its model is in the pack's load-time
    // "pack data N weapon ... model=" klog line), then its wheel slot (weapon_components.inc).
    uint32_t current = 0u;
    invoke_return<int>(g_n.get_current_ped_weapon, ped, &current, 1);
    char line[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(line, sizeof(line), "GTAVMenu pack weapon given 0x%08x current=0x%08x",
             (unsigned)hash, (unsigned)current);
    gtav_worker_klog(line);
    given = weapon_given_note(hash);
  }
#else
  (void)stock;
#endif
  gtav_weapon_stage = 0;
  return ok(action, given);
}

static uint32_t remove_weapons() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS;
  gtav_weapon_stage = 10;
  if (!g_n.remove_all_ped_weapons || !g_n.player_ped_id) {
    gtav_weapon_stage = 0;
    return unavailable(action, "missing remove-weapon native");
  }
  int ped = native_player_ped_id();
  // Same ped guard: REMOVE_ALL_PED_WEAPONS tears down the ped's weapon objects and
  // faults on an invalid ped, so refuse gracefully outside active gameplay.
  if (ped <= 0) {
    gtav_weapon_stage = 0;
    return failed(action, "no valid player ped (be in active gameplay)");
  }
  gtav_weapon_stage = 12;
  invoke_void(g_n.remove_all_ped_weapons, ped, 1);
  gtav_weapon_stage = 0;
  return ok(action, "all weapons removed");
}

// Give Max Ammo: top up ammo to the per-weapon maximum for the weapons the player carries,
// distinct from Give Weapon Pack (which dumps the whole catalog). We iterate the weapon catalog,
// then the weapon in hand and the loaded packs' weapons, and, for each, read GET_MAX_AMMO (int*
// out-param) then SET_PED_AMMO -- both are no-ops for a weapon the ped does not own, so this only
// tops up carried weapons. Ped weapon-inventory natives
// run on the game/script thread (gated like GIVE_WEAPON), never worker-direct.
// GET_MAX_AMMO (int* out-param) then SET_PED_AMMO for one weapon. Returns the max (0 when the ped
// does not own the weapon: nothing is set). Game thread.
[[gnu::noinline]] static int top_up_ammo(int ped, uint32_t hash) {
  int max_ammo = 0;
  invoke_void(g_n.get_max_ammo, ped, hash, &max_ammo);
  if (max_ammo > 0) invoke_void(g_n.set_ped_ammo, ped, hash, max_ammo);
  return max_ammo;
}

static uint32_t give_max_ammo() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO;
  if (!g_n.set_ped_ammo || !g_n.get_max_ammo || !g_n.player_ped_id) {
    return unavailable(action, "missing ammo natives");
  }
  int ped = native_player_ped_id();
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  for (uint32_t i = 0; i < gtav_weapon_catalog_count; ++i) {
    uint32_t hash = gtav_weapon_catalog[i].hash;
    if (!hash) continue;
    // GET_MAX_AMMO(ped, weaponHash, int* out) -> out is the weapon type's max; 0 if not owned.
    top_up_ammo(ped, hash);
  }
  give_max_ammo_extra(ped);  // the weapon in hand and the pack weapons (weapon_components.inc)
  return ok(action, "max ammo given");
}

