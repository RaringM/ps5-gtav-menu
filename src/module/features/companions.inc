// Companions / bodyguards.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace.
// Bodyguards are created on the game thread, tracked in a fixed roster, and joined to the
// player's group when the group natives resolve.

struct BodyguardHealthPreset {
  char label[8];
  int value;
};
struct BodyguardArmorPreset {
  char label[8];
  int value;
};
struct BodyguardFormationPreset {
  char label[12];
  int value;
};
// How tracked bodyguards react to threats. Drives SET_BLOCKING_OF_NON_TEMPORARY_EVENTS (a
// blocked ped ignores combat events like the player being shot at -- which is why guards used
// to "just stand there"), the BF_AlwaysFight combat attribute, combat movement (0 stationary /
// 1 defensive / 2 advance) and combat ability (0 poor / 1 average / 2 professional).
struct BodyguardAggressionPreset {
  char label[12];
  uint8_t block_events;     // SET_BLOCKING_OF_NON_TEMPORARY_EVENTS arg (1 = ignore threats)
  uint8_t always_fight;     // combat attribute 46 (BF_AlwaysFight)
  uint8_t movement;         // SET_PED_COMBAT_MOVEMENT
  uint8_t ability;          // SET_PED_COMBAT_ABILITY
  uint8_t engage_nearby;    // 1 = proactively TASK_COMBAT_PED the guard onto nearby peds (hunt)
  uint8_t engage_vehicles;  // 1 = also TASK_SHOOT_AT_ENTITY nearby cars (RAMPAGE)
};
// Loadout the guard is armed with on spawn. An unarmed civilian model cannot shoot back, so
// without a weapon "Defensive" looks inert -- the guards just stand there. hash 0 == UNARMED
// (melee only); otherwise a joaat weapon hash passed straight to GIVE_WEAPON_TO_PED.
struct BodyguardWeaponPreset {
  char label[12];
  uint32_t hash;
  int ammo;
};
struct CruiseSpeedPreset {
  char label[12];
  float mps;
};

static const BodyguardHealthPreset kBodyguardHealthPresets[] = {
    {"100", 100},
    {"200", 200},
    {"500", 500},
    {"1000", 1000},
};
static const BodyguardArmorPreset kBodyguardArmorPresets[] = {
    {"0", 0},
    {"100", 100},
    {"200", 200},
};
static const BodyguardFormationPreset kBodyguardFormationPresets[] = {
    {"DEFAULT", 0},
    {"CIRCLE", 1},
    {"LINE", 2},
};
// RAMPAGE is appended last (index 3) so existing saved profiles -- which store the raw aggression
// index -- keep their meaning (0 DEFENSIVE / 1 AGGRESSIVE / 2 PASSIVE); no profile-version bump.
static const BodyguardAggressionPreset kBodyguardAggression[] = {
    {"DEFENSIVE", 0, 1, 1, 2, 0, 0},   // react to threats and protect the player (default)
    {"AGGRESSIVE", 0, 1, 2, 2, 1, 0},  // proactively hunt and attack any nearby ped
    {"PASSIVE", 1, 0, 0, 1, 0, 0},     // ignore threats entirely, just follow
    {"RAMPAGE", 0, 1, 2, 2, 1, 1},     // hunt nearby peds AND shoot up nearby cars
};
// Weapon hashes match the give-weapon catalog (feature_catalog.c); kept inline so the guard
// loadout needs no GET_HASH_KEY on the game thread. CARBINE is the default so newly spawned
// guards can actually defend the player out of the box.
static const BodyguardWeaponPreset kBodyguardWeapons[] = {
    {"CARBINE", 0x83BF0278u, 500},   // WEAPON_CARBINERIFLE
    {"PISTOL", 0x1B06D571u, 250},    // WEAPON_PISTOL
    {"SMG", 0x2BE6766Bu, 600},       // WEAPON_SMG
    {"MICRO SMG", 0x13532244u, 500}, // WEAPON_MICROSMG
    {"SHOTGUN", 0x1D073A89u, 100},   // WEAPON_PUMPSHOTGUN
    {"UNARMED", 0u, 0},              // melee only
};
static const CruiseSpeedPreset kCruiseSpeeds[] = {
    {"20 KM/H", 5.56f},
    {"40 KM/H", 11.11f},
    {"60 KM/H", 16.67f},
    {"80 KM/H", 22.22f},
    {"120 KM/H", 33.33f},
};

