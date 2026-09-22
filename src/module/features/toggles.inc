// Persistent toggles (self / vehicle / HUD) re-applied by the worker tick
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

// CPED_CONFIG_FLAG_CanFlyThroughWindscreen. Clearing this flag is the seatbelt:
// it stops a car occupant being thrown through the windscreen on a hard crash.
#define GTAV_PED_CONFIG_FLAG_FLY_THROUGH_WINDSCREEN 32

static uint32_t toggle_super_jump() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_JUMP;
  if (!g_n.set_super_jump_this_frame) {
    return unavailable(action, "missing super jump native");
  }
  g_super_jump_enabled = !g_super_jump_enabled;
  return ok(action, g_super_jump_enabled ? "super jump enabled" : "super jump disabled");
}

static uint32_t toggle_invisible() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_INVISIBLE;
  if (!g_n.set_entity_visible || !g_n.player_ped_id) {
    return unavailable(action, "missing visibility native");
  }
  g_invisible_enabled = !g_invisible_enabled;
  // SET_ENTITY_VISIBLE(entity, toggle): toggle=1 visible, 0 invisible.
  invoke_void(g_n.set_entity_visible, native_player_ped_id(), g_invisible_enabled ? 0 : 1, 0);
  return ok(action, g_invisible_enabled ? "invisibility enabled" : "invisibility disabled");
}

static uint32_t toggle_never_wanted() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NEVER_WANTED;
  if (!g_n.player_id || !g_n.set_player_wanted_level || !g_n.set_player_wanted_level_now) {
    return unavailable(action, "missing wanted-level natives");
  }
  g_never_wanted_enabled = !g_never_wanted_enabled;
  if (g_never_wanted_enabled) {
    // Clear immediately so the toggle has visible effect even before the next tick.
    int player = native_player_id();
    invoke_void(g_n.set_player_wanted_level, player, 0, 0);
    invoke_void(g_n.set_player_wanted_level_now, player, 0);
  }
  return ok(action, g_never_wanted_enabled ? "never wanted enabled" : "never wanted disabled");
}

static uint32_t toggle_vehicle_god() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_VEHICLE_GOD;
  if (!g_n.get_vehicle_ped_is_in || !g_n.set_entity_invincible || !g_n.player_ped_id) {
    return unavailable(action, "missing vehicle invincibility natives");
  }
  g_vehicle_god_enabled = !g_vehicle_god_enabled;
  int v = current_vehicle();
  if (v) invoke_void(g_n.set_entity_invincible, v, g_vehicle_god_enabled ? 1 : 0);
  return ok(action,
            g_vehicle_god_enabled ? "vehicle god mode enabled" : "vehicle god mode disabled");
}

static uint32_t toggle_nitro() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NITRO;
  if (!g_n.set_vehicle_forward_speed || !g_n.get_entity_speed || !g_n.is_control_pressed ||
      !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing nitro natives");
  }
  g_nitro_enabled = !g_nitro_enabled;
  return ok(action, g_nitro_enabled ? "nitro on horn enabled" : "nitro on horn disabled");
}

static uint32_t toggle_low_gravity() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_LOW_GRAVITY;
  if (!g_n.set_gravity_level) return unavailable(action, "missing gravity native");
  g_low_gravity_enabled = !g_low_gravity_enabled;
  // SET_GRAVITY_LEVEL(level): 0 = normal (9.8 m/s^2), 2 = low/moon gravity. Sticky
  // global, so set once on toggle (no per-tick reapply).
  invoke_void(g_n.set_gravity_level, g_low_gravity_enabled ? 2 : 0);
  return ok(action, g_low_gravity_enabled ? "low gravity enabled" : "low gravity disabled");
}

static uint32_t toggle_slow_motion() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLOW_MOTION;
  if (!g_n.set_time_scale) return unavailable(action, "missing time-scale native");
  g_slow_motion_enabled = !g_slow_motion_enabled;
  // SET_TIME_SCALE(scale): 1.0 = normal, 0.4 = slow motion. Sticky global; the menu
  // runs on the scePad worker thread so it stays responsive regardless.
  invoke_void(g_n.set_time_scale, g_slow_motion_enabled ? 0.4f : 1.0f);
  return ok(action, g_slow_motion_enabled ? "slow motion enabled" : "slow motion disabled");
}

