#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_NATIVE_BRIDGE_ABI_VERSION 1u
#define GTAV_NATIVE_SHELL_LABEL_LEN 32u
#define GTAV_NATIVE_SHELL_MODE_LEN 32u
#define GTAV_NATIVE_SHELL_STATE_MAGIC 0x4C45485356415447ull
#define GTAV_NATIVE_SHELL_STATE_SIZE 128u

enum {
  GTAV_NATIVE_BRIDGE_FLAG_NO_NATIVE_TABLE = 1u << 0,
  GTAV_NATIVE_BRIDGE_FLAG_FRAME_READY = 1u << 1,
  GTAV_NATIVE_BRIDGE_FLAG_INPUT_READY = 1u << 2,
  GTAV_NATIVE_BRIDGE_FLAG_DRAW_READY = 1u << 3,
  GTAV_NATIVE_BRIDGE_FLAG_DRAW_CANARY_READY = 1u << 4,
  GTAV_NATIVE_BRIDGE_FLAG_TIMER_READY = 1u << 5,
  GTAV_NATIVE_BRIDGE_FLAG_TEXT_READY = 1u << 6,
  GTAV_NATIVE_BRIDGE_FLAG_TEXT_OVERLAY_READY = 1u << 7,
};

// Shell action ids: a STABLE wire + save ABI. These integer values are persisted (saved
// profiles, mailbox / ps5debug commands) and crossed over the C boundary, so they are
// fixed and never renumbered; retired ids (18, 19) stay reserved and are never reused.
enum {
  GTAV_NATIVE_SHELL_ACTION_NONE = 0,
  GTAV_NATIVE_SHELL_ACTION_HIDE = 1,
  GTAV_NATIVE_SHELL_ACTION_STOP = 2,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY = 3,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE = 4,
  GTAV_NATIVE_SHELL_ACTION_HEAL_ARMOR = 5,
  GTAV_NATIVE_SHELL_ACTION_CLEAR_WANTED = 6,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_RUN = 7,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_SWIM = 8,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_AMMO = 9,
  GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE = 10,
  GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE = 11,
  GTAV_NATIVE_SHELL_ACTION_CLEAN_VEHICLE = 12,
  GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS = 13,
  GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS = 14,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_JUMP = 15,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_INVISIBLE = 16,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WEATHER = 17,
  // 18, 19 were SET_TIME_NOON / SET_TIME_NIGHT, replaced by CYCLE_TIME (21). The values
  // stay retired (never reused) so any old saved state referring to them is inert.
  GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT = 20,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME = 21,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NEVER_WANTED = 22,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_VEHICLE_GOD = 23,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NITRO = 24,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_ENGINE_ON = 25,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_MOVE = 26,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RAGDOLL = 27,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEATBELT = 28,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_CLEAN = 29,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_PROOFS = 30,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_CRIT = 31,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_STAMINA = 32,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_SPECIAL = 33,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_LOW_GRAVITY = 34,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLOW_MOTION = 35,
  GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL = 36,
  GTAV_NATIVE_SHELL_ACTION_PLACE_ON_WHEELS = 37,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WANTED = 38,
  GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET = 39,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_CLASS = 40,
  GTAV_NATIVE_SHELL_ACTION_SAVE_LOCATION = 41,
  GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION = 42,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_FREEZE_TIME = 43,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_NITRO_POWER = 44,
  GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE = 45,
  GTAV_NATIVE_SHELL_ACTION_LAUNCH_BOOST = 46,
  GTAV_NATIVE_SHELL_ACTION_PRESET_STEALTH = 47,
  GTAV_NATIVE_SHELL_ACTION_PRESET_STUNT = 48,
  GTAV_NATIVE_SHELL_ACTION_PRESET_SURVIVAL = 49,
  GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER = 50,
  // Tier A additions (reuse already-proven, worker-safe natives).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_HEAL = 51,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_WANTED_LOCK = 52,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_REPAIRED = 53,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CLOCK_HOUR = 54,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME_SCALE = 55,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_GRAVITY = 56,
  // Heads-up display overlay (rendered via the proven text lane, getter-driven).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD = 57,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_SPEEDO = 58,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_COORDS = 59,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_FPS = 60,
  // Settings persistence (profile.cfg on /data).
  GTAV_NATIVE_SHELL_ACTION_SAVE_PROFILE = 61,
  GTAV_NATIVE_SHELL_ACTION_LOAD_PROFILE = 62,
  // Tier A additions (worker-safe, reuse already-proven natives).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_DISTANCE = 63,     // HUD: distance to waypoint blip
  GTAV_NATIVE_SHELL_ACTION_CYCLE_MOVE_RATE = 64,         // tune Fast Movement strength
  GTAV_NATIVE_SHELL_ACTION_PRESET_PARKOUR = 65,          // super jump + no-ragdoll + fast move
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_STICK_TO_GROUND = 66,  // anti-flip: keep vehicle grounded
  // Weapon loadout picker: give + equip ONE weapon (param = joaat hash). Game-thread
  // gated like GIVE_WEAPONS; rides the command lane carrying the hash in param.
  GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON = 67,
  // Self cosmetic: clear blood + wetness on the player ped (worker-safe).
  GTAV_NATIVE_SHELL_ACTION_CLEAN_SELF = 68,
  // Drift mode: reduce the current vehicle's grip while driving (worker-safe per-tick).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_DRIFT_MODE = 69,
  // Los Santos Customs (worker-safe vehicle mod/respray on the current vehicle).
  GTAV_NATIVE_SHELL_ACTION_LS_MAX_PERFORMANCE = 70,     // install top engine/brakes/etc + turbo
  GTAV_NATIVE_SHELL_ACTION_LS_STOCK_VEHICLE = 71,       // strip all mods + default paint
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_PRIMARY = 72,    // respray primary colour (list cycler)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_SECONDARY = 73,  // respray secondary colour (list cycler)
  // Skin changer: rebuild the player ped to a model (param = model joaat hash).
  // Game-thread + model-streaming, routed exactly like SPAWN_VEHICLE.
  GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL = 74,
  // Los Santos Customs expansion (worker-safe; same class as the mod/respray rows).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WINDOW_TINT = 75,     // window tint (list cycler)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_NEON = 76,            // neon underglow colour (list cycler)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TYRE_SMOKE = 77,      // coloured tyre smoke (list cycler)
  GTAV_NATIVE_SHELL_ACTION_LS_INSTALL_COSMETICS = 78,  // install top visual mods + wheels
  GTAV_NATIVE_SHELL_ACTION_LS_BULLETPROOF_TYRES = 79,  // make tyres non-burstable
  // Per-slot Los Santos Customs mod pickers (worker-safe list cyclers): each row picks a
  // tier for one mod category, bounded live by GET_NUM_VEHICLE_MODS. MUST stay contiguous
  // and in the SAME order as kVehicleModSlots[] in features.cpp -- range dispatch keys off
  // (action - GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST).
  GTAV_NATIVE_SHELL_ACTION_MOD_SPOILER = 80,
  GTAV_NATIVE_SHELL_ACTION_MOD_FRONT_BUMPER = 81,
  GTAV_NATIVE_SHELL_ACTION_MOD_REAR_BUMPER = 82,
  GTAV_NATIVE_SHELL_ACTION_MOD_SIDE_SKIRT = 83,
  GTAV_NATIVE_SHELL_ACTION_MOD_EXHAUST = 84,
  GTAV_NATIVE_SHELL_ACTION_MOD_FRAME = 85,
  GTAV_NATIVE_SHELL_ACTION_MOD_GRILLE = 86,
  GTAV_NATIVE_SHELL_ACTION_MOD_HOOD = 87,
  GTAV_NATIVE_SHELL_ACTION_MOD_FENDERS = 88,
  GTAV_NATIVE_SHELL_ACTION_MOD_ROOF = 89,
  GTAV_NATIVE_SHELL_ACTION_MOD_ENGINE = 90,
  GTAV_NATIVE_SHELL_ACTION_MOD_BRAKES = 91,
  GTAV_NATIVE_SHELL_ACTION_MOD_TRANSMISSION = 92,
  GTAV_NATIVE_SHELL_ACTION_MOD_SUSPENSION = 93,
  GTAV_NATIVE_SHELL_ACTION_MOD_ARMOR = 94,
  GTAV_NATIVE_SHELL_ACTION_MOD_WHEELS = 95,
  GTAV_NATIVE_SHELL_ACTION_MOD_HORN = 96,
  GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST = GTAV_NATIVE_SHELL_ACTION_MOD_SPOILER,
  GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_LAST = GTAV_NATIVE_SHELL_ACTION_MOD_HORN,
  // Xenon headlights toggle (TOGGLE_VEHICLE_MOD slot 22; reuses already-bound natives).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_XENON = 97,
  // LS Customs expansion 2 (worker-safe list cyclers + one toggle): wheel category,
  // liveries, number-plate style/text, extra (pearlescent + wheel) and custom-RGB
  // colours, plus a per-tick rainbow-neon toggle. All apply on the worker thread like
  // the paint/tint cyclers above.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_TYPE = 98,         // wheel category (Sport/Muscle/...)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_LIVERY = 99,             // livery index, bounded live by count
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_STYLE = 100,       // number-plate style id (0..5)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_TEXT = 101,        // curated plate-text presets
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PEARLESCENT = 102,       // extra-colour pearlescent highlight
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_COLOR = 103,       // extra-colour wheel tint
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_PRIMARY = 104,    // true-RGB primary colour
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_SECONDARY = 105,  // true-RGB secondary colour
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_NEON = 106,     // per-tick neon hue cycle
  // Weapon effects: continuous toggles whose entity-creating natives run only from the
  // per-frame game-thread tick (frame hook). Toggling refuses until the hook is live.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_AMMO = 107,  // explode at each weapon impact
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_FIRE_AMMO = 108,       // start a fire at each weapon impact
  // Combat extras. The two explosion actions CREATE entities, so (like SPAWN_VEHICLE)
  // they are marshalled onto the game thread via the frame-hook job queue and stay
  // locked until the hook is live. Weapon-damage is a worker-safe player-state cycler.
  GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER = 109,  // explosion at the player (game-thread)
  GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT =
      110,                                             // explosion at the waypoint (game-thread)
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_DAMAGE = 111,  // outgoing weapon-damage multiplier
  // Player / vehicle fun. KILL_SELF is a worker-safe health write; FORCE_RAGDOLL mutates
  // the ped task tree so it runs game-thread (like the explosions). RAINBOW_PAINT / NOCLIP
  // are continuous worker-tick effects (toggles), same class as RAINBOW_NEON.
  GTAV_NATIVE_SHELL_ACTION_KILL_SELF = 112,             // set player health to 0 (worker-safe)
  GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL = 113,         // ragdoll the player ped (game-thread)
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_PAINT = 114,  // per-tick body-paint hue cycle
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NOCLIP = 115,         // free-move the frozen player entity
  // Debug / Native Explorer overlay: a read-only on-screen panel of frame-hook, job-queue,
  // native-resolution and weapon-fx diagnostic counters. Worker-safe (render only).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_DEBUG_OVERLAY = 116,
  // Self: give single-player cash. Writes SP0/SP1/SP2_TOTAL_CASH via STAT_SET_INT, so the
  // increment lands on whichever protagonist is active (worker-safe stat write). The
  // dollar amount to add rides in the row's `param`.
  GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY = 117,
  // Vehicle: hold-the-brake hard stop. Worker-tick toggle that bleeds forward speed toward
  // zero while the brake control is held (SET_VEHICLE_FORWARD_SPEED, already worker-safe).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_BRAKE = 118,
  // Menu customisation cyclers. Pure display/state changes (no game native): they recolour
  // the menu (CYCLE_THEME) and move the panel left/right (CYCLE_REGION). Handled in menu.c
  // before the native-feature gate since they touch only the bridge's render state.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_THEME = 119,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_REGION = 120,
  // Keybind cyclers: one per fixed bindable feature (slot). Each cycles its assigned button
  // combo over a curated list (incl. "Off"); the chosen combo fires the feature while the
  // menu is closed (see gtav_pad_input_map). Handled as cyclers in features.cpp, which pushes
  // the resolved bind table to pad_input. Keep these contiguous (slot = action - first).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0 = 121,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_1 = 122,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_2 = 123,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_3 = 124,
  // Content: spawner (CREATE_PED/CREATE_OBJECT, game-thread + model streaming, like
  // SPAWN_VEHICLE). param = model joaat hash.
  GTAV_NATIVE_SHELL_ACTION_SPAWN_PED = 125,
  GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT = 126,
  // Effects: night vision / thermal are toggles re-asserted from the game-thread tick.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NIGHT_VISION = 127,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEETHROUGH = 128,
  // Effects cyclers: stage the index worker-side (Left/Right), apply on the game thread
  // (Cross) since the timecycle/animpostfx natives touch render-script state.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE = 129,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX = 130,
  // Ped control: enumerate nearby peds and retask them (game-thread one-shot actions).
  GTAV_NATIVE_SHELL_ACTION_PEDS_ATTACK_PLAYER = 131,
  GTAV_NATIVE_SHELL_ACTION_PEDS_FLEE_PLAYER = 132,
  GTAV_NATIVE_SHELL_ACTION_PEDS_STOP = 133,
  // Minigame "fun modes": each is a mutually-exclusive per-frame MODE feature (the noclip
  // pattern) whose per-frame work runs on the game/script thread via the frame-hook tick, so
  // toggling refuses until the hook is live. Phase A (Riot..Storm) uses only already-pinned
  // natives; Phase B (Blackout..Flood) needs natives not yet in the pinned table and degrades
  // to an "unavailable"/locked row until they are resolved (make feature-native-header). The
  // ids are allocated together so this stable wire/save ABI does not move across the rollout.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RIOT = 134,      // nearby peds turn hostile + swarm
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_METEOR = 135,    // explosions rain around the player
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_STORM = 136,     // persistent thunderstorm + midnight
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_INFERNO = 137,   // fires keep igniting around you
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RAGDOLL = 138,   // nearby peds keep ragdolling
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_BLACKOUT = 139,  // Phase B: kill city/streetlights
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_ZOMBIES = 140,   // Phase B: spawned hostile horde
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_FLOOD = 141,     // Phase B: experimental rising water
  // Internal game-thread job (no menu row): warm a model into the streamer on the GAME thread
  // via the frame-hook drain, so a confirmed spawn/skin finds it resident and lands on the
  // first hook fire. Enqueued by gtav_features_prewarm_enqueue(); never dispatched from a row.
  GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL = 142,
  // Feature growth: a game-thread ragdoll-nearby one-shot (like the ped-control actions) and
  // two worker-safe vehicle tyre ops.
  GTAV_NATIVE_SHELL_ACTION_RAGDOLL_NEARBY = 143,  // ragdoll nearby peds (game-thread)
  GTAV_NATIVE_SHELL_ACTION_BURST_TYRES = 144,     // pop the current vehicle's tyres
  GTAV_NATIVE_SHELL_ACTION_FIX_TYRES = 145,       // repair the current vehicle's tyres
  // Session: experimental "Skip Prologue" (behind GTAV_MENU_ENABLE_PROLOGUE_SKIP). Sets the
  // account-level prologue-complete profile flag and relaunches the SP session so it takes
  // effect (drops into post-prologue Los Santos). The relaunch native rebuilds the entire
  // script context, so this is a game-thread-gated one-shot drained by the frame hook.
  GTAV_NATIVE_SHELL_ACTION_SKIP_PROLOGUE = 146,
  // Menu Settings cyclers for the off-thread render cadence + toast dwell. Pure bridge state
  // (no game native), handled in menu.c like CYCLE_THEME/CYCLE_REGION: Worker Hz drives the
  // render over-sample rate (anti-flicker), Toast Time the notification dwell.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WORKER_HZ = 147,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TOAST_TIME = 148,
  // --- Menyoo-gap feature wave 1 ---
  // Self: set the active character's cash to an exact amount (worker-safe STAT_SET_INT
  // cycler, same store as GIVE_MONEY but absolute set instead of add).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CASH = 149,
  // Self: max every SP skill stat + special-ability capacity (worker-safe STAT_SET_INT batch).
  GTAV_NATIVE_SHELL_ACTION_MAX_ALL_STATS = 150,
  // Self: keep a parachute granted (re-grant GADGET_PARACHUTE on the game-thread tick).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_PARACHUTES = 151,
  // Vehicles: spawner defaults -- new vehicles arrive fully performance-modded / indestructible.
  // Display toggles (no native at toggle time); applied at vehicle create-time on the spawn job.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_MAXED = 152,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_INVINCIBLE = 153,
  // Vehicles: delete the player's current vehicle (DELETE_ENTITY, game-thread queued like spawn).
  GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE = 154,
  // World: thin ped + vehicle population (SET_*_POPULATION_BUDGET 0, re-asserted on the
  // game-thread tick because the engine re-raises the budget).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_THIN_POPULATION = 155,
  // Internal game-thread job (no menu row): stream the highlighted vehicle's preview texture
  // dict so the menu can DRAW_SPRITE its website thumbnail. REQUEST_STREAMED_TEXTURE_DICT is a
  // streaming native (NOT worker-safe, like PREWARM_MODEL), so the request runs on the frame-hook
  // drain; the worker reads a loaded flag and draws the sprite. Enqueued by
  // gtav_features_preview_set_target(); behind GTAV_MENU_ENABLE_VEHICLE_PREVIEW.
  GTAV_NATIVE_SHELL_ACTION_PREVIEW_TXD = 156,
  // --- Menyoo-informed feature wave 2 ---
  // Spawned content lifecycle: delete only entities the menu created and tracked.
  // DELETE_ENTITY / DELETE_PED tear down entities, so these are game-thread queued.
  GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES = 157,
  GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS = 158,
  GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS = 159,
  GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL = 160,
  // Companions / bodyguards. SPAWN_BODYGUARD allocates a ped and joins it to the player's
  // group, so it uses the same game-thread model-streaming lane as SPAWN_PED.
  GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD = 161,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_HEALTH = 162,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ARMOR = 163,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_INVINCIBLE = 164,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_FORMATION = 165,
  GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS = 166,
  GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS = 167,
  // Vehicle cruise: list-cycled target speed plus a held toggle reasserted on the
  // game-thread vehicle tick with SET_VEHICLE_FORWARD_SPEED.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CRUISE_SPEED = 168,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_CRUISE_CONTROL = 169,
  // --- Menyoo-gap feature wave 2 ---
  // Self: police + ambient peds + gangs all ignore the player (held toggle, re-asserted
  // on the game-thread tick).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_IGNORED_BY_ALL = 170,
  // Weapons: top up ammo to max for the weapons the player already carries (game-thread,
  // ped weapon-inventory natives like GIVE_WEAPON).
  GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO = 171,
  // Companions: how tracked bodyguards react to threats (Passive / Defensive / Aggressive).
  // Drives combat attributes + event blocking on the game-thread bodyguard tick.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_AGGRESSION = 172,
  // Companions: the weapon tracked bodyguards are armed with. An unarmed guard cannot defend
  // the player, so this loadout is what makes the aggression presets actually do something.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_WEAPON = 173,
  // Ped browser category filter (skin changer / spawn ped / bodyguard). Re-filters the shared
  // ped catalog by category; owned in native_bridge.cpp like the vehicle-class filter (40).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PED_CATEGORY = 174,
  // Object browser category filter (Spawn > Objects). Re-filters the object catalog by
  // category; owned in native_bridge.cpp like the vehicle-class / ped-category filters.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_CATEGORY = 175,
  // Object spawn-placement controls (feature-layer list cyclers; they tune file-local state
  // that spawn_object_model_hash() reads when placing the prop). SPAWN_AT picks the reference
  // frame (player vs gameplay camera), DISTANCE how far in front, HEADING the prop's facing,
  // ON_GROUND whether to snap it to the ground (PLACE_OBJECT_ON_GROUND_PROPERLY).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_SPAWN_AT = 176,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_DISTANCE = 177,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_HEADING = 178,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_ON_GROUND = 179,
  // Companions: combat accuracy (hit chance) of tracked bodyguards, and a minimap-blip toggle
  // that tags each tracked guard on the radar.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ACCURACY = 180,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_BLIPS = 181,
  // Vehicle Autopilot (Vehicles > Autopilot). MODE cycles Off / Wander / To Waypoint,
  // AGGRESSION the driving-style flags (obey vs ignore traffic), SPEED the cruise speed.
  // STOP disengages. All four only mutate file-local state on the worker; the actual
  // TASK_VEHICLE_* / SET_DRIVE_TASK_* / CLEAR_PED_TASKS calls run on the game-thread
  // frame-hook tick (autopilot_game_thread_tick), like noclip.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE = 182,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION = 183,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED = 184,
  GTAV_NATIVE_SHELL_ACTION_STOP_AUTOPILOT = 185,
  // Self > Scenarios. START_SCENARIO enters the highlighted scenario on the player; its .param
  // is the gtav_scenario_catalog index (not a hash), which the game-thread handler reads to pass
  // the scenario name to TASK_START_SCENARIO_IN_PLACE. STOP_SCENARIO clears it (CLEAR_PED_TASKS).
  // Both mutate the ped task tree -> game-thread gated. CYCLE_SCENARIO_CATEGORY re-filters the
  // scenario browser by category; owned in native_bridge.cpp like the other category filters.
  GTAV_NATIVE_SHELL_ACTION_START_SCENARIO = 186,
  GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO = 187,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_SCENARIO_CATEGORY = 188,
  // Spawn > Objects > interactive placement. MOVE_LAST_OBJECT enters a controller-driven
  // move/reorient mode for the most-recently-spawned object: it closes the menu and the
  // game-thread driver (object_move.inc) repositions the prop until Cross commits / Circle
  // reverts. TOGGLE_AUTO_EDIT_OBJECT (a saved display toggle) makes a spawn auto-enter that
  // mode. The mode itself is transient state, NOT a saved toggle, so it has no TOGGLE_ row.
  GTAV_NATIVE_SHELL_ACTION_MOVE_LAST_OBJECT = 189,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_EDIT_OBJECT = 190,
  // Self > Teleport > To Objective. Teleports to the current mission/objective marker
  // (a script blip) the same streaming-aware, worker-safe way as TELEPORT_WAYPOINT --
  // freeze, move, stream-in, settle. Not game-thread gated.
  GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE = 191,
  // Vehicles > Controls. One-shot, modify-existing field stores on the player's current
  // vehicle (worker-safe class, like the LSC setters / SET_VEHICLE_ENGINE_ON). CYCLE_VEHICLE_DOOR
  // is a pure selection cycler (picks which door OPEN/CLOSE acts on); HEADLIGHTS/LOCK are
  // stage/apply cyclers; the rest are actions. None are game-thread gated.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_DOOR = 192,
  GTAV_NATIVE_SHELL_ACTION_OPEN_VEHICLE_DOOR = 193,
  GTAV_NATIVE_SHELL_ACTION_CLOSE_VEHICLE_DOOR = 194,
  GTAV_NATIVE_SHELL_ACTION_ROLL_DOWN_WINDOWS = 195,
  GTAV_NATIVE_SHELL_ACTION_ROLL_UP_WINDOWS = 196,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_HEADLIGHTS = 197,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_LOCK = 198,
  GTAV_NATIVE_SHELL_ACTION_RAISE_VEHICLE_ROOF = 199,
  GTAV_NATIVE_SHELL_ACTION_LOWER_VEHICLE_ROOF = 200,
  // Self > Wardrobe (outfit editor). Modify-existing-ped component/prop variation -- worker-safe
  // class like the LSC setters (NOT the allocation-class SET_PLAYER_MODEL). SLOT picks the
  // component/prop; STYLE/TEXTURE step+apply its drawable/texture live; RESET restores the model
  // default; CLEAR_PROP removes a prop slot; SAVE/APPLY persist+restore one outfit (profile v8).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_SLOT = 201,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_STYLE = 202,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_TEXTURE = 203,
  GTAV_NATIVE_SHELL_ACTION_WARDROBE_RESET = 204,
  GTAV_NATIVE_SHELL_ACTION_WARDROBE_CLEAR_PROP = 205,
  GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT = 206,
  GTAV_NATIVE_SHELL_ACTION_APPLY_OUTFIT = 207,
  // Vehicles > Saved vehicle (garage). SAVE_VEHICLE captures the current vehicle's model hash +
  // indexed mod loadout worker-direct (getters) and persists it (profile v9). SPAWN_SAVED_VEHICLE
  // respawns it built -- it allocates (CREATE_VEHICLE), so it is game-thread gated like the spawn
  // rows and re-applies the saved mods in the post-create hook.
  GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE = 208,
  GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE = 209,
  // Self > Emotes. PLAY_EMOTE records the catalog index worker-direct (refuses until the frame
  // hook is live); the game-thread emote tick streams the dict and plays the clip. STOP_EMOTE
  // flags the tick to CLEAR_PED_TASKS. The picker row's .param is the gtav_anim_catalog index.
  GTAV_NATIVE_SHELL_ACTION_PLAY_EMOTE = 210,
  GTAV_NATIVE_SHELL_ACTION_STOP_EMOTE = 211,
  // Weapons > Weapon Tint. Stage/apply cycler: Left/Right pick a tint, Cross recolours the
  // equipped weapon (GET_CURRENT_PED_WEAPON -> SET_PED_WEAPON_TINT_INDEX). Worker-direct
  // modify-existing weapon-field setter (NOT the give/remove weapon-manager lane).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_TINT = 212,
  // Spawn > Spawned Entities > Save/Load Map (Spooner-lite). SAVE_MAP captures the live spawned
  // rosters (kind + model + transform) to /data/GTAVMenu/custom/maps/active.map.cfg,
  // worker-direct (getters + file).
  // LOAD_MAP reads the file and kicks off the streaming re-spawn; LOAD_MAP_STEP is the per-entity
  // game-thread job (REQUEST_MODEL -> CREATE_* at the saved transform -> track), self-requeuing
  // while the model streams and chaining to the next entity. LOAD_MAP_STEP has no menu row.
  GTAV_NATIVE_SHELL_ACTION_SAVE_MAP = 213,
  GTAV_NATIVE_SHELL_ACTION_LOAD_MAP = 214,
  GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP = 215,
  // Fly modes (game-thread drivers, velocity-driven; distinct from noclip's freeze-teleport).
  // CYCLE_FLY_MODE: Self > Movement > Fly (Off/On/Fast) flies the player ped on foot.
  // CYCLE_VEHICLE_FLY: Vehicles > Fly (Off/On/Fast) flies the current vehicle. Both are LIST
  // cyclers backed by volatile mode state (not saved toggle bits); the driver self-refuses until
  // the frame hook is live (the velocity/gravity/camera natives fault off the game thread).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE = 216,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY = 217,
  // Weapons > Gun Toolkit. LIST cycler (Off/Gravity/Teleport/Kaboom) selecting the active gun;
  // the game-thread driver raycasts the camera aim and applies the effect on the fire button.
  // Volatile mode state (not a saved toggle); self-refuses until the frame hook is live (the
  // raycast/explosion natives fault off the game thread).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN = 218,
  // World > Spectacle. CYCLE_WIND (Calm/Breezy/Windy/Gale) is a worker-safe global wind setter
  // (like weather). CYCLE_CAM_SHAKE (Off/Small/Medium/Large) is a game-thread re-asserted camera
  // shake (volatile mode state). SPAWN_FIREWORKS is a game-thread PTFX streaming job (request the
  // named asset, then start the non-looped fx at the player) -- gated to the frame hook like the
  // spawn actions.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND = 219,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE = 220,
  GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS = 221,
  // --- Fun + parity wave (Self / Vehicles / Weapons) ---
  // Weapons: No-Reload / Infinite Clip. SET_PED_INFINITE_AMMO_CLIP is re-asserted on the
  // game-thread tick (it touches the ped weapon state); the toggle itself is a worker-side flag
  // flip (the native is not entity-allocation, so it is not gated, just re-asserted game-thread).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RELOAD = 222,
  // Vehicles: Slippery Roads -- constant reduced tyre grip (ice physics), distinct from Drift Mode
  // (which is speed-gated). SET_VEHICLE_REDUCE_GRIP is a worker-safe field store re-asserted each
  // tick on the current vehicle, the same lane as Drift.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLIPPERY = 223,
  // Self > Teleport: warp the player into their last-driven vehicle (GET_LAST_DRIVEN_VEHICLE ->
  // SET_PED_INTO_VEHICLE). Both walk the ped/vehicle managers, so this is a game-thread-gated
  // one-shot drained by the frame hook (locked until the hook is live), like the spawn rows.
  GTAV_NATIVE_SHELL_ACTION_TELEPORT_LAST_VEHICLE = 224,
  // Self > Free Camera: enter a DETACHED cinematic camera (distinct from Noclip, which moves the
  // ped). Enters a transient mode -- closes the menu, Circle exits -- driven on the game-thread
  // frame hook (the scripted-cam natives touch render-script state). Refuses until the hook is
  // live, like the fly / gun-toolkit modes. CYCLE_FREE_CAM_SPEED tunes the fly speed worker-side.
  GTAV_NATIVE_SHELL_ACTION_FREE_CAM = 225,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED = 226,
  // --- Fun wave 2 (Vehicles / Weapons) ---
  // Vehicles: Rocket Boost -- a one-shot vertical impulse (APPLY_FORCE_TO_ENTITY) on the current
  // vehicle. Touches a live CVehicle, so it is a game-thread-gated one-shot (frame-hook drain).
  GTAV_NATIVE_SHELL_ACTION_VEHICLE_ROCKET_BOOST = 227,
  // Weapons > Combat Effects: Explosive Melee -- a held toggle whose game-thread tick spawns a
  // small owned explosion at each unarmed/melee impact (reuses the explosive-ammo machinery).
  // Refuses until the hook is live, like Explosive/Fire Ammo.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_MELEE = 228,
  // --- Menyoo parity depth (Spawn > Spawned Entities) ---
  // Act on the most-recently spawned/selected entity in the menu's roster. DUPLICATE re-spawns the
  // same model nearby (reuses the spawn lane). ATTACH/DETACH bind it to the player.
  // CYCLE_ENTITY_ALPHA fades it (SET_ENTITY_ALPHA). All but the alpha cycler touch entity managers
  // -> game-thread gated.
  GTAV_NATIVE_SHELL_ACTION_DUPLICATE_LAST_ENTITY = 229,
  GTAV_NATIVE_SHELL_ACTION_ATTACH_LAST_ENTITY = 230,
  GTAV_NATIVE_SHELL_ACTION_DETACH_LAST_ENTITY = 231,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA = 232,
  // Menu customisation: the in-menu custom-theme editor. CYCLE_CUSTOM_COLOR steps one colour
  // channel (the row's param selects it: 0-2 accent RGB, 3-5 selection RGB, 6-8 panel RGB) of the
  // mutable "Custom" theme; RESET_CUSTOM_THEME re-seeds it to the default palette. Pure render
  // state -- handled in the bridge, not the native-feature dispatch.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_COLOR = 233,
  GTAV_NATIVE_SHELL_ACTION_RESET_CUSTOM_THEME = 234,
  // Menu accessibility/feel cyclers (v13): Reduce Motion (Full/Reduced) suppresses the menu's
  // animations; Nav Delay / Nav Speed tune the D-pad auto-repeat cadence. All three are pure
  // bridge/menu state (no game native), handled in menu.c like the other Menu Settings cyclers.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_MOTION = 235,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_DELAY = 236,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_NAV_SPEED = 237,
  // Optional touchpad gesture input (Off/On). Pure bridge state (read by the live pad poll path),
  // handled in menu.c like the other Menu Settings cyclers.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_TOUCHPAD = 238,
  // Emotes: loop the selected emote until Stop, vs play it once (default off = play once).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_EMOTE_LOOP = 239,
  // Saved-vehicle / saved-outfit slot selectors (10 slots each): pick which slot Save/Spawn/Apply
  // act on. Pure worker-side selection state.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_SAVED_VEHICLE_SLOT = 240,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_OUTFIT_SLOT = 241,
  // Menu panel width (Narrow/Normal/Wide). Pure bridge render state, handled in menu.c like the
  // other Menu Settings cyclers.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_PANEL_WIDTH = 242,
  // Weapons > Attachments (weapon_attachments.inc): per-slot staged Off/On cyclers committed to the
  // equipped weapon by APPLY_ATTACHMENTS. The give/remove-component natives are game-thread-only
  // (action_needs_main_thread), so the cyclers stage worker-side and Apply rides the gated lane;
  // the per-weapon component catalog lives in weapon_attachments.inc (kWAttach).
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SUPP = 243,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SCOPE = 244,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_GRIP = 245,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_CLIP = 246,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_FLASH = 247,
  GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS = 248,
  GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS = 249,
  // Cancel an in-progress versioned/legacy map load. The worker clears the published load claim;
  // any already-created entities remain roster-owned and can be removed with Clear Spawned.
  GTAV_NATIVE_SHELL_ACTION_CANCEL_MAP_LOAD = 250,
  // 251 was PROBE_CUSTOM_MOUNT (retired in-game custom-root probe); reserved, never reuse.
  // 252 was MOUNT_CUSTOM_DEVICE (retired P1 gtavmenu:/ device mount); reserved, never reuse.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_PRESERVE_SPEED = 253,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_REPLACE_PREVIOUS = 254,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_AIRCRAFT_IN_FLIGHT = 255,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_POPULATION_DENSITY = 256,
  GTAV_NATIVE_SHELL_ACTION_SPAWN_CROWD = 257,
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_MAINTAIN_CROWD = 258,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_SPEEDOMETER_LAYOUT = 259,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_LANGUAGE = 260,
  // 261-263 were the retired P1 stock-texture admission/inspection/request; reserved, never reuse.
  // 264-265 were the retired DLC-route census and effective dlclist capture; reserved, never reuse.
  // CUSTOM_STREAM=1 host-debug texture card for the last requested pack card. SHOW (operator
  // param 0) checks the dict is loaded and resolves, then the worker draws the sprite and keeps
  // the resident dict touched (internal param 1). RELEASE (operator param 0) hides the card; once
  // every draw list that referenced it has drained, the worker queues the release (internal 1).
  GTAV_NATIVE_SHELL_ACTION_SHOW_PACK_CARD = 266,
  GTAV_NATIVE_SHELL_ACTION_RELEASE_PACK_CARD = 267,
  // 268-269 were the retired P1 loose stream-entry and read probes; reserved, never reuse.
  // CUSTOM_STREAM=1 runtime pack lane (resources/pack.cfg of the active pack): register the pack's
  // archive through the engine memory device the way the game's own changeset code does
  // (AddImageToList + LoadImage), then request one card's texture dictionary (param = card index).
  GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK = 270,
  GTAV_NATIVE_SHELL_ACTION_REQUEST_PACK_CARD = 271,
  // Read-only streaming-info words of the registered archive and its members.
  GTAV_NATIVE_SHELL_ACTION_INSPECT_PACK = 272,
  // Request only the registered archive's own streamable so its TOC is re-parsed (no member).
  GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE = 273,
  // Load one of the pack's data files (param = data row) through its data-file mounter, once.
  GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA = 274,
  // Parse the queued data row through the parser pump once its read completed (retry until ready).
  GTAV_NATIVE_SHELL_ACTION_PUMP_PACK_DATA = 275,
  // Request one of the pack's archetype definitions (.ptyp, param = typ row) via DLC_ITYP_REQUEST.
  GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP = 276,
  // Request one of the pack's map-data members (.pmap, param = map row) keep-resident, then finish
  // its activation (initialised bit, box-streamer state, IPL enable) once it has loaded.
  GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP = 277,
  GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP = 278,
  // Add the pack's text labels (e.g. vehicle gameNames) to the merged text map, once.
  GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS = 279,
  // Custom Packs page: load the active pack in one press (worker-side state machine over 270-279).
  GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD = 280,
  // Custom Packs page: teleport to a pack map row's declared place (param = map row), or to a
  // descriptor `place` row (param = 0x100 + place row).
  GTAV_NATIVE_SHELL_ACTION_TELEPORT_PACK_MAP = 281,
  // Custom Packs page: add/remove an installed pack (param = page order) in packs/active.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE = 282,
  // Request one of the pack's static collision bounds (.pbn, param = bounds row) keep-resident;
  // repeated calls confirm the composite root, its box and its physics instances.
  GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS = 283,
  // Weapon Browser "Category" selector (All / each weapon category / Custom). Handled in the bridge
  // like the other browser selectors: stepping it re-filters the weapon rows below it.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_CATEGORY = 284,
  // Custom Packs page: apply a pack timecycle modifier (descriptor `spawn timecycle` row, param =
  // joaat of its name) at full strength, or clear it when it is the applied one. Game thread.
  GTAV_NATIVE_SHELL_ACTION_APPLY_PACK_TIMECYCLE = 285,
  // Custom Packs page: play a pack particle effect once at the player (descriptor `spawn ptfx
  // <asset>:<effect>` row, param = joaat of that model text). Requests the pack dictionary and
  // re-queues itself (bounded) until it is loaded. Game thread.
  GTAV_NATIVE_SHELL_ACTION_PLAY_PACK_PTFX = 286,
  // Custom Packs page "Uninstall < id >" (pick then apply): param 1/2 step the target installed
  // pack, 0 removes it from packs/active and packs/installed and renames its pack.cfg to
  // pack.cfg.uninstalled (files stay). Worker side, behind the menu's two-press confirm.
  GTAV_NATIVE_SHELL_ACTION_UNINSTALL_PACK = 287,
  // Custom Packs page: toggle a pack weapon component (descriptor `spawn component
  // <weapon>:<component>` row, param = joaat of that model text) on that weapon, giving and
  // equipping the weapon first when it is not the current one. Game thread.
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_COMPONENT = 288,
  // One-press load stage after the maps: hide the stock map entities of a descriptor `hide` row
  // (param = merged hide row) with CREATE_MODEL_HIDE for the session; param 0xffff (host
  // `pack-unhide`) removes every applied row with REMOVE_MODEL_HIDE. Game thread.
  GTAV_NATIVE_SHELL_ACTION_PACK_HIDE = 289,
  // Host `menu-ctl.sh export-templates`: read one allow-listed retail file (param = row of
  // custom_retail_export.inc, the vehicle converter's encrypted templates) through the game's
  // common.rpf packfile on the main thread, then write the plaintext to
  // /data/gtavmenu/custom/exports/<name> on the worker (temp + rename). No arbitrary paths.
  GTAV_NATIVE_SHELL_ACTION_EXPORT_RETAIL_FILE = 290,
  // Custom Packs > Manage Packs "Revert overrides": put every stock member the loaded packs
  // override back on its stock archive (handle + overlay node). All or nothing: refuses while any
  // target is loaded, requested or referenced. Game thread, behind the menu's two-press confirm.
  GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES = 291,
  // Vehicles > Spawn Options "Spawn Upgraded" (a toggle): new vehicles get Spawn Maxed's
  // performance and the top part of every visual slot their mod kit has (lsc.inc).
  GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_UPGRADED = 292,
  // LS Customs "Part Type" (list_kinds.def SETTING): one of the kit slots the current car has parts
  // for (every SET_VEHICLE_MOD slot 0..49 but the toggle mods); "Part" (CHOICE): stage one of that
  // slot's parts, Cross fits it. Worker-safe like the per-slot mod rows.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KIT_SLOT = 293,
  GTAV_NATIVE_SHELL_ACTION_CYCLE_KIT_PART = 294,
  // Weapons > Attachments "Component" (list_kinds.def CHOICE): one of the components the equipped
  // weapon's meta lists (a pack weapon's own weapons.meta, else the Attachments table); Cross fits
  // or removes it on the game thread (weapon_components.inc). Worker-staged like the tint row.
  GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_COMPONENT = 295,
  // Sentinel: one past the highest action id, for iterating the action space (e.g. the
  // list-value push loop in menu.c). Not a dispatchable action.
  GTAV_NATIVE_SHELL_ACTION_COUNT,
};