// Bodyguard combat accuracy (SET_PED_ACCURACY hit-chance %). Without this, spawned guards use
// their model's default accuracy, which for civilians is low enough that they barely defend.
struct BodyguardAccuracyPreset {
  char label[8];
  int value;
};
static const BodyguardAccuracyPreset kBodyguardAccuracy[] = {
    {"LOW", 25},
    {"MEDIUM", 50},
    {"HIGH", 75},
    {"DEADLY", 100},
};

enum { GTAV_BODYGUARD_ROSTER_MAX = 7 };
static int g_bodyguard_roster[GTAV_BODYGUARD_ROSTER_MAX];
// Minimap blip handle for each roster slot (0 = none). Kept index-aligned with the roster
// through cull/dismiss so a guard and its blip move together.
static int g_bodyguard_blip[GTAV_BODYGUARD_ROSTER_MAX];
// AGGRESSIVE/RAMPAGE: the entity (ped or car) each guard is currently hunting (0 = none). Sticky
// so a guard commits to its target instead of re-rolling one every refresh -- the fix for guards
// "forgetting" a target the moment they chase it out of the player's scan radius. Index-aligned
// with the roster (carried through cull, cleared on dismiss), same as g_bodyguard_blip.
static int g_bodyguard_target[GTAV_BODYGUARD_ROSTER_MAX];
static int g_bodyguard_count = 0;

static int selected_bodyguard_accuracy() {
  const uint32_t count = (uint32_t)(sizeof(kBodyguardAccuracy) / sizeof(kBodyguardAccuracy[0]));
  return kBodyguardAccuracy[g_bodyguard_accuracy_index % count].value;
}

// Remove the blip in roster slot `idx` (if any). REMOVE_BLIP takes a Blip* out-param, so copy
// the handle to a local first (same pattern as DELETE_PED) and zero our slot afterwards.
static void bodyguard_drop_blip(int idx) {
  int blip = g_bodyguard_blip[idx];
  if (blip && g_n.remove_blip) invoke_void(g_n.remove_blip, (void*)&blip);
  g_bodyguard_blip[idx] = 0;
}

static int selected_bodyguard_health() {
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardHealthPresets) / sizeof(kBodyguardHealthPresets[0]));
  return kBodyguardHealthPresets[g_bodyguard_health_index % count].value;
}

static int selected_bodyguard_armor() {
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardArmorPresets) / sizeof(kBodyguardArmorPresets[0]));
  return kBodyguardArmorPresets[g_bodyguard_armor_index % count].value;
}

static int selected_bodyguard_formation() {
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardFormationPresets) / sizeof(kBodyguardFormationPresets[0]));
  return kBodyguardFormationPresets[g_bodyguard_formation_index % count].value;
}

static float selected_cruise_speed_mps() {
  const uint32_t count = (uint32_t)(sizeof(kCruiseSpeeds) / sizeof(kCruiseSpeeds[0]));
  return kCruiseSpeeds[g_cruise_speed_index % count].mps;
}

static const BodyguardAggressionPreset* selected_bodyguard_aggression() {
  const uint32_t count = (uint32_t)(sizeof(kBodyguardAggression) / sizeof(kBodyguardAggression[0]));
  return &kBodyguardAggression[g_bodyguard_aggression_index % count];
}

static const BodyguardWeaponPreset* selected_bodyguard_weapon() {
  const uint32_t count = (uint32_t)(sizeof(kBodyguardWeapons) / sizeof(kBodyguardWeapons[0]));
  return &kBodyguardWeapons[g_bodyguard_weapon_index % count];
}

// joaat("PLAYER") -- the player's own relationship group. Cached after the first resolve so the
// per-tick re-assert costs nothing. Putting a guard in this group makes it share the player's
// friend/foe (so it engages whoever attacks the player) and stops it ever treating the player as
// a target.
static uint32_t g_guard_relgroup = 0;
static uint32_t bodyguard_relationship_group(void) {
  if (!g_guard_relgroup && g_n.get_hash_key) {
    g_guard_relgroup = (uint32_t)invoke_return<int>(g_n.get_hash_key, "PLAYER");
  }
  return g_guard_relgroup;
}

// Arm (or disarm) a guard with the current weapon preset. Runs on the game thread (spawn config +
// bodyguard tick); GIVE_WEAPON_TO_PED walks the ped weapon manager, so it is never called from the
// worker. equipNow=1 puts the gun in hand; infinite clip keeps them from running dry mid-fight.
static void apply_bodyguard_weapon(int guard, int equip) {
  const BodyguardWeaponPreset* w = selected_bodyguard_weapon();
  if (w->hash == 0u) {
    if (g_n.remove_all_ped_weapons) invoke_void(g_n.remove_all_ped_weapons, guard, 1);
    return;
  }
  if (g_n.give_weapon_to_ped) {
    // equip=1 forces the gun into hand (spawn / loadout change); equip=0 just ensures it stays
    // in inventory for the periodic recovery re-assert, so we don't spam re-draw animations.
    invoke_void(g_n.give_weapon_to_ped, guard, w->hash, w->ammo, 0, equip ? 1 : 0);
  }
  if (g_n.set_ped_infinite_ammo_clip) invoke_void(g_n.set_ped_infinite_ammo_clip, guard, 1);
}

