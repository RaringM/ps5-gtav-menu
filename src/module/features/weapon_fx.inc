// Weapon effects: explosive / fire ammo.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace
// (shares g_n / g_state / the invoke helpers / Vec3). Not a standalone TU.
//
// The toggles only flip a worker-side flag. The natives that DO the work create entities
// (ADD_OWNED_EXPLOSION / START_SCRIPT_FIRE), so they crash off the game thread just like
// CREATE_VEHICLE -- they run exclusively from gtav_features_game_thread_tick(), on the
// game/script thread via the frame hook. Toggling is therefore refused until the hook is
// live (and reports unavailable in a build without the frame hook).

// Diagnostic counters. This feature fails *silently* -- no crash, just nothing spawns --
// so the raw counters below are surfaced on the Debug page (gtav_features_weapon_fx_stats)
// to make the failure observable and bisect the cause on hardware:
//   tick_fired       climbs only if the per-frame game-thread tick actually runs.
//   impact_hit       climbs only if GET_PED_LAST_WEAPON_IMPACT_COORD returned a fresh hit.
//   effect_requested climbs only if ADD_OWNED_EXPLOSION / START_SCRIPT_FIRE was called.
// Reading them while firing localizes the failure:
//   tick_fired == 0                      -> the frame-hook tick is not firing (hook dead).
//   tick_fired > 0, impact_hit == 0      -> impact-coord native wrong / no impacts seen.
//   effect_requested > 0, nothing spawns -> explosion/fire native address is wrong.
// Always compiled (even without the frame hook) so the accessor link is unconditional;
// they simply stay 0 when the tick can never run.
static volatile uint32_t g_wfx_tick_fired;
static volatile uint32_t g_wfx_impact_hit;
static volatile uint32_t g_wfx_effect_requested;

// Last impact coordinate read back from GET_PED_LAST_WEAPON_IMPACT_COORD -- the exact
// point an explosion/fire is requested at. Surfaced on the Debug page next to the
// counters so a wrong impact-coord native is visible without guessing: with impact_hit
// climbing, sane values near the player's world position mean the native is good (look
// at the explosion path instead), whereas 0/0/0 or absurd magnitudes mean the
// impact-coord native (address 0x199ded0) is returning garbage and is the bug. Always
// compiled like the counters; stays 0 when the tick can never run.
static volatile float g_wfx_last_x;
static volatile float g_wfx_last_y;
static volatile float g_wfx_last_z;

extern "C" void gtav_features_weapon_fx_stats(uint32_t* tick_fired, uint32_t* impact_hit,
                                              uint32_t* effect_requested) {
  if (tick_fired) *tick_fired = g_wfx_tick_fired;
  if (impact_hit) *impact_hit = g_wfx_impact_hit;
  if (effect_requested) *effect_requested = g_wfx_effect_requested;
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Last impact we acted on (game-thread-only state), used to dedup the latched coord.
// Declared before the toggles so flipping a toggle off can clear it (see below).
static Vec3 g_last_impact;
static int g_have_last_impact = 0;
#endif

static uint32_t toggle_explosive_ammo() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_AMMO;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.add_owned_explosion || !g_n.get_ped_last_weapon_impact_coord) {
    return unavailable(action, "missing explosion natives");
  }
  if (!gtav_frame_hook_is_active()) {
    return unavailable(action, "needs main-thread hook");
  }
  g_explosive_ammo_enabled = !g_explosive_ammo_enabled;
  // Mutually exclusive with Fire Ammo: the per-impact cascade is an else-if (explosive wins), so
  // leaving both on silently suppresses fire. Clear the other so the active one always applies.
  if (g_explosive_ammo_enabled) { g_fire_ammo_enabled = 0; g_active_gun = 0; }
  // Drop the latched impact so the first shot after re-enabling is not deduped away.
  g_have_last_impact = 0;
  return ok(action, g_explosive_ammo_enabled ? "explosive ammo on" : "explosive ammo off");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

static uint32_t toggle_fire_ammo() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_FIRE_AMMO;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.start_script_fire || !g_n.get_ped_last_weapon_impact_coord) {
    return unavailable(action, "missing fire natives");
  }
  if (!gtav_frame_hook_is_active()) {
    return unavailable(action, "needs main-thread hook");
  }
  g_fire_ammo_enabled = !g_fire_ammo_enabled;
  // Mutually exclusive with Explosive Ammo (see toggle_explosive_ammo) -- the else-if cascade lets
  // explosive win, so clearing it here keeps fire from being silently suppressed.
  if (g_fire_ammo_enabled) { g_explosive_ammo_enabled = 0; g_active_gun = 0; }
  // Drop the latched impact so the first shot after re-enabling is not deduped away.
  g_have_last_impact = 0;
  return ok(action, g_fire_ammo_enabled ? "fire ammo on" : "fire ammo off");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

