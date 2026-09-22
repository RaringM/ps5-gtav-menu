// Minigames -- fun "modes" (Riot, Meteor Shower, Storm, Inferno, Ragdoll, and the Phase-B
// Blackout / Zombie Outbreak / Flood).
//
// Source fragment of features.cpp, textually included inside its anonymous namespace
// (shares g_n / g_state / the invoke helpers / Vec3 / retask_peds_core). Not a standalone TU.
//
// A minigame is just a per-frame MODE feature, structurally identical to noclip: the toggle
// handler only flips worker-side state (the active id), and a per-frame driver registered
// through gtav_features_game_thread_tick() does the real work in valid script context,
// collapsing the many PLAYER_PED_ID fires per rendered frame via GET_FRAME_COUNT. The
// entity-creating / ped-task / world natives faulted from the scePad worker, so EVERYTHING
// runs game-thread; the toggle refuses until the frame hook is live (like noclip / weapon-fx).
//
// Modes are MUTUALLY EXCLUSIVE: a single active id (g_active_minigame) means enabling one
// turns off any other, so two modes can never fight over the same global (e.g. weather) and
// the toggle mask shows at most one checkmark. The driver detects a change of active id and
// runs the previous mode's cleanup (calm rioting peds, restore weather/lights) on the game
// thread before starting the new one.
//
// Every mode's natives are now pinned in native_addresses_generated.h and live (Riot,
// Meteor, Storm, Inferno, Ragdoll, Blackout, Zombies; Flood is experimental).
// minigame_natives_ready() is a defensive runtime gate: if any native a mode needs fails
// to resolve on a given build its g_n.* field is 0, so the mode reports unavailable and the
// menu row renders locked -- never a crash. See docs/MINIGAMES.md.

// Map a menu action to the internal minigame id (GTAV_MINIGAME_*). Used by the toggle handler.
[[maybe_unused]] static int minigame_id_for_action(uint32_t action) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RIOT:
      return GTAV_MINIGAME_RIOT;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_METEOR:
      return GTAV_MINIGAME_METEOR;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_STORM:
      return GTAV_MINIGAME_STORM;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_INFERNO:
      return GTAV_MINIGAME_INFERNO;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RAGDOLL:
      return GTAV_MINIGAME_RAGDOLL;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_BLACKOUT:
      return GTAV_MINIGAME_BLACKOUT;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_ZOMBIES:
      return GTAV_MINIGAME_ZOMBIES;
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_FLOOD:
      return GTAV_MINIGAME_FLOOD;
    default:
      return GTAV_MINIGAME_NONE;
  }
}

// Make `id` the one active mode (or NONE). Mutually exclusive: every other flag is cleared.
// Flag-only (no native call), so it is safe on the worker thread and at init.
static void minigame_set_active(int id) {
  for (int i = 0; i < GTAV_MINIGAME_COUNT; ++i) g_minigame_on[i] = 0;
  if (id > GTAV_MINIGAME_NONE && id < GTAV_MINIGAME_COUNT) g_minigame_on[id] = 1;
  g_active_minigame = id;
}