static uint32_t toggle_inf_stamina() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_STAMINA;
  if (!g_n.reset_player_stamina || !g_n.player_id) {
    return unavailable(action, "missing stamina native");
  }
  g_inf_stamina_enabled = !g_inf_stamina_enabled;
  return ok(action,
            g_inf_stamina_enabled ? "infinite stamina enabled" : "infinite stamina disabled");
}

static uint32_t toggle_inf_special() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_SPECIAL;
  if (!g_n.special_ability_fill_meter || !g_n.player_id) {
    return unavailable(action, "missing special-ability native");
  }
  g_inf_special_enabled = !g_inf_special_enabled;
  return ok(action,
            g_inf_special_enabled ? "infinite special enabled" : "infinite special disabled");
}

static uint32_t toggle_super_proofs() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_PROOFS;
  if (!g_n.set_entity_proofs || !g_n.player_ped_id) {
    return unavailable(action, "missing proofs native");
  }
  g_super_proofs_enabled = !g_super_proofs_enabled;
  int p = g_super_proofs_enabled ? 1 : 0;
  // SET_ENTITY_PROOFS(entity, bullet, fire, explosion, collision, melee, steam,
  // smoke, drown): exactly 8 proof flags after the entity.
  invoke_void(g_n.set_entity_proofs, native_player_ped_id(), p, p, p, p, p, p, p, p);
  return ok(action, g_super_proofs_enabled ? "super proofs enabled" : "super proofs disabled");
}

static uint32_t toggle_no_crit() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_CRIT;
  if (!g_n.set_ped_suffers_critical_hits || !g_n.player_ped_id) {
    return unavailable(action, "missing critical-hits native");
  }
  g_no_crit_enabled = !g_no_crit_enabled;
  // SET_PED_SUFFERS_CRITICAL_HITS(ped, toggle): toggle=0 means no critical hits
  // (e.g. headshots no longer instantly kill).
  invoke_void(g_n.set_ped_suffers_critical_hits, native_player_ped_id(), g_no_crit_enabled ? 0 : 1);
  return ok(action, g_no_crit_enabled ? "no critical hits enabled" : "no critical hits disabled");
}

static uint32_t toggle_no_ragdoll() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RAGDOLL;
  if (!g_n.set_ped_can_ragdoll || !g_n.player_ped_id) {
    return unavailable(action, "missing ragdoll native");
  }
  g_no_ragdoll_enabled = !g_no_ragdoll_enabled;
  // SET_PED_CAN_RAGDOLL(ped, toggle): toggle=0 means the ped can no longer ragdoll.
  invoke_void(g_n.set_ped_can_ragdoll, native_player_ped_id(), g_no_ragdoll_enabled ? 0 : 1);
  return ok(action, g_no_ragdoll_enabled ? "no ragdoll enabled" : "no ragdoll disabled");
}

static uint32_t toggle_seatbelt() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEATBELT;
  if (!g_n.set_ped_can_be_knocked_off_vehicle || !g_n.player_ped_id) {
    return unavailable(action, "missing seatbelt native");
  }
  g_seatbelt_enabled = !g_seatbelt_enabled;
  int ped = native_player_ped_id();
  // SET_PED_CAN_BE_KNOCKED_OFF_VEHICLE(ped, state): 1 = KNOCKOFFVEHICLE_NEVER. This
  // only covers two-wheelers (bikes/motorcycles), not car windscreen ejection.
  invoke_void(g_n.set_ped_can_be_knocked_off_vehicle, ped, g_seatbelt_enabled ? 1 : 0);
  // SET_PED_CONFIG_FLAG(ped, 32, value): flag 32 = CanFlyThroughWindscreen. Clearing it
  // (false) is the real seatbelt -- it stops the car driver/passenger flying through the
  // windscreen on a high-speed crash, which the knock-off-vehicle native does not do.
  if (g_n.set_ped_config_flag)
    invoke_void(g_n.set_ped_config_flag, ped, GTAV_PED_CONFIG_FLAG_FLY_THROUGH_WINDSCREEN,
                g_seatbelt_enabled ? 0 : 1);
  return ok(action, g_seatbelt_enabled ? "seatbelt enabled" : "seatbelt disabled");
}