// Explosive Melee ("explosive punch"): while enabled, the game-thread tick spawns a small owned
// explosion at each unarmed melee impact. Reuses the explosive-ammo machinery (the entity-creating
// ADD_OWNED_EXPLOSION runs only on the game/script thread), so toggling refuses until the hook is
// live, exactly like Explosive Ammo.
static uint32_t toggle_explosive_melee() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_MELEE;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.add_owned_explosion || !g_n.get_ped_last_weapon_impact_coord ||
      !g_n.get_current_ped_weapon) {
    return unavailable(action, "missing melee/explosion natives");
  }
  if (!gtav_frame_hook_is_active()) {
    return unavailable(action, "needs main-thread hook");
  }
  g_explosive_melee_enabled = !g_explosive_melee_enabled;
  if (g_explosive_melee_enabled) g_active_gun = 0;  // an active gun owns the shot (mutual excl.)
  g_have_last_impact = 0;  // don't dedup the first post-enable hit
  return ok(action, g_explosive_melee_enabled ? "explosive melee on" : "explosive melee off");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

// --- Combat extras: spawn-explosion actions + weapon-damage multiplier ---

// One ROCKET-type explosion owned by the player ped. ADD_OWNED_EXPLOSION creates an
// entity, so these run on the game/script thread via run_gated_action (gated like
// SPAWN_VEHICLE), never worker-direct. "At player" also isolates ADD_OWNED_EXPLOSION for
// the explosive-ammo diagnosis: if it works but explosive ammo does not, the impact-coord
// native is the culprit. type 4 = ROCKET; owned by the ped so it raises no wanted level.
static uint32_t spawn_explosion_at_player() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER;
  if (!g_n.add_owned_explosion || !g_n.get_entity_coords || !g_n.player_ped_id) {
    return unavailable(action, "missing explosion natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  int entity = current_player_entity();
  Vec3 c = invoke_vector(g_n.get_entity_coords, entity ? entity : ped, 1);
  invoke_void(g_n.add_owned_explosion, ped, c.x, c.y, c.z, 4, 1.0f, 1, 0, 0.5f);
  return ok(action, "explosion at player");
}

static uint32_t spawn_explosion_at_waypoint() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT;
  if (!g_n.add_owned_explosion || !g_n.get_first_blip_info_id || !g_n.get_blip_coords ||
      !g_n.get_entity_coords || !g_n.player_ped_id) {
    return unavailable(action, "missing explosion/waypoint natives");
  }
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  // Waypoint blips are registered under blip type/sprite id 8 (BLIP_WAYPOINT).
  int blip = invoke_return<int>(g_n.get_first_blip_info_id, 8);
  if (blip == 0 ||
      (g_n.get_blip_info_id_type && invoke_return<int>(g_n.get_blip_info_id_type, blip) == 0)) {
    return unavailable(action, "no waypoint set");
  }
  Vec3 wp = invoke_vector_blip(g_n.get_blip_coords, blip);
  // A waypoint only carries x/y; use the player's current z as the blast height.
  Vec3 cur = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.add_owned_explosion, ped, wp.x, wp.y, cur.z, 4, 1.0f, 1, 0, 0.5f);
  return ok(action, "explosion at waypoint");
}

// Outgoing weapon-damage multipliers cycled by CYCLE_WEAPON_DAMAGE. x1 = the engine
// default (off); the high end is an effective one-shot kill. Inline char arrays (no
// relocation) per feature_catalog.h.
struct WeaponDamageMult {
  char label[16];
  float mult;
};
static const WeaponDamageMult kWeaponDamageMults[] = {
    {"x1 (off)", 1.0f}, {"x2", 2.0f},   {"x5", 5.0f},
    {"x10", 10.0f},     {"x25", 25.0f}, {"x100 (1-shot)", 100.0f},
};