// Turn off every minigame. Called from disable_all_features() (which also runs at init via
// import_profile), so this MUST stay flag-only -- the driver does any native restore on the
// game thread when it next sees the active id change.
static void minigame_stop_all(void) { minigame_set_active(GTAV_MINIGAME_NONE); }

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Are all natives a mode needs resolved? Defensive gate: if a native fails to resolve on a
// build its g_n.* field is 0, so the mode reports unavailable (the row locks) instead of crashing.
static int minigame_natives_ready(int id) {
  switch (id) {
    case GTAV_MINIGAME_RIOT:
      return g_n.get_ped_nearby_peds && g_n.task_combat_ped && g_n.player_ped_id;
    case GTAV_MINIGAME_METEOR:
      return g_n.add_owned_explosion && g_n.get_entity_coords && g_n.player_ped_id;
    case GTAV_MINIGAME_STORM:
      return g_n.set_weather_type_now_persist != 0;
    case GTAV_MINIGAME_INFERNO:
      return g_n.start_script_fire && g_n.get_entity_coords && g_n.player_ped_id;
    case GTAV_MINIGAME_RAGDOLL:
      return g_n.get_ped_nearby_peds && g_n.set_ped_to_ragdoll && g_n.player_ped_id;
    case GTAV_MINIGAME_BLACKOUT:
      return g_n.set_artificial_lights_state != 0;  // Phase B
    case GTAV_MINIGAME_ZOMBIES:  // Phase B
      return g_n.create_ped && g_n.request_model && g_n.has_model_loaded && g_n.get_entity_coords &&
             g_n.player_ped_id && g_n.task_combat_ped && g_n.set_ped_relationship_group_hash &&
             g_n.set_ped_combat_attributes && g_n.add_relationship_group &&
             g_n.set_relationship_between_groups && g_n.get_hash_key;
    case GTAV_MINIGAME_FLOOD:  // Phase B (experimental)
      return g_n.modify_water && g_n.get_entity_coords && g_n.player_ped_id;
    default:
      return 0;
  }
}

static const char* minigame_on_label(int id) {
  switch (id) {
    case GTAV_MINIGAME_RIOT:
      return "riot mode on";
    case GTAV_MINIGAME_METEOR:
      return "meteor shower on";
    case GTAV_MINIGAME_STORM:
      return "storm on";
    case GTAV_MINIGAME_INFERNO:
      return "inferno on";
    case GTAV_MINIGAME_RAGDOLL:
      return "ragdoll mode on";
    case GTAV_MINIGAME_BLACKOUT:
      return "blackout on";
    case GTAV_MINIGAME_ZOMBIES:
      return "zombie outbreak on";
    case GTAV_MINIGAME_FLOOD:
      return "flood on";
    default:
      return "minigame on";
  }
}

// How often (ms) the mode's periodic effect fires. Continuous re-tasks join newly-streamed
// peds; explosion/fire cadence is the documented weapon-fx throttle (without it the effect
// would fire ~60x/sec). The two world setters (Storm/Blackout) re-assert occasionally to
// survive a cutscene/script reset.
static uint32_t minigame_interval_ms(int id) {
  switch (id) {
    case GTAV_MINIGAME_METEOR:
      return 700u;
    case GTAV_MINIGAME_RAGDOLL:
      return 1200u;
    case GTAV_MINIGAME_RIOT:
    case GTAV_MINIGAME_INFERNO:
      return 1500u;
    case GTAV_MINIGAME_BLACKOUT:
      return 2000u;
    case GTAV_MINIGAME_ZOMBIES:
      return 1000u;  // survival cull/spawn cadence (snappier than a pure wave timer)
    case GTAV_MINIGAME_STORM:
      return 3000u;
    case GTAV_MINIGAME_FLOOD:
      return 1000u;
    default:
      return 1000u;
  }
}

// Game-thread-owned driver state.
static uint32_t g_minigame_last_frame = 0;          // collapse the many hook fires per frame
static uint32_t g_minigame_last_action_ms = 0;      // wall-clock gate for the periodic effect
static int g_minigame_driven = GTAV_MINIGAME_NONE;  // mode the driver last serviced
static uint32_t g_minigame_rng = 0;                 // xorshift state (seeded from GET_GAME_TIMER)