void gtav_native_bridge_set_speedometer_layout(uint32_t layout);
uint32_t gtav_native_bridge_speedometer_layout(void);
void gtav_native_bridge_set_language(uint32_t id);

enum {
  GTAV_NATIVE_CANARY_DRAW_RECT = 1u << 0,
  GTAV_NATIVE_CANARY_GET_GAME_TIMER = 1u << 1,
};

typedef struct GtavNativeAddressTable {
  uint32_t abi_version;
  uint32_t reserved;
  uint64_t get_frame_count;
  uint64_t is_control_pressed;
  uint64_t is_control_just_pressed;
  uint64_t draw_rect;
  uint64_t begin_text_command_display_text;
  uint64_t add_text_component_substring_player_name;
  uint64_t end_text_command_display_text;
  uint64_t set_text_scale;
  uint64_t set_text_colour;
  uint64_t set_text_font;
  uint64_t set_text_centre;
  uint64_t set_text_wrap;
  uint64_t set_text_justification;
  uint64_t set_text_drop_shadow;
  uint64_t set_text_dropshadow;
  uint64_t set_text_outline;
  uint64_t is_disabled_control_pressed;
  uint64_t is_disabled_control_just_pressed;
  uint64_t is_disabled_control_just_released;
  uint64_t disable_control_action;
  uint64_t set_input_exclusive;
  uint64_t get_game_timer;
  uint64_t draw_sprite;
  uint64_t has_streamed_texture_dict_loaded;
  uint64_t request_streamed_texture_dict;
  uint64_t player_id;
  uint64_t player_ped_id;
  uint64_t set_player_invincible;
  uint64_t set_entity_invincible;
  uint64_t set_entity_health;
  uint64_t set_ped_armour;
  uint64_t set_player_wanted_level;
  uint64_t set_player_wanted_level_now;
} GtavNativeAddressTable;