// Stage/apply cycler (param: 1 = next, 2 = prev, 0 = apply). list_step + the worker-safe
// SET_PLAYER_WEAPON_DAMAGE_MODIFIER, re-asserted each tick by apply_weapon_damage_tick().
static uint32_t cycle_weapon_damage(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_DAMAGE;
  const uint32_t count = (uint32_t)(sizeof(kWeaponDamageMults) / sizeof(kWeaponDamageMults[0]));
  if (stage_step(&g_weapon_damage_index, count, param))
    return ok(action, kWeaponDamageMults[g_weapon_damage_index].label);
  if (!g_n.set_player_weapon_damage_modifier || !g_n.player_id) {
    return unavailable(action, "missing weapon-damage native");
  }
  const WeaponDamageMult& m = kWeaponDamageMults[g_weapon_damage_index % count];
  invoke_void(g_n.set_player_weapon_damage_modifier, native_player_id(), m.mult);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "weapon damage %s", m.label);
  return ok(action, msg);
}

// Re-assert the chosen multiplier from the worker tick (the engine resets it on weapon
// switch). No-op at x1 so we don't churn the native every tick when the feature is off.
static void apply_weapon_damage_tick() {
  const uint32_t count = (uint32_t)(sizeof(kWeaponDamageMults) / sizeof(kWeaponDamageMults[0]));
  const uint32_t idx = g_weapon_damage_index % count;
  if (idx == 0u) return;
  if (!g_n.set_player_weapon_damage_modifier || !g_n.player_id) return;
  invoke_void(g_n.set_player_weapon_damage_modifier, native_player_id(), kWeaponDamageMults[idx].mult);
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// GET_PED_LAST_WEAPON_IMPACT_COORD(ped, Vector3* out) -> BOOL. The out-param is a rage
// scrVector: three floats each in an 8-byte script slot, so pass a 3x u64 buffer and read
// x/y/z from slots [0]/[1]/[2] (the same layout invoke_vector reads back from returns[]).
static int last_weapon_impact(int ped, Vec3* out) {
  uint64_t slots[3] = {0, 0, 0};
  int hit = invoke_return<int>(g_n.get_ped_last_weapon_impact_coord, ped, (void*)slots);
  out->x = *reinterpret_cast<const float*>(&slots[0]);
  out->y = *reinterpret_cast<const float*>(&slots[1]);
  out->z = *reinterpret_cast<const float*>(&slots[2]);
  return hit;
}

// While the menu is open we navigate with the controller, so GTA still sees the same
// physical button presses and would act on them (D-pad Left cycles the radio in a vehicle,
// Cross jumps, Circle changes camera/cover, etc.). DISABLE_CONTROL_ACTION(group, control, 1)
// raises a per-frame "disabled" flag the engine honors that same frame -- but only when
// issued in real script context, so we run it HERE on the frame-hook game thread every
// frame the menu is visible (the old worker-thread call missed the radio/context controls
// and raced the game's own per-frame read). We disable only the menu-navigation buttons and
// the per-context inputs they map to; walk/run/look/aim/fire stay enabled so the player can
// still move with the menu open. IDs are GTA V INPUT_* (ref/.../Enums.h); group 0 = PLAYER,
// group 2 = FRONTEND.
static void suppress_menu_input_controls(void) {
  if (!g_n.disable_control_action) return;
  // DISABLE_CONTROL_ACTION suppresses a control for the CURRENT FRAME, so this has to run
  // every frame -- but exactly once. The frame hook chains a native the scripts call ~137
  // times per rendered frame, and this loop issues one native per suppressed control, so
  // ungated it fired ~33 x 8.2k = ~271k DISABLE_CONTROL_ACTION calls a SECOND on the game
  // thread, and only while the menu was open. That flood hung GTA while driving with the
  // menu up (live 2026-09-17: hook call count frozen, drain idle and g_draining == 0, so
  // the game thread had stopped outside our code). Its own static -- deliberately not the
  // streaming gate's, which would make the two steal frames from each other.
  if (g_n.get_frame_count) {
    static uint32_t s_last_frame;
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == s_last_frame) return;  // already suppressed this frame
    s_last_frame = frame;
  }
  static const struct {
    int group;
    int control;
  } kSuppress[] = {
      // D-pad Up -> phone / frontend up / cellphone up / script pad up
      {0, 27}, {0, 188}, {2, 188}, {2, 172}, {0, 232},
      // D-pad Down -> frontend/cellphone down / character wheel / cinematic cam / script pad
      {0, 187}, {2, 187}, {2, 173}, {0, 19}, {0, 80}, {0, 233},
      // D-pad Left -> frontend/cellphone left / script pad left
      {0, 189}, {2, 189}, {2, 174}, {0, 234},
      // D-pad Right -> frontend/cellphone right / script pad right
      {0, 190}, {2, 190}, {2, 175}, {0, 235},
      // In a vehicle the D-pad cycles the radio (NEXT/PREV/TRACKS/WHEEL) -- the reported leak
      {0, 81}, {0, 82}, {0, 83}, {0, 84}, {0, 85},
      // Cross -> accept / cellphone select / jump. INPUT_ENTER=23 (Triangle = Enter Vehicle) is
      // intentionally NOT suppressed: it is not a menu-navigation button, and the player must be
      // able to board a car with the menu open.
      {0, 201}, {2, 201}, {2, 176}, {0, 22},
      // Circle -> cancel / change view / cover / reload / duck
      {0, 202}, {2, 202}, {2, 177}, {0, 0}, {0, 44}, {0, 45}, {0, 36},
  };
  for (unsigned i = 0; i < sizeof(kSuppress) / sizeof(kSuppress[0]); ++i) {
    invoke_void(g_n.disable_control_action, kSuppress[i].group, kSuppress[i].control, 1);
  }
}