// Zombie Outbreak survival game (game-thread-owned). The hostile relationship group is
// registered once per worker lifetime and reused. Spawned zombies are tracked in a fixed
// roster so the driver can score kills, advance the wave once the horde is cleared, and
// despawn the horde on stop. Kill/despawn detection and cleanup are each independently gated:
// if IS_ENTITY_DEAD / DOES_ENTITY_EXIST / DELETE_PED are unresolved the mode degrades to the
// legacy endless spawner instead of locking (the minigame_natives_ready contract). The
// survival phase is published to the HUD as GTAV_ZOMBIE_STATE_* (features.h).
static const uint32_t GTAV_ZOMBIE_MODEL = 0xAC4B4506u;  // joaat("u_m_y_zombie_01")
enum { GTAV_ZOMBIE_ROSTER_MAX = 48 };  // fixed roster; CREATE_PED==0 (pool full) also caps

// Zombie archetypes, picked per spawn from a small integer weight table that escalates with the
// wave (no libm -- selection is minigame_rand() % table_len, all integer arithmetic).
enum {
  ZOMBIE_SHAMBLER = 0,  // slow + tanky: the baseline horror
  ZOMBIE_RUNNER,        // fast + fragile: the panic-inducer
  ZOMBIE_BRUTE,         // very tanky + explosion-proof: forces gunplay
  ZOMBIE_EXPLODER,      // pops easily, then detonates where it falls
};

enum {
  GTAV_ZOMBIE_SPAWN_BATCH = 4,          // peds emitted per spawn tick
  GTAV_ZOMBIE_INTERMISSION_MS = 4000u,  // breather between waves
  GTAV_ZOMBIE_GAMEOVER_MS = 6000u,      // hold GAME OVER before auto-stop
};

static uint32_t g_zombie_group = 0;                    // relationship-group hash (0 = unset)
static int g_zombie_roster[GTAV_ZOMBIE_ROSTER_MAX];    // live ped handles (compacted)
static uint8_t g_zombie_kind[GTAV_ZOMBIE_ROSTER_MAX];  // archetype per live handle
static int g_zombie_roster_n = 0;                      // live count
static int g_zombie_wave = 0;                          // 0 = not started; 1..N
static int g_zombie_to_spawn = 0;                      // peds still to emit this wave
static uint32_t g_zombie_kills = 0;                    // confirmed kills this session
static int g_zombie_state = GTAV_ZOMBIE_STATE_IDLE;
static uint32_t g_zombie_phase_ms = 0;                 // wall-clock marker (intermission/gameover)

// xorshift32: cheap deterministic scatter for explosion/fire offsets (no libm -- the payload
// does not link -lm, so sinf/cosf/rand are out).
static uint32_t minigame_rand(void) {
  uint32_t x = g_minigame_rng ? g_minigame_rng : 0x9e3779b9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_minigame_rng = x;
  return x;
}

// A signed offset in roughly [-range, range] metres.
static float minigame_offset(float range) {
  const float t = (float)(minigame_rand() & 0xffffu) / 32768.0f - 1.0f;  // ~ -1..1
  return t * range;
}

// One ROCKET explosion at a random offset around the player, owned by the player ped so it
// raises no wanted level (the exact weapon_fx.inc:257 call). Explosions are not models, so no
// streaming is needed.
static void minigame_spawn_explosion(void) {
  if (!g_n.add_owned_explosion || !g_n.get_entity_coords || !g_n.player_ped_id) return;
  int ped = native_player_ped_id();
  if (!ped) return;
  Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.add_owned_explosion, ped, c.x + minigame_offset(18.0f),
              c.y + minigame_offset(18.0f), c.z + 2.0f, 4, 1.0f, 1, 0, 0.5f);
}

// One script fire at a random offset around the player.
static void minigame_spawn_fire(void) {
  if (!g_n.start_script_fire || !g_n.get_entity_coords || !g_n.player_ped_id) return;
  int ped = native_player_ped_id();
  if (!ped) return;
  Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  // START_SCRIPT_FIRE(x, y, z, maxChildren, isGasFire) -> fire handle (discarded).
  (void)invoke_return<int>(g_n.start_script_fire, c.x + minigame_offset(10.0f),
                           c.y + minigame_offset(10.0f), c.z, 5, 0);
}