// Apply the current aggression preset's combat behaviour to one guard. Runs on the game
// thread (spawn config + bodyguard tick); combat attributes are flags read by the AI, so
// re-applying is idempotent. We never TASK_COMBAT a target here -- group membership plus an
// un-blocked event stream is what makes a guard defend the player against whoever attacks.
static void apply_bodyguard_combat(int guard) {
  const BodyguardAggressionPreset* a = selected_bodyguard_aggression();
  if (g_n.set_blocking_of_non_temporary_events)
    invoke_void(g_n.set_blocking_of_non_temporary_events, guard, a->block_events);
  if (g_n.set_ped_keep_task) invoke_void(g_n.set_ped_keep_task, guard, 1);
  if (g_n.set_ped_combat_ability) invoke_void(g_n.set_ped_combat_ability, guard, a->ability);
  if (g_n.set_ped_combat_movement) invoke_void(g_n.set_ped_combat_movement, guard, a->movement);
  if (g_n.set_ped_combat_attributes) {
    // GTA V combat behaviour-flag IDs. These were previously swapped here (5 labelled
    // BF_AlwaysFight), which pinned AlwaysFight on for every preset and left PASSIVE unable
    // to stand down. Correct IDs: 46 = BF_AlwaysFight (engage instead of flee), 5 =
    // BF_CanFightArmedPedsWhenNotArmed, 0/1/3 let the guard take cover and use vehicles.
    invoke_void(g_n.set_ped_combat_attributes, guard, 46, a->always_fight);
    invoke_void(g_n.set_ped_combat_attributes, guard, 5, 1);
    invoke_void(g_n.set_ped_combat_attributes, guard, 0, 1);  // BF_CanUseCover
    invoke_void(g_n.set_ped_combat_attributes, guard, 1, 1);  // BF_CanUseVehicles
    invoke_void(g_n.set_ped_combat_attributes, guard, 3, 1);  // BF_CanLeaveVehicle
  }
  // Share the player's friend/foe so the guard treats whoever attacks the player as a threat
  // (and never the player). Without this a spawned civilian keeps its model's neutral group and
  // just watches the player get shot -- the main reason "Defensive" looked inert.
  if (g_n.set_ped_relationship_group_hash) {
    uint32_t rg = bodyguard_relationship_group();
    if (rg) invoke_void(g_n.set_ped_relationship_group_hash, guard, rg);
  }
  if (g_n.set_ped_accuracy) invoke_void(g_n.set_ped_accuracy, guard, selected_bodyguard_accuracy());
  apply_bodyguard_weapon(guard, /*equip=*/1);
}

static void bodyguard_roster_cull(void) {
  int out = 0;
  for (int i = 0; i < g_bodyguard_count; ++i) {
    if (roster_handle_alive(g_bodyguard_roster[i])) {
      // Carry the guard, its blip and its current target forward together (out <= i, so this
      // never clobbers an unread slot).
      g_bodyguard_roster[out] = g_bodyguard_roster[i];
      g_bodyguard_blip[out] = g_bodyguard_blip[i];
      g_bodyguard_target[out] = g_bodyguard_target[i];
      ++out;
    } else {
      bodyguard_drop_blip(i);  // guard died -> drop its minimap blip
    }
  }
  for (int i = out; i < GTAV_BODYGUARD_ROSTER_MAX; ++i) {
    g_bodyguard_roster[i] = 0;
    g_bodyguard_blip[i] = 0;
    g_bodyguard_target[i] = 0;
  }
  g_bodyguard_count = out;
}

static void bodyguard_roster_add(int ped) {
  if (ped <= 0) return;
  bodyguard_roster_cull();
  if (g_bodyguard_count >= GTAV_BODYGUARD_ROSTER_MAX) return;
  g_bodyguard_roster[g_bodyguard_count++] = ped;
}

// Is this ped one of the tracked bodyguards? Used by ped-control (included after this fragment)
// to keep "everyone attacks me" / flee / ragdoll from retasking the player's own guards.
static int handle_is_bodyguard(int ped) {
  if (ped <= 0) return 0;
  for (int i = 0; i < g_bodyguard_count; ++i) {
    if (g_bodyguard_roster[i] == ped) return 1;
  }
  return 0;
}