typedef struct GtavNativeShellSnapshot {
  uint32_t abi_version;
  uint32_t flags;
  uint32_t visible;
  uint32_t selected_index;
  uint32_t item_count;
  uint32_t telemetry_enabled;
  uint32_t last_action;
  uint32_t reserved;
  uint64_t frame_count;
  uint64_t worker_tick;
  char selected_label[GTAV_NATIVE_SHELL_LABEL_LEN];
  char mode[GTAV_NATIVE_SHELL_MODE_LEN];
} GtavNativeShellSnapshot;

typedef struct GtavNativeShellState {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  GtavNativeShellSnapshot snapshot;
} GtavNativeShellState;

extern GtavNativeShellState gtav_native_shell_state;
extern const char gtav_native_shell_state_marker[];

void gtav_native_bridge_init(const GtavNativeAddressTable* table);
void gtav_native_bridge_set_canary(uint32_t flags, uint32_t interval, uint32_t max_frames);
void gtav_native_bridge_set_draw_rect_call_override(uint64_t address);
void gtav_native_bridge_set_visible(int visible);
uint32_t gtav_native_bridge_poll_input_command(uint64_t worker_tick, int visible);
void gtav_native_bridge_worker_tick(uint64_t worker_tick, int visible);
// Render park: while parked, the worker tick refreshes its status snapshot but issues NO
// drawing/getter natives, so the menu stops feeding GTA's draw lists. The teardown/suspend guard
// (quit_guard.c) parks the renderer alongside the scePad input path on a force-close / rest-mode
// suspend: a worker that keeps calling DRAW_* keeps the GPU graphics pipe non-idle, which stalls
// the OS app-suspend handshake and trips the SYSTEM_SUSPEND_BLOCK_TIMEOUT crash (0xa0024301).
// Reversible: unpark re-arms rendering if the game thread comes back (e.g. a long load finished).
void gtav_native_bridge_park(void);
void gtav_native_bridge_unpark(void);
int gtav_native_bridge_is_parked(void);
// Runtime-tunable worker text-render cadence (ticks between renders); 0 ignored. Lets us
// sweep the smooth-game vs flicker-free-menu trade-off without a rebuild.
void gtav_native_bridge_set_render_interval(uint32_t interval);
uint32_t gtav_native_bridge_render_interval(void);
// On-screen toast confirmation. result_code: GTAV_FEATURE_RESULT_OK/FAILED/UNAVAILABLE
// (1/2/3) -> green/red/amber; other -> off-white. text is copied into a fixed slot
// (<=79 chars, truncated). No-op when toasts are built out, so the ABI is stable in every
// build. Same worker thread as the renderer; no locking.
void gtav_native_bridge_push_toast(const char* text, uint32_t result_code);
// Toast display duration in worker ticks (clamped 30..1800; out-of-range ignored).
void gtav_native_bridge_set_toast_ticks(uint32_t ticks);
uint32_t gtav_native_bridge_toast_ticks(void);
// Persisted selection index for the Worker Hz / Toast Time cyclers (stored as plain data; the
// choice tables + apply live in menu.c). Round-tripped through the feature profile (v11).
void gtav_native_bridge_set_worker_hz_index(uint32_t index);
uint32_t gtav_native_bridge_worker_hz_index(void);
void gtav_native_bridge_set_toast_time_index(uint32_t index);
uint32_t gtav_native_bridge_toast_time_index(void);
// v13 accessibility/feel tuners (round-tripped through the feature profile). Reduce Motion (0/1) is
// read by the draw path's animation helpers; the Nav Delay / Nav Speed indices are plain data (the
// choice tables + apply, which actuate the pad mapper, live in menu.c).
void gtav_native_bridge_set_reduce_motion(int on);
int gtav_native_bridge_reduce_motion(void);
void gtav_native_bridge_set_nav_delay_index(uint32_t index);
uint32_t gtav_native_bridge_nav_delay_index(void);
void gtav_native_bridge_set_nav_speed_index(uint32_t index);
uint32_t gtav_native_bridge_nav_speed_index(void);
// v14: optional touchpad gesture input (default off). Read by the live pad poll path.
void gtav_native_bridge_set_touchpad_enabled(int on);
int gtav_native_bridge_touchpad_enabled(void);
void gtav_native_bridge_frame_tick(uint64_t frame_tick, int visible);
int gtav_native_bridge_phase_tick(uint64_t epoch, int visible);
void gtav_native_bridge_tick(uint64_t worker_tick, int visible);
void gtav_native_bridge_next(void);
void gtav_native_bridge_prev(void);
// Page the cursor by one visible window (kShellVisibleRows), clamped at the ends with no
// wrap. page(dir<0)=up, dir>=0=down. Reached from the touchpad flick and the mailbox.
void gtav_native_bridge_page(int dir);
void gtav_native_bridge_page_prev(void);
void gtav_native_bridge_page_next(void);
// Page, but roll into the previous/next sibling submenu when already at the list edge on a child
// screen (the "page to the edge, then flip tabs" idiom). dir<0 = prev, dir>=0 = next. The
// PAGE_PREV/PAGE_NEXT command (touchpad flick, mailbox).
void gtav_native_bridge_page_or_sibling(int dir);
// Seek the cursor to the previous/next first-letter boundary in the list (mailbox letter_prev /
// letter_next; no controller button). dir<0 = prev.
void gtav_native_bridge_letter_jump(int dir);
// Jump the cursor to the first / last selectable row of the page (L1 / R1 while the menu is open),
// skipping heading/info rows like next/prev; an all-info page falls back to its first / last row.
void gtav_native_bridge_home(void);
void gtav_native_bridge_end(void);
uint32_t gtav_native_bridge_activate(void);
// D-pad Left/Right: adjust the selected list-row value (returns its action with the
// direction encoded in last_action_param: 2 = left/prev, 1 = right/next) without moving
// the cursor. On a toggle row Left sets OFF and Right sets ON: the row action (param = the row's
// param, like Cross) is returned only when the state has to flip, NONE otherwise. On action and
// submenu rows Left is Back. adjust(dir<0)=left, dir>=0=right.
uint32_t gtav_native_bridge_adjust(int dir);
uint32_t gtav_native_bridge_left(void);
uint32_t gtav_native_bridge_right(void);
// Pin/unpin the selected row to the global Quick menu (or toggle a browser favorite on a spawn/skin
// row). Bound to R3 (right-stick click); also reachable via the "pin" mailbox command.
void gtav_native_bridge_pin_selected(void);
// 1 when the currently-selected row is a value cycler (SHELL_ROW_LIST). The pad layer uses
// this to allow Left/Right auto-repeat only on cyclers -- on other rows Left is a one-shot
// Back, so repeating it would walk the user out of the menu.
int gtav_native_bridge_selected_is_cycler(void);
// 1 when the currently-selected row is a toggle (SHELL_ROW_TOGGLE): its Left/Right Off/On step
// confirms with a toast like Cross does.
int gtav_native_bridge_selected_is_toggle(void);
// Parameter of the item that produced the last activate/adjust. Vehicle spawn rows emit
// model joaat hashes; list rows emit direction (2 = left/prev, 1 = right/next).
uint32_t gtav_native_bridge_last_action_param(void);
// Update the menu's display state for a feature toggle bitmask.
void gtav_native_bridge_set_feature_toggles(uint64_t mask);
// Push the current display value for a SHELL_ROW_LIST cycler (e.g. weather/time).
void gtav_native_bridge_set_list_value(uint32_t action, const char* value);
// A pick-then-apply row whose staged value was not applied reverts when the cursor leaves it
// (another row or menu, Back, menu closed). Returns 1 once that happened, with the action, the
// step direction to replay (2 = prev, 1 = next) and the step count that undo the pick; 0 otherwise.
int gtav_native_bridge_take_staged_revert(uint32_t* action, uint32_t* param, uint32_t* steps);
// How a list row behaves (include/gtavmenu/list_kinds.def): CHOICE rows stage on Left/Right and
// apply on Cross; SETTING rows apply on Left/Right and ignore Cross; SETTING_TOAST rows are
// SETTING rows whose step also shows the feature's toast; SETTING_APPLY rows apply on Left/Right
// and Cross applies the shown value again (it can differ from the game's). Unlisted actions read
// as CHOICE.
enum {
  GTAV_LIST_KIND_CHOICE = 0,
  GTAV_LIST_KIND_SETTING = 1,
  GTAV_LIST_KIND_SETTING_TOAST = 2,
  GTAV_LIST_KIND_SETTING_APPLY = 3,
};
uint32_t gtav_native_bridge_list_kind(uint32_t action);
// List-value store diagnostics. capacity() is the compile-time slot count; saturated()
// returns 1 once set_list_value() has had to drop a write (distinct cyclers > capacity) --
// a health signal that should stay 0 in a correctly-sized build.
uint32_t gtav_native_bridge_list_values_capacity(void);
int gtav_native_bridge_list_values_saturated(void);
// Menu customisation: colour theme + draw region (left/right). Setters clamp into range
// (so a value restored from /data is always valid); the render path reads them each
// frame. The *_label getters back the Theme / Draw Side cyclers and the *_count getters
// bound them.
void gtav_native_bridge_set_theme(uint32_t index);
uint32_t gtav_native_bridge_theme(void);
uint32_t gtav_native_bridge_theme_count(void);
const char* gtav_native_bridge_theme_label(uint32_t index);
// In-menu custom-theme editor. 30 RGB channels across 10 colour groups (3 each): 0-8
// accent/selection/panel, 9-29 footer/scrollbar/sel-label/toggle-on/toggle-off/list/locked.
// Stepping a channel selects the Custom slot so the recolour previews live; values clamp 0-255.
// The channels round-trip through the feature profile (v15) via get/set_custom_theme.
#define GTAV_CUSTOM_THEME_CHANNELS 30u
void gtav_native_bridge_adjust_custom_color(uint32_t channel, int dir);
uint32_t gtav_native_bridge_custom_color(uint32_t channel);
void gtav_native_bridge_reset_custom_theme(void);
void gtav_native_bridge_get_custom_theme(uint32_t* channels);  // GTAV_CUSTOM_THEME_CHANNELS values
void gtav_native_bridge_set_custom_theme(const uint32_t* channels);
// Apply only the first `count` channels (the rest keep the seeded default-palette value). Used by
// the profile import to upgrade an older file that stored fewer channels without blacking out the
// groups it never had.
void gtav_native_bridge_set_custom_theme_count(const uint32_t* channels, uint32_t count);
void gtav_native_bridge_set_region(uint32_t index);
uint32_t gtav_native_bridge_region(void);
uint32_t gtav_native_bridge_region_count(void);
const char* gtav_native_bridge_region_label(uint32_t index);
// Menu panel width (Narrow/Normal/Wide). Pure render state, set immediately like the region cycler.
void gtav_native_bridge_set_panel_width_index(uint32_t index);
uint32_t gtav_native_bridge_panel_width_index(void);
uint32_t gtav_native_bridge_panel_width_count(void);
const char* gtav_native_bridge_panel_width_label(uint32_t index);
uint32_t gtav_native_bridge_toggle_telemetry(void);