static uint32_t toggle_keep_clean() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_CLEAN;
  if (!g_n.set_vehicle_dirt_level || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing dirt-level native");
  }
  g_keep_clean_enabled = !g_keep_clean_enabled;
  int v = current_vehicle();
  if (v && g_keep_clean_enabled) invoke_void(g_n.set_vehicle_dirt_level, v, 0.0f);
  return ok(action, g_keep_clean_enabled ? "keep clean enabled" : "keep clean disabled");
}

static uint32_t toggle_fast_move() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_MOVE;
  if (!g_n.set_ped_move_rate_override || !g_n.player_ped_id) {
    return unavailable(action, "missing move-rate native");
  }
  g_fast_move_enabled = !g_fast_move_enabled;
  if (!g_fast_move_enabled) {
    // Restore the normal 1.0 move rate immediately when disabling.
    invoke_void(g_n.set_ped_move_rate_override, native_player_ped_id(), 1.0f);
  }
  return ok(action, g_fast_move_enabled ? "fast movement enabled" : "fast movement disabled");
}

static uint32_t toggle_engine_on() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_ENGINE_ON;
  if (!g_n.set_vehicle_engine_on || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing engine natives");
  }
  g_engine_on_enabled = !g_engine_on_enabled;
  int v = current_vehicle();
  // SET_VEHICLE_ENGINE_ON(veh, on, instantly, disableAutoStart): start it now when
  // enabling so the toggle has immediate effect.
  if (v && g_engine_on_enabled) invoke_void(g_n.set_vehicle_engine_on, v, 1, 1, 0);
  return ok(action, g_engine_on_enabled ? "engine always on enabled" : "engine always on disabled");
}

// Auto-Heal: top up health + armor every tick so the player is effectively immortal
// against chip damage without the full god-mode invincibility. Reuses the same
// modify-existing natives as the one-shot heal_armor(), driven from the worker tick.
static uint32_t toggle_auto_heal() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_HEAL;
  if (!g_n.player_ped_id || !g_n.set_entity_health) {
    return unavailable(action, "missing heal natives");
  }
  g_auto_heal_enabled = !g_auto_heal_enabled;
  return ok(action, g_auto_heal_enabled ? "auto heal enabled" : "auto heal disabled");
}

// Wanted Lock: hold the wanted level at the Set-Wanted-Level cycler's selected value
// every tick (distinct from Never Wanted, which forces 0). Reuses the same wanted
// natives; the level comes from g_wanted_index so picking "3 STARS" then locking
// keeps exactly three stars.
static uint32_t toggle_wanted_lock() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_WANTED_LOCK;
  if (!g_n.player_id || !g_n.set_player_wanted_level || !g_n.set_player_wanted_level_now) {
    return unavailable(action, "missing wanted-level natives");
  }
  g_wanted_lock_enabled = !g_wanted_lock_enabled;
  return ok(action, g_wanted_lock_enabled ? "wanted lock enabled" : "wanted lock disabled");
}

// Keep Repaired: just flips the flag. The actual repair runs on the game-thread
// frame-hook tick (tick_vehicle_toggles): it pins engine/body/petrol health every tick
// and runs SET_VEHICLE_FIXED whenever the car shows damage. We do NOT call
// SET_VEHICLE_FIXED here -- toggle dispatch is worker-direct, and its Fix() rebuild
// deadlocks the console off the worker thread; the game-thread tick picks up the newly
// set flag and repairs any existing damage within a frame or two.
static uint32_t toggle_keep_repaired() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_REPAIRED;
  if (!g_n.set_vehicle_fixed || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing vehicle fix native");
  }
  g_keep_repaired_enabled = !g_keep_repaired_enabled;
  return ok(action, g_keep_repaired_enabled ? "keep repaired enabled" : "keep repaired disabled");
}

static uint32_t toggle_stick_to_ground() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_STICK_TO_GROUND;
  if (!g_n.set_vehicle_on_ground_properly || !g_n.get_vehicle_ped_is_in || !g_n.player_ped_id) {
    return unavailable(action, "missing place-on-ground native");
  }
  g_stick_to_ground_enabled = !g_stick_to_ground_enabled;
  return ok(action,
            g_stick_to_ground_enabled ? "stick to ground enabled" : "stick to ground disabled");
}