static int bodyguard_natives_ready(void) {
  return g_n.create_ped && g_n.request_model && g_n.has_model_loaded && g_n.player_ped_id &&
         g_n.player_id && g_n.get_entity_coords && g_n.get_player_group &&
         g_n.set_ped_as_group_member;
}

static void configure_bodyguard(int guard, int player_ped) {
  if (g_n.set_entity_as_mission_entity) invoke_void(g_n.set_entity_as_mission_entity, guard, 1, 1);
  if (g_n.set_ped_max_health) invoke_void(g_n.set_ped_max_health, guard, selected_bodyguard_health());
  if (g_n.set_entity_health) invoke_void(g_n.set_entity_health, guard, selected_bodyguard_health());
  if (g_n.set_ped_armour) invoke_void(g_n.set_ped_armour, guard, selected_bodyguard_armor());
  if (g_n.set_entity_invincible) {
    invoke_void(g_n.set_entity_invincible, guard, g_bodyguard_invincible_enabled ? 1 : 0);
  }
  apply_bodyguard_combat(guard);

  int player = valid_player_id();
  if (player >= 0 && g_n.get_player_group && g_n.set_ped_as_group_member) {
    int group = invoke_return<int>(g_n.get_player_group, player);
    if (group) {
      invoke_void(g_n.set_ped_as_group_member, guard, group);
      if (g_n.set_ped_never_leaves_group) invoke_void(g_n.set_ped_never_leaves_group, guard, 1);
      if (g_n.set_group_formation) invoke_void(g_n.set_group_formation, group, selected_bodyguard_formation());
      if (g_n.set_group_separation_range) invoke_void(g_n.set_group_separation_range, group, 25.0f);
    }
  }
  if (g_n.task_combat_ped && player_ped > 0) {
    // Keeps the AI awake without making it hostile to the player; the group membership still
    // drives normal follow/protect behaviour.
    if (g_n.set_ped_keep_task) invoke_void(g_n.set_ped_keep_task, guard, 1);
  }
}

static uint32_t spawn_bodyguard_model_hash(uint32_t model, int allow_block) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD;
  g_state.last_param = model;
  bodyguard_roster_cull();
  if (g_bodyguard_count >= GTAV_BODYGUARD_ROSTER_MAX) return failed(action, "bodyguard roster full");
  if (!bodyguard_natives_ready()) return unavailable(action, "missing bodyguard natives");
  if (g_n.is_model_valid && !invoke_return<int>(g_n.is_model_valid, model)) {
    return failed(action, "model not valid");
  }
  if (g_n.is_model_in_cdimage && !invoke_return<int>(g_n.is_model_in_cdimage, model)) {
    return failed(action, "model not in cdimage");
  }
  if (!wait_for_model(model, allow_block)) return failed(action, "model not loaded");

  int player_ped = native_player_ped_id();
  if (player_ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  Vec3 pos = invoke_vector(g_n.get_entity_coords, player_ped, 1);
  const float sx = pos.x + 2.0f + (float)(g_bodyguard_count % 3);
  const float sy = pos.y + 2.0f + (float)(g_bodyguard_count / 3);
  const float sz = pos.z;
  const int worker = allow_block;
  int guard = invoke_return_ctx<int>(worker, g_n.create_ped, 4, (int)model, sx, sy, sz, 0.0f, 0, 0);
  if (!guard) {
    if (g_n.set_model_as_no_longer_needed)
      invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
    return failed(action, "create bodyguard failed");
  }
  configure_bodyguard(guard, player_ped);
  if (g_n.set_model_as_no_longer_needed)
    invoke_void_ctx(worker, g_n.set_model_as_no_longer_needed, model);
  bodyguard_roster_add(guard);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "bodyguard spawned (%d/%d)", g_bodyguard_count,
           GTAV_BODYGUARD_ROSTER_MAX);
  return ok(action, msg);
}

