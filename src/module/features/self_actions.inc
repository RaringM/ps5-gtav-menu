// Self one-shot actions (god mode, heal/armor, clean, clear wanted, run/swim/ammo)
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

static uint32_t toggle_god_mode() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE;
  if (!has_core_player_natives() || !g_n.set_player_invincible) {
    return unavailable(action, "missing player invincibility natives");
  }
  int player = native_player_id();
  int ped = native_player_ped_id();
  g_state.god_mode_enabled = !g_state.god_mode_enabled;
  invoke_void(g_n.set_player_invincible, player, (int)g_state.god_mode_enabled);
  if (g_n.set_entity_invincible && ped) {
    invoke_void(g_n.set_entity_invincible, ped, (int)g_state.god_mode_enabled);
  }
  return ok(action, g_state.god_mode_enabled ? "god mode enabled" : "god mode disabled");
}

static uint32_t heal_armor() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_HEAL_ARMOR;
  if (!g_n.player_ped_id || !g_n.set_entity_health) {
    return unavailable(action, "missing heal natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return unavailable(action, "player ped unavailable");

  // Raise the health ceiling to the player's full upgraded maximum (328 in story
  // mode) before topping up, so Heal + Armor also restores a depleted max-health pool.
  if (g_n.set_ped_max_health) invoke_void(g_n.set_ped_max_health, ped, 328);
  int health = 200;
  if (g_n.get_ped_max_health) {
    int max = invoke_return<int>(g_n.get_ped_max_health, ped);
    if (max > 0) health = max;
  }
  invoke_void(g_n.set_entity_health, ped, health);
  if (g_n.set_ped_armour) invoke_void(g_n.set_ped_armour, ped, 100);
  if (g_n.clear_ped_blood_damage) invoke_void(g_n.clear_ped_blood_damage, ped);
  if (g_n.clear_ped_wetness) invoke_void(g_n.clear_ped_wetness, ped);
  return ok(action, "health and armor applied");
}

// Clean Self: clear blood splatter and wetness from the player ped without healing.
// Modify-existing-entity natives, worker-safe (no allocation).
static uint32_t clean_self() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAN_SELF;
  if (!g_n.player_ped_id || (!g_n.clear_ped_blood_damage && !g_n.clear_ped_wetness)) {
    return unavailable(action, "missing clean-self natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return unavailable(action, "player ped unavailable");
  if (g_n.clear_ped_blood_damage) invoke_void(g_n.clear_ped_blood_damage, ped);
  if (g_n.clear_ped_wetness) invoke_void(g_n.clear_ped_wetness, ped);
  return ok(action, "blood and wetness cleared");
}

// Kill Self: drop the player ped's health to 0 for a quick respawn. Worker-safe
// modify-existing write (same lane as heal_armor).
static uint32_t kill_self() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_KILL_SELF;
  if (!g_n.player_ped_id || !g_n.set_entity_health) {
    return unavailable(action, "missing health native");
  }
  int ped = native_player_ped_id();
  if (!ped) return unavailable(action, "player ped unavailable");
  invoke_void(g_n.set_entity_health, ped, 0);
  return ok(action, "player killed");
}

// Force Ragdoll: flop the player ped into a physics ragdoll for ~3s. Mutates the ped task
// tree, so it runs on the game/script thread via run_gated_action (gated like the
// explosions), not worker-direct. SET_PED_TO_RAGDOLL(ped, time1, time2, type, p4, p5, p6):
// time1/time2 = min/max ms, type 0 = normal balance.
static uint32_t force_ragdoll() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL;
  if (!g_n.set_ped_to_ragdoll || !g_n.player_ped_id) {
    return unavailable(action, "missing ragdoll native");
  }
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  invoke_return<int>(g_n.set_ped_to_ragdoll, ped, 2000, 3000, 0, 1, 1, 0);
  return ok(action, "ragdoll");
}

static uint32_t clear_wanted() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_WANTED;
  if (!g_n.player_id || !g_n.set_player_wanted_level || !g_n.set_player_wanted_level_now) {
    return unavailable(action, "missing wanted-level natives");
  }
  int player = native_player_id();
  invoke_void(g_n.set_player_wanted_level, player, 0, 0);
  invoke_void(g_n.set_player_wanted_level_now, player, 0);
  return ok(action, "wanted level cleared");
}

static uint32_t toggle_super_run() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_RUN;
  if (!g_n.player_id || !g_n.set_run_sprint_multiplier) {
    return unavailable(action, "missing run multiplier native");
  }
  g_state.super_run_enabled = !g_state.super_run_enabled;
  if (!g_state.super_run_enabled) {
    invoke_void(g_n.set_run_sprint_multiplier, native_player_id(), 1.0f);
  }
  return ok(action, g_state.super_run_enabled ? "super run enabled" : "super run disabled");
}

static uint32_t toggle_fast_swim() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_SWIM;
  if (!g_n.player_id || !g_n.set_swim_multiplier) {
    return unavailable(action, "missing swim multiplier native");
  }
  g_state.fast_swim_enabled = !g_state.fast_swim_enabled;
  if (!g_state.fast_swim_enabled) {
    invoke_void(g_n.set_swim_multiplier, native_player_id(), 1.0f);
  }
  return ok(action, g_state.fast_swim_enabled ? "fast swim enabled" : "fast swim disabled");
}

static uint32_t toggle_infinite_ammo() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_AMMO;
  if (!g_n.player_ped_id || !g_n.set_ped_infinite_ammo_clip) {
    return unavailable(action, "missing infinite ammo native");
  }
  g_state.infinite_ammo_enabled = !g_state.infinite_ammo_enabled;
  invoke_void(g_n.set_ped_infinite_ammo_clip, native_player_ped_id(),
              (int)g_state.infinite_ammo_enabled);
  return ok(action,
            g_state.infinite_ammo_enabled ? "infinite ammo enabled" : "infinite ammo disabled");
}