// Browser favorites/recents persistence bridge. The bridge owns the live hash lists (the
// Favorites/Recents filter categories in the vehicle/ped browsers); features.cpp copies them
// in/out of the saved profile like theme/region. The `veh`/`ped` buffers are the profile's
// fixed-capacity arrays (GTAV_FAVORITE_*_MAX / GTAV_RECENT_*_MAX hashes, 0 == empty slot).
void gtav_native_bridge_get_favorites(uint32_t* veh, uint32_t* ped);
void gtav_native_bridge_set_favorites(const uint32_t* veh, const uint32_t* ped);
void gtav_native_bridge_get_recents(uint32_t* veh, uint32_t* ped);
void gtav_native_bridge_set_recents(const uint32_t* veh, const uint32_t* ped);

// Global Quick pins persistence bridge. The bridge owns the live (action, param) pin arrays (the
// top-level Quick menu); features.cpp copies them in/out of the saved profile like the browser
// favorites. The `action`/`param` buffers are the profile's GTAV_QUICK_PINS_MAX arrays (action
// 0 == empty slot).
void gtav_native_bridge_get_quick(uint32_t* action, uint32_t* param);
void gtav_native_bridge_set_quick(const uint32_t* action, const uint32_t* param);

// Return the shell action produced by back navigation, without performing it.
uint32_t gtav_native_bridge_back_action(void);
void gtav_native_bridge_back(void);
// Collapse straight to the root menu (Circle held past the hold threshold). Pops every level in
// one gesture; no-op at the root. Returns GTAV_NATIVE_SHELL_ACTION_NONE.
uint32_t gtav_native_bridge_back_to_root(void);
void gtav_native_bridge_snapshot(GtavNativeShellSnapshot* out);
const char* gtav_native_bridge_action_name(uint32_t action);

#ifdef __cplusplus
}
#endif