static void run_bodyguard_spawn_job(uint32_t model) {
  static ModelStreamJob job;
  // One gate, which also adopts the model and arms the wall-clock budget. This job used to arm its
  // own deadline ABOVE the gate, where a queue-full requeue cleared `pending` and the next fire
  // re-armed it -- so the budget could be pushed out indefinitely.
  if (model_stream_defer_to_next_frame(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, model, &job))
    return;
  if (!bodyguard_natives_ready()) {
    job.pending = 0;
    unavailable(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "missing bodyguard natives");
    return;
  }
  if (g_n.is_model_valid && !invoke_return_ctx<int>(0, g_n.is_model_valid, model)) {
    job.pending = 0;
    failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "model not valid");
    return;
  }
  if (g_n.is_model_in_cdimage && !invoke_return_ctx<int>(0, g_n.is_model_in_cdimage, model)) {
    job.pending = 0;
    failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "model not in cdimage");
    return;
  }
  switch (model_stream_step(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, model, &job)) {
    case MODEL_STREAM_LOADED:
      break;
    case MODEL_STREAM_PENDING:
      if (model_stream_should_report_pending(&job))
        ok(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "model pending");
      return;
    case MODEL_STREAM_QUEUE_FULL:
      failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "main-thread job queue full");
      return;
    case MODEL_STREAM_NO_HOOK:
      failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "frame hook unavailable");
      return;
    case MODEL_STREAM_TIMEOUT:
      failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "model not loaded");
      return;
    case MODEL_STREAM_NOT_RESIDENT:
      failed(GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD, "model not streamed in");
      return;
  }
  spawn_bodyguard_model_hash(model, /*allow_block=*/0);
  job.pending = 0;
}

static uint32_t cycle_bodyguard_health(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_HEALTH;
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardHealthPresets) / sizeof(kBodyguardHealthPresets[0]));
  stage_step(&g_bodyguard_health_index, count, param);
  return ok(action, kBodyguardHealthPresets[g_bodyguard_health_index % count].label);
}

static uint32_t cycle_bodyguard_armor(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ARMOR;
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardArmorPresets) / sizeof(kBodyguardArmorPresets[0]));
  stage_step(&g_bodyguard_armor_index, count, param);
  return ok(action, kBodyguardArmorPresets[g_bodyguard_armor_index % count].label);
}

static uint32_t cycle_bodyguard_formation(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_FORMATION;
  const uint32_t count =
      (uint32_t)(sizeof(kBodyguardFormationPresets) / sizeof(kBodyguardFormationPresets[0]));
  if (param == 1u || param == 2u) {
    g_bodyguard_formation_index =
        list_step(g_bodyguard_formation_index, count, param == 2u ? -1 : 1);
  }
  return ok(action, kBodyguardFormationPresets[g_bodyguard_formation_index % count].label);
}

static uint32_t cycle_bodyguard_aggression(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_AGGRESSION;
  const uint32_t count = (uint32_t)(sizeof(kBodyguardAggression) / sizeof(kBodyguardAggression[0]));
  if (param == 1u || param == 2u) {
    g_bodyguard_aggression_index =
        list_step(g_bodyguard_aggression_index, count, param == 2u ? -1 : 1);
    // Re-apply to already-spawned guards on the next game-thread tick (worker-direct combat
    // natives on existing peds are not safe -- see configure path).
    g_bodyguard_combat_dirty = 1;
  }
  return ok(action, kBodyguardAggression[g_bodyguard_aggression_index % count].label);
}

static uint32_t cycle_bodyguard_weapon(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_WEAPON;
  const uint32_t count = (uint32_t)(sizeof(kBodyguardWeapons) / sizeof(kBodyguardWeapons[0]));
  if (param == 1u || param == 2u) {
    g_bodyguard_weapon_index = list_step(g_bodyguard_weapon_index, count, param == 2u ? -1 : 1);
    // Re-arm already-spawned guards on the next game-thread tick (give-weapon off the worker is
    // not safe -- see apply_bodyguard_weapon).
    g_bodyguard_combat_dirty = 1;
  }
  return ok(action, kBodyguardWeapons[g_bodyguard_weapon_index % count].label);
}

static uint32_t cycle_bodyguard_accuracy(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ACCURACY;
  const uint32_t count = (uint32_t)(sizeof(kBodyguardAccuracy) / sizeof(kBodyguardAccuracy[0]));
  if (param == 1u || param == 2u) {
    g_bodyguard_accuracy_index = list_step(g_bodyguard_accuracy_index, count, param == 2u ? -1 : 1);
    g_bodyguard_combat_dirty = 1;  // push to existing guards on the next game-thread tick
  }
  return ok(action, kBodyguardAccuracy[g_bodyguard_accuracy_index % count].label);
}

static uint32_t toggle_bodyguard_invincible() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_INVINCIBLE;
  if (!g_n.set_entity_invincible) return unavailable(action, "missing invincible native");
  g_bodyguard_invincible_enabled = !g_bodyguard_invincible_enabled;
  return ok(action, g_bodyguard_invincible_enabled ? "bodyguards invincible"
                                                  : "bodyguards vulnerable");
}