// One-time setup when Zombie Outbreak activates: register a relationship group that hates the
// PLAYER group (so zombies are autonomously hostile), then kick off model streaming. The group
// is created once per worker lifetime and reused across activations.
static void minigame_zombie_setup(void) {
  if (!g_zombie_group && g_n.add_relationship_group) {
    // ADD_RELATIONSHIP_GROUP(name, Hash* out): the 4-byte group hash is written into the slot.
    uint64_t slot = 0;
    (void)invoke_return<int>(g_n.add_relationship_group, "GTAVMENU_ZOMBIE", (void*)&slot);
    g_zombie_group = (uint32_t)slot;
    if (g_zombie_group && g_n.set_relationship_between_groups && g_n.get_hash_key) {
      const uint32_t player_group = (uint32_t)invoke_return<int>(g_n.get_hash_key, "PLAYER");
      // 5 = Hate. Make the two groups hate each other so the zombies engage and stay hostile.
      invoke_void(g_n.set_relationship_between_groups, 5, g_zombie_group, player_group);
      invoke_void(g_n.set_relationship_between_groups, 5, player_group, g_zombie_group);
    }
  }
  if (g_n.request_model) invoke_void(g_n.request_model, (int)GTAV_ZOMBIE_MODEL);
}

// Pick an archetype for `wave` from a stack-built integer weight table (no libm: a uniform index
// via minigame_rand() % n). Early waves are pure shamblers; runners join from wave 3, brutes from
// wave 5, exploders from wave 7 -- so the horde steadily gets nastier.
static int zombie_pick_kind(int wave) {
  uint8_t table[10];
  int n = 0;
  table[n++] = ZOMBIE_SHAMBLER;
  table[n++] = ZOMBIE_SHAMBLER;
  if (wave >= 3) table[n++] = ZOMBIE_RUNNER;
  if (wave >= 4) table[n++] = ZOMBIE_RUNNER;
  if (wave >= 5) table[n++] = ZOMBIE_BRUTE;
  if (wave >= 7) table[n++] = ZOMBIE_EXPLODER;
  if (wave >= 9) table[n++] = ZOMBIE_RUNNER;
  return (int)table[minigame_rand() % (uint32_t)n];
}

// Apply per-archetype health + move-rate (and brute explosion-proofing) to a fresh zombie.
// Health scales with the wave so later hordes soak more damage. All pinned natives, each guarded
// so a missing one is simply skipped.
static void zombie_apply_archetype(int zped, int kind, int wave) {
  const int base = 100 + 20 * (wave - 1);  // wave 1 = 100 HP, +20 per wave
  int health = base;
  float move_rate = 1.0f;
  switch (kind) {
    case ZOMBIE_RUNNER:
      health = base * 3 / 4;  // fragile
      move_rate = 1.35f;      // sprint
      break;
    case ZOMBIE_BRUTE:
      health = base * 4;  // very tanky
      move_rate = 0.85f;
      if (g_n.set_entity_proofs)
        // SET_ENTITY_PROOFS(entity, bullet, fire, explosion, collision, melee, steam, p7, drown).
        invoke_void(g_n.set_entity_proofs, zped, 0, 0, 1, 0, 0, 0, 0, 0);  // explosion-proof
      break;
    case ZOMBIE_EXPLODER:
      health = base / 2;  // pops easily
      move_rate = 1.1f;
      break;
    case ZOMBIE_SHAMBLER:
    default:
      health = base * 2;  // tanky baseline
      move_rate = 0.75f;  // slow trudge
      break;
  }
  if (g_n.set_ped_max_health) invoke_void(g_n.set_ped_max_health, zped, health);
  if (g_n.set_entity_health) invoke_void(g_n.set_entity_health, zped, health);
  if (g_n.set_ped_move_rate_override) invoke_void(g_n.set_ped_move_rate_override, zped, move_rate);
}