// Drift Mode: reduce the current vehicle's tyre grip so it slides under throttle.
// SET_VEHICLE_REDUCE_GRIP is a modify-existing field store (worker-safe). The worker
// tick re-asserts it (speed-gated, see drift_reduce_grip_now) while enabled; on enable
// we apply the same speed gate so toggling on at a standstill doesn't strand the car,
// and on disable we restore grip on the current vehicle.
static uint32_t toggle_drift_mode() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_DRIFT_MODE;
  if (!g_n.set_vehicle_reduce_grip || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing drift native");
  }
  g_drift_mode_enabled = !g_drift_mode_enabled;
  int v = current_vehicle_strict();
  if (v) {
    int reduce = g_drift_mode_enabled ? drift_reduce_grip_now(v) : 0;
    invoke_void(g_n.set_vehicle_reduce_grip, v, reduce);
  }
  return ok(action, g_drift_mode_enabled ? "drift mode enabled" : "drift mode disabled");
}

// Slippery Roads: cut the current vehicle's tyre grip to a constant maximum so it slides like ice,
// regardless of speed (distinct from Drift Mode, which only reduces grip once rolling). Same
// modify-existing field store as Drift (SET_VEHICLE_REDUCE_GRIP, worker-safe); the game-thread
// vehicle tick re-asserts it while enabled, and the handler restores grip on the current car on
// disable. Re-running the toggle on profile restore is safe (flag flip + one field store).
static uint32_t toggle_slippery() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLIPPERY;
  if (!g_n.set_vehicle_reduce_grip || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing grip native");
  }
  g_slippery_enabled = !g_slippery_enabled;
  int v = current_vehicle_strict();
  if (v) invoke_void(g_n.set_vehicle_reduce_grip, v, g_slippery_enabled ? 1 : 0);
  return ok(action, g_slippery_enabled ? "slippery roads enabled" : "slippery roads disabled");
}

// No-Reload / Infinite Clip: flag flip only. SET_PED_INFINITE_AMMO_CLIP touches the ped weapon
// state, so it is re-asserted on the game-thread player tick (tick_player_toggles), not the worker.
// Re-running the toggle on profile restore is safe (no native call here).
static uint32_t toggle_no_reload() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RELOAD;
  if (!g_n.set_ped_infinite_ammo_clip || !g_n.player_ped_id) {
    return unavailable(action, "missing infinite-clip native");
  }
  g_no_reload_enabled = !g_no_reload_enabled;
  return ok(action, g_no_reload_enabled ? "no reload enabled" : "no reload disabled");
}

// Super Brake: while enabled and the brake control is held, the worker tick bleeds the
// current vehicle's forward speed toward zero for a hard, quick stop. SET_VEHICLE_FORWARD_SPEED
// is a modify-existing field store (worker-safe), the same native Nitro uses. The toggle just
// flips the flag; the per-tick logic lives in gtav_features_worker_tick's vehicle block.
static uint32_t toggle_super_brake() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_BRAKE;
  if (!g_n.set_vehicle_forward_speed || !g_n.get_entity_speed || !g_n.is_control_pressed ||
      !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing brake natives");
  }
  g_super_brake_enabled = !g_super_brake_enabled;
  return ok(action, g_super_brake_enabled ? "super brake enabled" : "super brake disabled");
}

// Infinite Parachutes: flag flip only. The GIVE_WEAPON_TO_PED(GADGET_PARACHUTE) re-grant walks
// the ped weapon manager (NOT worker-safe -- it crashed off-thread for give_weapons), so it runs
// on the game-thread frame-hook tick (tick_player_toggles), like the seatbelt/proofs re-asserts.
static uint32_t toggle_infinite_parachutes() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_PARACHUTES;
  if (!g_n.give_weapon_to_ped || !g_n.player_ped_id) {
    return unavailable(action, "missing parachute natives");
  }
  g_inf_parachute_enabled = !g_inf_parachute_enabled;
  return ok(action, g_inf_parachute_enabled ? "infinite parachutes enabled"
                                            : "infinite parachutes disabled");
}