// Per-frame game/script-thread effect tick, registered with the frame hook in
// gtav_features_init. The native latches the LAST impact, so it keeps reading the same
// point after the shot lands -- without the move check we would spawn ~60 overlapping
// explosions/sec at one spot. Only acts when the impact moved (>0.5 m). Entity-creating,
// so this is correct only because the frame hook runs it on the game/script thread.
extern "C" void gtav_features_game_thread_tick(void) {
  // Record that the frame-hook side fired this frame (updates hookSideTicks and runs the
  // native-bridge game-thread canary). Kept at the top so it happens even if a later
  // feature branch returns early.
  gtav_menu_frame_tick();
  // Census which script owns this fire, so the model-streaming jobs can wait for a long-lived
  // owner instead of attributing their request to a transient script that will reclaim it. Runs
  // here because the frame hook calls this tick on EVERY fire, in the same script context and
  // inside the same drain guard as the jobs -- so the handler the census records and the handler
  // a job reads are the same fire's. Compiled to nothing unless the census gate is on.
  model_stream_census_tick();
  // Block GTA's own reaction to the menu-navigation buttons while the panel is open. Runs
  // on the game thread (real script context) so DISABLE_CONTROL_ACTION actually takes.
  if (g_menu_open) suppress_menu_input_controls();
  // Noclip free-move runs here (game/script thread) -- its camera + entity-move natives
  // fault on the scePad worker thread. Independent of the weapon-fx effects below.
  noclip_game_thread_tick();
  // Fly modes: velocity-driven flight of the ped/vehicle. Same game-thread requirement as noclip
  // (velocity/gravity/camera natives fault on the worker). Cheap when both modes are Off.
  fly_game_thread_tick();
  // Interactive object placement: same game-thread requirement as noclip (entity-move natives).
  object_move_game_thread_tick();
  // Gun toolkit (Gravity/Teleport/Kaboom/Freeze/Clone): raycasts the aim + applies the effect on
  // Fire. Same game-thread requirement (shape-test + ADD_EXPLOSION). Cheap when no gun is active.
  gun_toolkit_game_thread_tick();
  free_cam_game_thread_tick();  // free/cinematic camera (scripted cam natives; game-thread only)
  // Wardrobe full-player camera: frame the ped in front while the Wardrobe submenu is open (scripted
  // cam natives; game-thread only). Cheap when not in Wardrobe (one volatile read + early return).
  wardrobe_cam_game_thread_tick();
  // World spectacle: re-assert the sustained camera shake (render-script effect). Cheap when Off.
  spectacle_game_thread_tick();
  // Vehicle autopilot issues/adjusts the player ped's drive task here (game/script thread):
  // the TASK_VEHICLE_* / SET_DRIVE_TASK_* / CLEAR_PED_TASKS natives fault on the worker.
  // Cheap when disengaged (one volatile read + early return).
  autopilot_game_thread_tick();
  // Minigame modes (Riot/Meteor/Storm/...) likewise run their per-frame work here: ped
  // retasks, explosions/fires, and world setters all need valid script context. Cheap when
  // no mode is active (one volatile read + early return).
  minigame_game_thread_tick();
  // Screen effects (night vision / thermal) re-assert here so they survive cutscene/load
  // clears; inert until their toggle is enabled.
  effects_game_thread_tick();
  emote_game_thread_tick();  // emote player: stream dict + TASK_PLAY_ANIM (game-thread only)
  // Held-toggle re-assertions (god mode, vehicle god/health/grip, nitro/brake, auto-heal,
  // wanted lock, movement multipliers, ...). These natives walk LIVE engine entities, so they
  // run here on the game/script thread -- NOT on the scePad worker, where they raced the
  // engine's entity lifecycle and intermittently crashed GTA during state transitions. Cheap
  // when nothing is toggled on (a handful of guards + one GET_FRAME_COUNT).
  reassert_toggles_game_thread();
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  // MP/DLC-vehicle despawn fix: dormant until an explicit vehicle-spawn request
  // arms it. Thereafter this game-thread tick verifies/maintains the RAGE flag. This
  // deliberately performs no script-global access during story/save initialization.
  gtav_script_globals_tick(NULL);
#endif
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_NATIVE_FEATURES
  // Native instructional-button bar: request/build/draw GTA's own instructional_buttons Scaleform
  // for the menu's bottom controls strip. The same thread scripts draw fullscreen scaleforms from;
  // cheap when the menu is closed (one volatile read + early return). Kept here (after the held
  // re-assertions) so it never delays them and the rest of the tick reads top-to-bottom.
  instructional_game_thread_tick();
#endif
  if (!g_explosive_ammo_enabled && !g_fire_ammo_enabled && !g_explosive_melee_enabled) return;
  // An active gun-toolkit gun owns the shot (it also fires on the weapon-impact edge); belt-and-
  // suspenders so a stale ammo flag never stacks an explosion/fire on top of the gun's effect.
  // (g_active_gun == 0 is "no gun"; GTAV_GUN_* is defined in gun_toolkit.inc, included after this.)
  if (g_active_gun != 0) return;
  g_wfx_tick_fired++;  // tick ran with an ammo/melee effect enabled
  if (!g_n.get_ped_last_weapon_impact_coord || !g_n.player_ped_id) return;
  int ped = native_player_ped_id();
  if (!ped) return;
  // Explosive Melee only fires on an UNARMED impact (a punch), so the explosion does not also go off
  // when the player shoots a gun while the toggle is on. GET_CURRENT_PED_WEAPON writes the hash via
  // a Hash* out-param (BOOL return). WEAPON_UNARMED = 0xA2719263.
  int melee_now = 0;
  if (g_explosive_melee_enabled && g_n.get_current_ped_weapon) {
    uint32_t wpn = 0;
    invoke_return<int>(g_n.get_current_ped_weapon, ped, (void*)&wpn, 1);
    melee_now = (wpn == 0xA2719263u);
  }
  Vec3 hit;
  if (!last_weapon_impact(ped, &hit)) return;
  g_wfx_impact_hit++;  // native returned a hit this frame
  g_wfx_last_x = hit.x;  // raw read, pre-dedup -- shown on the Debug page for bisection
  g_wfx_last_y = hit.y;
  g_wfx_last_z = hit.z;
  if (g_have_last_impact) {
    const float dx = hit.x - g_last_impact.x;
    const float dy = hit.y - g_last_impact.y;
    const float dz = hit.z - g_last_impact.z;
    if (dx * dx + dy * dy + dz * dz < 0.25f) return;  // same impact, already handled
  }
  g_last_impact = hit;
  g_have_last_impact = 1;
  if (g_explosive_ammo_enabled && g_n.add_owned_explosion) {
    // ADD_OWNED_EXPLOSION(ped, x, y, z, type, damageScale, isAudible, isInvisible, shake).
    // type 4 = ROCKET; owned by the player ped so it does not raise a wanted level.
    invoke_void(g_n.add_owned_explosion, ped, hit.x, hit.y, hit.z, 4, 1.0f, 1, 0, 0.5f);
    g_wfx_effect_requested++;
  } else if (g_explosive_melee_enabled && melee_now && g_n.add_owned_explosion) {
    // Explosive punch: same owned ROCKET blast, only on an unarmed impact.
    invoke_void(g_n.add_owned_explosion, ped, hit.x, hit.y, hit.z, 4, 1.0f, 1, 0, 0.5f);
    g_wfx_effect_requested++;
  } else if (g_fire_ammo_enabled && g_n.start_script_fire) {
    // START_SCRIPT_FIRE(x, y, z, maxChildren, isGasFire) -> fire handle (discarded).
    (void)invoke_return<int>(g_n.start_script_fire, hit.x, hit.y, hit.z, 5, 0);
    g_wfx_effect_requested++;
  }
}
#endif