// Track a freshly-spawned zombie so we can reap it later. Silently drops if the roster is full
// (a second cap alongside CREATE_PED's pool-full backpressure).
static void zombie_roster_add(int ped, int kind) {
  if (g_zombie_roster_n >= GTAV_ZOMBIE_ROSTER_MAX) return;
  g_zombie_roster[g_zombie_roster_n] = ped;
  g_zombie_kind[g_zombie_roster_n] = (uint8_t)kind;
  ++g_zombie_roster_n;
}

// One player-owned ROCKET blast where an exploder fell -- owned by the player ped so it raises no
// wanted level (the exact Meteor-Shower call shape). Coords are still valid on a just-dead ped
// (it is reaped right after).
static void zombie_exploder_blast(int ped) {
  if (!g_n.add_owned_explosion || !g_n.get_entity_coords) return;
  int player = native_player_ped_id();
  if (player <= 0) return;
  Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.add_owned_explosion, player, c.x, c.y, c.z, 4, 1.0f, 1, 0, 0.5f);
}

// Reap the roster once per tick: a dead-but-existing handle is a player kill (score + exploder
// payoff, then DELETE_PED to reclaim the pool slot); a no-longer-existing handle was despawned by
// the engine (freed, no score). Survivors compact to the front. Without the detection natives
// this is a no-op -- the roster only grows to the cap (legacy endless-spawner behavior).
static void zombie_roster_cull(void) {
  if (!g_n.is_entity_dead && !g_n.does_entity_exist) return;
  int w = 0;
  for (int i = 0; i < g_zombie_roster_n; ++i) {
    const int h = g_zombie_roster[i];
    const int kind = (int)g_zombie_kind[i];
    if (g_n.does_entity_exist && !invoke_return<int>(g_n.does_entity_exist, h)) {
      continue;  // engine despawn -- drop without scoring
    }
    if (g_n.is_entity_dead && invoke_return<int>(g_n.is_entity_dead, h)) {
      ++g_zombie_kills;
      if (kind == ZOMBIE_EXPLODER) zombie_exploder_blast(h);
      if (g_n.delete_ped) {
        int handle = h;
        invoke_void(g_n.delete_ped, (void*)&handle);  // reclaim the ped-pool slot
      }
      continue;  // killed -- drop from roster
    }
    g_zombie_roster[w] = h;  // still alive -- keep
    g_zombie_kind[w] = (uint8_t)kind;
    ++w;
  }
  g_zombie_roster_n = w;
}

// Start wave `w`: set its size (ramping but capped well under the roster max so high waves do not
// starve the rest of the game's ped pool) and queue the spawns.
static void zombie_begin_wave(int w) {
  g_zombie_wave = w;
  int count = 4 + 2 * w;       // wave 1 = 6, +2 per wave
  if (count > 24) count = 24;  // headroom under GTAV_ZOMBIE_ROSTER_MAX
  g_zombie_to_spawn = count;
  g_zombie_state = GTAV_ZOMBIE_STATE_SPAWNING;
}