static uint32_t toggle_bodyguard_blips() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_BLIPS;
  if (!g_n.add_blip_for_entity || !g_n.remove_blip) return unavailable(action, "missing blip natives");
  g_bodyguard_blips_enabled = !g_bodyguard_blips_enabled;
  // The blips themselves are created/removed on the next game-thread bodyguard tick (HUD natives
  // mutate render state, so they ride the tick, not the worker).
  return ok(action, g_bodyguard_blips_enabled ? "guard blips on" : "guard blips off");
}

static uint32_t bring_bodyguards() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS;
  if (!g_n.player_ped_id || !g_n.get_entity_coords ||
      !(g_n.set_entity_coords_no_offset || g_n.set_entity_coords)) {
    return unavailable(action, "missing bodyguard bring natives");
  }
  int player_ped = native_player_ped_id();
  if (player_ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  Vec3 pos = invoke_vector(g_n.get_entity_coords, player_ped, 1);
  bodyguard_roster_cull();
  uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  for (int i = 0; i < g_bodyguard_count; ++i) {
    const float ox = 1.5f + (float)(i % 3);
    const float oy = 1.5f + (float)(i / 3);
    if (g_n.set_entity_coords_no_offset) {
      invoke_void(set_coords, g_bodyguard_roster[i], pos.x + ox, pos.y + oy, pos.z, 0, 0, 1);
    } else {
      invoke_void(set_coords, g_bodyguard_roster[i], pos.x + ox, pos.y + oy, pos.z, 0, 0, 0, 0);
    }
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "brought %d bodyguards", g_bodyguard_count);
  return ok(action, msg);
}

static int dismiss_bodyguards_core(void) {
  if (!g_n.delete_ped) return -1;
  int deleted = 0;
  bodyguard_roster_cull();
  for (int i = 0; i < g_bodyguard_count; ++i) {
    int guard = g_bodyguard_roster[i];
    bodyguard_drop_blip(i);  // remove the minimap blip before the handle goes away
    if (!roster_handle_alive(guard)) continue;
    if (g_n.set_entity_as_mission_entity) invoke_void(g_n.set_entity_as_mission_entity, guard, 1, 1);
    invoke_void(g_n.delete_ped, (void*)&guard);
    ++deleted;
  }
  memset(g_bodyguard_roster, 0, sizeof(g_bodyguard_roster));
  memset(g_bodyguard_blip, 0, sizeof(g_bodyguard_blip));
  memset(g_bodyguard_target, 0, sizeof(g_bodyguard_target));
  g_bodyguard_count = 0;
  return deleted;
}

static uint32_t dismiss_bodyguards() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS;
  int deleted = dismiss_bodyguards_core();
  if (deleted < 0) return unavailable(action, "missing delete ped native");
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "dismissed %d bodyguards", deleted);
  return ok(action, msg);
}

static uint32_t clear_spawned_all_with_bodyguards() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL;
  int vehicles = delete_spawned_roster(g_spawned_vehicle_roster, &g_spawned_vehicle_count, 0);
  int peds = delete_spawned_roster(g_spawned_ped_roster, &g_spawned_ped_count, 1);
  int objects = delete_spawned_roster(g_spawned_object_roster, &g_spawned_object_count, 0);
  int guards = dismiss_bodyguards_core();
  if (vehicles < 0 || objects < 0) return unavailable(action, "missing delete native");
  if (peds < 0 || guards < 0) return unavailable(action, "missing delete ped native");
  g_last_spawned_vehicle = 0;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "deleted %d spawned entities", vehicles + peds + objects + guards);
  return ok(action, msg);
}

static uint32_t cycle_cruise_speed(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CRUISE_SPEED;
  const uint32_t count = (uint32_t)(sizeof(kCruiseSpeeds) / sizeof(kCruiseSpeeds[0]));
  stage_step(&g_cruise_speed_index, count, param);
  return ok(action, kCruiseSpeeds[g_cruise_speed_index % count].label);
}

static uint32_t toggle_cruise_control() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_CRUISE_CONTROL;
  if (!g_n.set_vehicle_forward_speed || !g_n.get_vehicle_ped_is_in) {
    return unavailable(action, "missing cruise natives");
  }
  g_cruise_control_enabled = !g_cruise_control_enabled;
  return ok(action, g_cruise_control_enabled ? "cruise control on" : "cruise control off");
}

// Create the minimap blip for roster slot `idx` (green, friendly) if blips are enabled and the
// slot has none yet. No-op when the natives are unresolved.
static void bodyguard_ensure_blip(int idx, int guard) {
  if (g_bodyguard_blip[idx] || !g_n.add_blip_for_entity) return;
  int blip = invoke_return<int>(g_n.add_blip_for_entity, guard);
  if (!blip) return;
  g_bodyguard_blip[idx] = blip;
  if (g_n.set_blip_sprite) invoke_void(g_n.set_blip_sprite, blip, 1);       // standard dot
  if (g_n.set_blip_as_friendly) invoke_void(g_n.set_blip_as_friendly, blip, 1);
  if (g_n.set_blip_colour) invoke_void(g_n.set_blip_colour, blip, 2);       // green = ally
}