// Spawn Maxed / Spawn Invincible: pure display flags read at vehicle create-time by
// apply_spawn_vehicle_defaults() (no native at toggle time, like the HUD toggles). They change
// only FUTURE spawns, so the handlers are crash-safe even when re-run at init by disable_all /
// import_profile.
static uint32_t toggle_spawn_maxed() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_MAXED;
  g_spawn_maxed_enabled = !g_spawn_maxed_enabled;
  return ok(action, g_spawn_maxed_enabled ? "new vehicles spawn maxed" : "spawn maxed disabled");
}

static uint32_t toggle_spawn_invincible() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_INVINCIBLE;
  g_spawn_invincible_enabled = !g_spawn_invincible_enabled;
  return ok(action, g_spawn_invincible_enabled ? "new vehicles spawn invincible"
                                               : "spawn invincible disabled");
}

// Thin Population: flag flip only. SET_*_POPULATION_BUDGET(0) are global int setters (no entity
// walk), but the engine re-raises the budget, so they are re-asserted every tick from
// tick_player_toggles; the engine restores normal density once we stop forcing 0 (no explicit
// restore needed on disable).
static uint32_t toggle_thin_population() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_THIN_POPULATION;
  if (!g_n.set_ped_population_budget || !g_n.set_vehicle_population_budget) {
    return unavailable(action, "missing population natives");
  }
  g_thin_population_enabled = !g_thin_population_enabled;
  return ok(action,
            g_thin_population_enabled ? "thin population enabled" : "thin population disabled");
}

// Ignored By Everyone: police, ambient peds and gangs all leave the player alone. The ignore
// flags are sticky player-state setters, so the game-thread tick re-asserts them while enabled
// (survives respawns) and the handler restores the defaults once on disable -- same shape as
// toggle_fast_move. No native is called per-tick from the worker (the project's game-thread rule).
static uint32_t toggle_ignored_by_all() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_IGNORED_BY_ALL;
  if (!g_n.player_id || !g_n.set_police_ignore_player || !g_n.set_everyone_ignore_player) {
    return unavailable(action, "missing ignore natives");
  }
  g_ignored_by_all_enabled = !g_ignored_by_all_enabled;
  if (!g_ignored_by_all_enabled) {
    // Restore "noticed by everyone" immediately on disable (the tick stops asserting).
    int player = valid_player_id();
    if (player >= 0) {
      invoke_void(g_n.set_police_ignore_player, player, 0);
      invoke_void(g_n.set_everyone_ignore_player, player, 0);
      if (g_n.set_player_can_be_hassled_by_gangs)
        invoke_void(g_n.set_player_can_be_hassled_by_gangs, player, 1);
    }
  }
  return ok(action,
            g_ignored_by_all_enabled ? "ignored by everyone enabled" : "ignored by everyone disabled");
}

// HUD master + per-element toggles. The overlay itself is drawn by the bridge render
// path (which owns the text natives); these just flip the flags the renderer reads
// back through gtav_features_hud_readout().
static uint32_t toggle_hud() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD;
  g_hud_enabled = !g_hud_enabled;
  return ok(action, g_hud_enabled ? "hud enabled" : "hud disabled");
}

static uint32_t toggle_hud_speedo() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_SPEEDO;
  g_hud_speedo_enabled = !g_hud_speedo_enabled;
  return ok(action, g_hud_speedo_enabled ? "speedometer enabled" : "speedometer disabled");
}

static uint32_t toggle_hud_coords() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_COORDS;
  g_hud_coords_enabled = !g_hud_coords_enabled;
  return ok(action, g_hud_coords_enabled ? "coordinates enabled" : "coordinates disabled");
}

static uint32_t toggle_hud_fps() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_FPS;
  g_hud_fps_enabled = !g_hud_fps_enabled;
  return ok(action, g_hud_fps_enabled ? "fps readout enabled" : "fps readout disabled");
}

static uint32_t toggle_hud_distance() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_DISTANCE;
  g_hud_distance_enabled = !g_hud_distance_enabled;
  return ok(action,
            g_hud_distance_enabled ? "waypoint distance enabled" : "waypoint distance disabled");
}