// Emit up to GTAV_ZOMBIE_SPAWN_BATCH zombies of the current wave around the player, tracking each
// in the roster. Runs on the game thread (driver tick) so the fresh handle is tasked immediately.
// Streams the model non-blocking and only spawns once it is resident; CREATE_PED returning 0
// (pool full) or a full roster ends the batch. When the wave is fully emitted -> FIGHTING.
static void minigame_zombie_spawn(void) {
  if (!g_n.create_ped || !g_n.has_model_loaded || !g_n.get_entity_coords || !g_n.player_ped_id) {
    return;
  }
  if (g_n.request_model) invoke_void(g_n.request_model, (int)GTAV_ZOMBIE_MODEL);
  if (!invoke_return<int>(g_n.has_model_loaded, (int)GTAV_ZOMBIE_MODEL)) return;  // still streaming
  int player = native_player_ped_id();
  if (player <= 0) return;
  Vec3 c = invoke_vector(g_n.get_entity_coords, player, 1);
  int batch = GTAV_ZOMBIE_SPAWN_BATCH;
  while (g_zombie_to_spawn > 0 && batch > 0 && g_zombie_roster_n < GTAV_ZOMBIE_ROSTER_MAX) {
    --batch;
    const int kind = zombie_pick_kind(g_zombie_wave);
    // CREATE_PED(pedType 4 = CIVMALE, model, x, y, z, heading, isNetwork=0, scriptHostPed=0).
    int zped = invoke_return<int>(g_n.create_ped, 4, (int)GTAV_ZOMBIE_MODEL,
                                  c.x + minigame_offset(14.0f), c.y + minigame_offset(14.0f), c.z,
                                  0.0f, 0, 0);
    if (!zped) break;  // ped pool full -- stop this batch
    if (g_zombie_group && g_n.set_ped_relationship_group_hash)
      invoke_void(g_n.set_ped_relationship_group_hash, zped, g_zombie_group);
    if (g_n.set_ped_combat_attributes) {
      invoke_void(g_n.set_ped_combat_attributes, zped, 46, 1);  // BF_AlwaysFight
      invoke_void(g_n.set_ped_combat_attributes, zped, 5, 1);   // BF_CanFightArmedPedsWhenNotArmed
    }
    if (g_n.set_blocking_of_non_temporary_events)
      invoke_void(g_n.set_blocking_of_non_temporary_events, zped, 1);
    if (g_n.set_ped_keep_task) invoke_void(g_n.set_ped_keep_task, zped, 1);
    zombie_apply_archetype(zped, kind, g_zombie_wave);
    if (g_n.task_combat_ped) invoke_void(g_n.task_combat_ped, zped, player, 0, 16);
    zombie_roster_add(zped, kind);
    --g_zombie_to_spawn;
  }
  if (g_zombie_to_spawn <= 0) g_zombie_state = GTAV_ZOMBIE_STATE_FIGHTING;
}

// Survival driver, run once per throttle interval from minigame_periodic(). Reap the horde, watch
// for player death, then advance the wave machine. Degrades gracefully: without the detection
// natives there is no reaping/clear/game-over, so the FIGHTING arm re-arms the wave and the mode
// behaves like the old endless spawner.
static void minigame_zombie_loop(void) {
  zombie_roster_cull();
  const uint32_t now = g_n.get_game_timer ? invoke_return<uint32_t>(g_n.get_game_timer) : 0u;

  // Player death -> game over (only meaningful with kill detection; else the mode is endless).
  if (g_zombie_state != GTAV_ZOMBIE_STATE_GAMEOVER && g_n.is_entity_dead && g_n.player_ped_id) {
    const int player = native_player_ped_id();
    if (player > 0 && invoke_return<int>(g_n.is_entity_dead, player)) {
      g_zombie_state = GTAV_ZOMBIE_STATE_GAMEOVER;
      g_zombie_phase_ms = now;
    }
  }

  switch (g_zombie_state) {
    case GTAV_ZOMBIE_STATE_SPAWNING:
      minigame_zombie_spawn();
      break;
    case GTAV_ZOMBIE_STATE_FIGHTING:
      if (g_n.is_entity_dead || g_n.does_entity_exist) {
        if (g_zombie_roster_n <= 0) {  // horde cleared
          g_zombie_state = GTAV_ZOMBIE_STATE_INTERMISSION;
          g_zombie_phase_ms = now;
        }
      } else {
        g_zombie_to_spawn = 3;  // no detection -> keep the horde topped up (legacy behavior)
        g_zombie_state = GTAV_ZOMBIE_STATE_SPAWNING;
      }
      break;
    case GTAV_ZOMBIE_STATE_INTERMISSION:
      if (now - g_zombie_phase_ms >= GTAV_ZOMBIE_INTERMISSION_MS)
        zombie_begin_wave(g_zombie_wave + 1);
      break;
    case GTAV_ZOMBIE_STATE_GAMEOVER:
      if (now - g_zombie_phase_ms >= GTAV_ZOMBIE_GAMEOVER_MS)
        minigame_set_active(GTAV_MINIGAME_NONE);  // driver runs cleanup on the next tick
      break;
    default:
      break;
  }
}