// joaat("FIRING_PATTERN_FULL_AUTO") -- the firing pattern handed to TASK_SHOOT_AT_ENTITY so guards
// hose a targeted car on full auto instead of slow, single shots.
static const uint32_t GTAV_FIRING_PATTERN_FULL_AUTO = 0xC6EE6B4Cu;

// Is a tracked target handle still worth attacking (exists and not wrecked/dead)? Works for both
// peds and vehicles -- IS_ENTITY_DEAD reports a blown-up car as dead too.
static int bodyguard_target_alive(int h) {
  if (h <= 0) return 0;
  if (g_n.does_entity_exist && !invoke_return<int>(g_n.does_entity_exist, h)) return 0;
  if (g_n.is_entity_dead && invoke_return<int>(g_n.is_entity_dead, h)) return 0;
  return 1;
}

// Task one guard to attack a target, picking the right native for its kind: a car gets
// TASK_SHOOT_AT_ENTITY (TASK_COMBAT_PED expects a ped and does nothing useful with a vehicle
// handle); a ped gets TASK_COMBAT_PED. KEEP_TASK (set in apply_bodyguard_combat) holds the task
// between refreshes so the guard stays on target between ticks.
static void bodyguard_task_engage(int guard, int target) {
  if (g_n.is_entity_a_vehicle && invoke_return<int>(g_n.is_entity_a_vehicle, target)) {
    if (g_n.task_shoot_at_entity)
      invoke_void(g_n.task_shoot_at_entity, guard, target, 3000, GTAV_FIRING_PATTERN_FULL_AUTO);
  } else if (g_n.task_combat_ped) {
    invoke_void(g_n.task_combat_ped, guard, target, 0, 16);
  }
}

// AGGRESSIVE / RAMPAGE: make the guards seek out and open fire instead of waiting to be provoked.
// Default/Passive guards only fight once a combat event reaches them (the player is shot at) --
// that group-defense path is reactive, which is why "Aggressive" looked like it only kicked in
// when the player was attacked. Here we gather peds (and, in RAMPAGE, cars) near the player and
// assign each guard a target.
//
// Targets are *sticky*: a guard keeps attacking its current target as long as that target lives,
// rolling a new one only when it dies or despawns. Without this the per-refresh round-robin
// re-pointed each guard at a different ped every ~0.5s, so a guard that chased someone out of the
// player's scan radius would "forget" it and drift back -- exactly the reported bug.
//
// Runs on the game thread (pool walk + ped-task mutation, same lane as ped_control.inc).
static void bodyguard_aggressive_engage(void) {
  if (!g_n.get_ped_nearby_peds || !g_n.task_combat_ped) return;
  int player = native_player_ped_id();
  if (player <= 0) return;
  const BodyguardAggressionPreset* a = selected_bodyguard_aggression();

  // Candidate pool: the nearby peds (up to 96) plus the occupied cars derived from them (up to 32).
  int targets[96 + 32];
  int ntargets = 0;
  const int cap = (int)(sizeof(targets) / sizeof(targets[0]));

  // Nearby peds: sizeAndPeds buffer, [0] = capacity in / count out, [1..96] = handles. Skip the
  // player, our own guards (never turn a guard on the squad), and corpses.
  int buf[97];
  memset(buf, 0, sizeof(buf));
  buf[0] = 96;
  invoke_void(g_n.get_ped_nearby_peds, player, &buf[0], -1);
  for (int i = 1; i < 97 && ntargets < cap; ++i) {
    int h = buf[i];
    if (h <= 0) continue;
    if (g_n.is_ped_a_player && invoke_return<int>(g_n.is_ped_a_player, h)) continue;
    if (handle_is_bodyguard(h)) continue;
    if (g_n.is_entity_dead && invoke_return<int>(g_n.is_entity_dead, h)) continue;
    targets[ntargets++] = h;
  }

  // RAMPAGE also attacks OCCUPIED cars near the player (empty/parked cars are left alone). Rather
  // than scan the vehicle pool and risk GET_PED_IN_VEHICLE_SEAT spawning an ambient occupant just
  // to test a seat, we derive the cars from the peds we just gathered: any nearby ped riding a car
  // marks that car as a live target. That guarantees occupancy, and because the player + guards
  // were filtered out above, our own ride (and a guard's) never lands in the set. Dedup so a full
  // car isn't over-represented in the round-robin.
  if (a->engage_vehicles && g_n.get_vehicle_ped_is_in && g_n.task_shoot_at_entity) {
    const int nped = ntargets;
    int player_veh = invoke_return<int>(g_n.get_vehicle_ped_is_in, player, 0);
    for (int p = 0; p < nped && ntargets < cap; ++p) {
      int veh = invoke_return<int>(g_n.get_vehicle_ped_is_in, targets[p], 0);
      if (veh <= 0 || veh == player_veh) continue;
      int seen = 0;
      for (int j = nped; j < ntargets; ++j) {
        if (targets[j] == veh) {
          seen = 1;
          break;
        }
      }
      if (!seen) targets[ntargets++] = veh;
    }
  }

  // Assign: keep a guard's living target, else hand it the next pool entry (round-robined so the
  // squad spreads across the crowd instead of dogpiling one). `next` advances only on a fresh
  // assignment, so guards already locked on don't perturb the distribution.
  int next = 0;
  for (int i = 0; i < g_bodyguard_count; ++i) {
    int guard = g_bodyguard_roster[i];
    if (!roster_handle_alive(guard)) {
      g_bodyguard_target[i] = 0;
      continue;
    }
    int target = g_bodyguard_target[i];
    if (!bodyguard_target_alive(target)) {
      target = ntargets ? targets[next++ % ntargets] : 0;
      g_bodyguard_target[i] = target;
    }
    if (target > 0) bodyguard_task_engage(guard, target);
  }
}

static void bodyguard_game_thread_tick(void) {
  if (!g_bodyguard_count) return;
  bodyguard_roster_cull();
  if (!g_bodyguard_count) return;
  int player = valid_player_id();
  int group = 0;
  if (player >= 0 && g_n.get_player_group) group = invoke_return<int>(g_n.get_player_group, player);
  // Re-assert the loadout + relationship group periodically (not every frame) so a guard that
  // gets disarmed or has its relationship reset recovers, without churning the weapon manager.
  // The tick is already collapsed to one call per rendered frame, so ~every 64 frames is ~1s.
  static uint32_t s_reassert_div;
  const int reassert = ((s_reassert_div++ & 63u) == 0u);
  for (int i = 0; i < g_bodyguard_count; ++i) {
    int guard = g_bodyguard_roster[i];
    if (g_bodyguard_invincible_enabled && g_n.set_entity_invincible) {
      invoke_void(g_n.set_entity_invincible, guard, 1);
    }
    // The aggression/weapon/accuracy presets only change occasionally, so push the full combat
    // config to existing guards on the tick after a change; otherwise do the light recovery.
    if (g_bodyguard_combat_dirty) {
      apply_bodyguard_combat(guard);
      // Switched out of a proactive mode -> drop any leftover hunt task + target so the guard
      // returns to following/reactive defense instead of finishing off its last target.
      if (!selected_bodyguard_aggression()->engage_nearby) {
        g_bodyguard_target[i] = 0;
        if (g_n.clear_ped_tasks) invoke_void(g_n.clear_ped_tasks, guard);
      }
    } else if (reassert) {
      apply_bodyguard_weapon(guard, /*equip=*/0);
      if (g_n.set_ped_relationship_group_hash) {
        uint32_t rg = bodyguard_relationship_group();
        if (rg) invoke_void(g_n.set_ped_relationship_group_hash, guard, rg);
      }
    }
    if (group && g_n.set_ped_as_group_member) {
      invoke_void(g_n.set_ped_as_group_member, guard, group);
      if (g_n.set_ped_never_leaves_group) invoke_void(g_n.set_ped_never_leaves_group, guard, 1);
    }
    if (g_bodyguard_blips_enabled) {
      bodyguard_ensure_blip(i, guard);
    } else if (g_bodyguard_blip[i]) {
      bodyguard_drop_blip(i);
    }
  }
  g_bodyguard_combat_dirty = 0;
  if (group && g_n.set_group_formation) {
    invoke_void(g_n.set_group_formation, group, selected_bodyguard_formation());
  }
  // AGGRESSIVE: refresh combat targets ~every 32 frames (~0.5s). Throttled so a guard mid-fight
  // keeps its task instead of restarting it every frame (which would stutter the chase) while
  // still picking up a fresh target after it downs one. Other presets stay reactive.
  if (selected_bodyguard_aggression()->engage_nearby) {
    static uint32_t s_engage_div;
    if ((s_engage_div++ & 31u) == 0u) bodyguard_aggressive_engage();
  }
}