// Phase-B Flood (experimental). There is no per-frame water-LEVEL setter native, only
// MODIFY_WATER (a localized patch with no clean inverse), so this is a deliberately gated
// experiment: raise a water patch around the player. Restore is best-effort (see cleanup).
static void minigame_flood_step(void) {
  if (!g_n.modify_water || !g_n.get_entity_coords || !g_n.player_ped_id) return;
  int ped = native_player_ped_id();
  if (!ped) return;
  Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  // MODIFY_WATER(x, y, radius, height): nudge the local water height up around the player.
  invoke_void(g_n.modify_water, c.x, c.y, 80.0f, c.z + 3.0f);
}

// Immediate setup when a mode becomes active (runs once, on the transition).
static void minigame_start(int id) {
  switch (id) {
    case GTAV_MINIGAME_STORM:
      if (g_n.set_weather_type_now_persist)
        invoke_void(g_n.set_weather_type_now_persist, "THUNDER");
      if (g_n.network_override_clock_time)
        invoke_void(g_n.network_override_clock_time, 0, 0, 0);  // midnight
      break;
    case GTAV_MINIGAME_BLACKOUT:
      if (g_n.set_artificial_lights_state) invoke_void(g_n.set_artificial_lights_state, 1);
      break;
    case GTAV_MINIGAME_ZOMBIES:
      minigame_zombie_setup();  // register the hostile group + start streaming the model
      g_zombie_roster_n = 0;    // fresh survival session
      g_zombie_kills = 0;
      g_zombie_wave = 0;
      g_zombie_state = GTAV_ZOMBIE_STATE_IDLE;
      zombie_begin_wave(1);  // queue the first wave
      break;
    default:
      break;  // periodic modes do their first action via the throttle gate (last_ms reset)
  }
}

// Restore world state a mode changed (runs once, on the transition AWAY from `id`).
static void minigame_cleanup(int id) {
  switch (id) {
    case GTAV_MINIGAME_RIOT:
      (void)retask_peds_core(GTAV_PED_CONTROL_STOP);  // calm the peds we made hostile
      break;
    case GTAV_MINIGAME_STORM:
      if (g_n.set_weather_type_now_persist)
        invoke_void(g_n.set_weather_type_now_persist, "EXTRASUNNY");
      break;
    case GTAV_MINIGAME_BLACKOUT:
      if (g_n.set_artificial_lights_state) invoke_void(g_n.set_artificial_lights_state, 0);
      break;
    case GTAV_MINIGAME_ZOMBIES:
      // Despawn the tracked horde so disabling actually ends the outbreak. DELETE_PED reclaims
      // each ped-pool slot; if it is unresolved the spawns persist until the game despawns them
      // (the legacy contract) and we only clear our bookkeeping. Then stop pinning the model so
      // the streamer can reclaim it (the relationship group stays registered, cheap, reused).
      for (int i = 0; i < g_zombie_roster_n; ++i) {
        if (g_n.delete_ped) {
          int handle = g_zombie_roster[i];
          invoke_void(g_n.delete_ped, (void*)&handle);
        }
      }
      g_zombie_roster_n = 0;
      g_zombie_kills = 0;
      g_zombie_wave = 0;
      g_zombie_to_spawn = 0;
      g_zombie_state = GTAV_ZOMBIE_STATE_IDLE;
      if (g_n.set_model_as_no_longer_needed)
        invoke_void(g_n.set_model_as_no_longer_needed, (int)GTAV_ZOMBIE_MODEL);
      break;
    // Meteor / Inferno / Ragdoll: fire-and-forget; effects fade on their own.
    // Flood: MODIFY_WATER has no documented inverse -- experimental, no clean restore.
    default:
      break;
  }
}

// Run the mode's periodic effect (called once per throttle interval, on the game thread).
static void minigame_periodic(int id) {
  switch (id) {
    case GTAV_MINIGAME_RIOT:
      (void)retask_peds_core(GTAV_PED_CONTROL_ATTACK);
      break;
    case GTAV_MINIGAME_RAGDOLL:
      (void)retask_peds_core(GTAV_PED_CONTROL_RAGDOLL);
      break;
    case GTAV_MINIGAME_METEOR:
      minigame_spawn_explosion();
      break;
    case GTAV_MINIGAME_INFERNO:
      minigame_spawn_fire();
      break;
    case GTAV_MINIGAME_STORM:
      if (g_n.set_weather_type_now_persist)
        invoke_void(g_n.set_weather_type_now_persist, "THUNDER");  // re-assert persistence
      break;
    case GTAV_MINIGAME_BLACKOUT:
      if (g_n.set_artificial_lights_state) invoke_void(g_n.set_artificial_lights_state, 1);
      break;
    case GTAV_MINIGAME_ZOMBIES:
      minigame_zombie_loop();
      break;
    case GTAV_MINIGAME_FLOOD:
      minigame_flood_step();
      break;
    default:
      break;
  }
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick(). The
// transition (active != driven) runs cleanup-then-start once; the periodic effect is gated by
// GET_FRAME_COUNT (one logical tick per rendered frame) and a wall-clock interval.
static void minigame_game_thread_tick(void) {
  const int active = g_active_minigame;

  // Mode change: clean up the previous mode and start the new one. Runs once per change
  // (subsequent fires see active == driven). Done here, on the game thread, so the restore
  // lands in valid script context just like the per-frame effect.
  if (active != g_minigame_driven) {
    minigame_cleanup(g_minigame_driven);
    g_minigame_driven = active;
    g_minigame_last_action_ms = 0;  // fire the new mode's first periodic effect immediately
    if (active != GTAV_MINIGAME_NONE) minigame_start(active);
  }
  if (active == GTAV_MINIGAME_NONE) return;

  // Collapse the many PLAYER_PED_ID fires within one rendered frame to a single logical tick.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_minigame_last_frame) return;  // already ticked this frame
    g_minigame_last_frame = frame;
  }

  // Wall-clock throttle so the periodic effect cadence is frame-rate independent.
  if (!g_n.get_game_timer) return;
  const uint32_t now = invoke_return<uint32_t>(g_n.get_game_timer);
  if (g_minigame_rng == 0u) g_minigame_rng = now ? now : 1u;  // seed the PRNG once
  if (g_minigame_last_action_ms != 0u &&
      (now - g_minigame_last_action_ms) < minigame_interval_ms(active)) {
    return;
  }
  g_minigame_last_action_ms = now;
  minigame_periodic(active);
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Toggle a minigame mode on/off. Like noclip, the per-frame work runs on the game thread, so
// this refuses until the frame hook is live (and is unavailable in a build without it). A
// Phase-B mode whose native is unresolved reports unavailable rather than activating.
static uint32_t toggle_minigame(uint32_t action) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  const int id = minigame_id_for_action(action);
  if (id == GTAV_MINIGAME_NONE) return unavailable(action, "unknown minigame");
  if (!minigame_natives_ready(id)) return unavailable(action, "minigame natives unavailable");
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  if (g_active_minigame == id) {
    minigame_set_active(GTAV_MINIGAME_NONE);  // turn this mode off
    return ok(action, "minigame off");
  }
  minigame_set_active(id);  // mutually exclusive: replaces any other active mode
  return ok(action, minigame_on_label(id));
#else
  (void)action;
  return unavailable(action, "needs main-thread hook");
#endif
}
