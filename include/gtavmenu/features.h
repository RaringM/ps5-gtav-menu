#pragma once

#include "gtavmenu/native_bridge.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_FEATURES_ABI_VERSION 2u
#define GTAV_FEATURE_MESSAGE_LEN 80u

enum {
  GTAV_FEATURE_RESULT_NONE = 0,
  GTAV_FEATURE_RESULT_OK = 1,
  GTAV_FEATURE_RESULT_UNAVAILABLE = 2,
  GTAV_FEATURE_RESULT_FAILED = 3,
};

typedef struct GtavFeatureState {
  uint32_t abi_version;
  uint32_t native_ready;
  uint32_t telemetry_enabled;
  uint32_t god_mode_enabled;
  uint32_t super_run_enabled;
  uint32_t fast_swim_enabled;
  uint32_t infinite_ammo_enabled;
  uint32_t activation_count;
  uint32_t last_action;
  uint32_t last_result;
  uint32_t last_param;
  uint32_t reserved;
  char last_message[GTAV_FEATURE_MESSAGE_LEN];
} GtavFeatureState;

typedef struct GtavHudReadout GtavHudReadout;
typedef struct GtavZombieReadout GtavZombieReadout;
typedef struct GtavObjectMoveReadout GtavObjectMoveReadout;

void gtav_features_init(const GtavNativeAddressTable* table);
/* Arm the main-thread frame hook independent of gameplay natives. A native-features-off
 * build (e.g. a probe/calibration lane) still needs the external-install thunk + per-frame
 * tick wired so the frame-hook context probe can run on the game thread; in feature
 * builds this does what gtav_features_init used to do at its tail. No-op (still safe to
 * call) when the build has no frame hook or skips hook setup. */
void gtav_features_init_frame_hook(void);
/* Disable active feature state and clear queued game-thread work during runtime shutdown. */
void gtav_features_shutdown(void);
/* action with no parameter (param defaults to 0). */
uint32_t gtav_features_activate(uint32_t action);
/* action carrying a row parameter. For SPAWN_VEHICLE this is a model joaat hash
 * once the request enters the feature layer; index-based callers must translate
 * before dispatch. */
uint32_t gtav_features_activate_param(uint32_t action, uint32_t param);
/* Non-blocking worker-side preload of the highlighted vehicle's model (0 = none),
 * called every worker tick so the streamer has it resident before the spawn is
 * confirmed. Never blocks; only warms the cache (CREATE_VEHICLE stays game-thread). */
void gtav_features_preload_highlighted_vehicle(uint32_t model);
/* One-shot non-blocking pre-warm of a model at spawn/skin QUEUE time (0 = no-op), so an
 * instant select starts streaming immediately instead of at the first game-thread hook
 * fire. Shares the hover-preload's single warming slot (one REQUEST_MODEL owner / one
 * release); never blocks. No-op unless built with GTAV_MENU_ENABLE_VEHICLE_PRELOAD. */
void gtav_features_prewarm_model_hash(uint32_t model);
/* Vehicle-spawner image preview (behind GTAV_MENU_ENABLE_VEHICLE_PREVIEW). The worker passes the
 * highlighted vehicle's preview target every tick: the streamed texture `dict`, the `texture` name
 * INSIDE that dict to DRAW_SPRITE, and a display `caption`. A vehicle with no verified mapping
 * passes dict == texture == "" (caption may still hold the vehicle label for the placeholder
 * card). The setter publishes the dict and -- throttled -- kicks a game-thread PREVIEW_TXD job
 * that streams it (keep-alive while shown, release on change); dict/texture are kept independent
 * because a model-named dict can load yet hold no model-named texture (the white-card bug). The
 * getters expose the current target + residency so the worker render path can DRAW_SPRITE only a
 * loaded, mapped texture (else a placeholder), making the preview layer the single source of
 * truth. All no-op / empty without the gate + frame hook; safe to call from any build (stable
 * ABI). The returned pointers are stable for the life of the process (point at internal storage);
 * read them, do not free or retain across ticks. */
void gtav_features_preview_set_target(const char* dict, const char* texture, const char* caption);
int gtav_features_preview_texture_loaded(void);
const char* gtav_features_preview_dict(void);
const char* gtav_features_preview_texture(void);
const char* gtav_features_preview_caption(void);
/* Diagnostic: the resolution (rounded px) of the current (dict, texture) per
 * GET_TEXTURE_RESOLUTION, measured game-thread once the dict is resident. 0 width => the texture is
 * NOT in the loaded dict (wrong name / no image); non-zero while the sprite still draws white =>
 * DRAW_SPRITE cannot bind the streamed texture on the worker render thread. The render path
 * overlays this on the card so the cause is visible on-screen. 0 without the gate. */
uint32_t gtav_features_preview_res_w(void);
uint32_t gtav_features_preview_res_h(void);
/* Phase-list worker publication fence. `dict` is the texture dictionary actually referenced by
 * the just-published immutable generation, or "" when it contains no preview sprite. The
 * game-thread streamer uses this to defer releasing a superseded dictionary until the phase
 * consumer can no longer be reading it. No-op outside the phase-preview build. */
void gtav_features_preview_phase_published(uint64_t generation, const char* dict);
/* Native instructional-button bar (behind GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM). The menu's
 * bottom-right controls strip can be drawn as GTA's own "instructional_buttons" Scaleform -- the
 * exact widget the pause menu uses -- so it renders real DualSense glyphs in the native format.
 * The whole Scaleform lifecycle (request + build the data slots + DRAW_SCALEFORM_MOVIE_FULLSCREEN)
 * runs on the game-thread frame-hook tick, the same thread scripts draw fullscreen scaleforms from;
 * the worker only PUBLISHES which buttons the focused row needs. Each button carries its FRONTEND
 * control id(s) + a verb label; the label must be static (stable for process life -- the worker
 * passes a pointer into its own rodata, read on the game thread without copying). All no-op without
 * the gate + frame hook; safe to call from any build (stable ABI). */
typedef struct GtavInstructionalButton {
  int control;       /* GTA control id -> the controller's real glyph (GET_CONTROL_INSTRUCTIONAL_
                      * BUTTONS_STRING, which handles the X/O face-button swap per platform). */
  int control2;      /* secondary control id for a two-glyph slot (e.g. L1+R1 shown together), or
                      * -1 for a single glyph. */
  const char* glyph; /* "~INPUT_*~" token fallback: some controls (the d-pad directions) yield no
                      * glyph string from the control id on Enhanced, so the builder pushes this
                      * token through the Scaleform text path instead. Static, stable; may be "". */
  const char* label; /* verb performed on the focused row (e.g. "Select"); static, stable. */
} GtavInstructionalButton;
/* Worker-safe: publish the focused row's ordered buttons. Cheap; only rebuilds the Scaleform data
 * slots game-thread-side when the set actually changes. count clamps to capacity. */
void gtav_features_instructional_publish(uint32_t count, const GtavInstructionalButton* buttons);
/* Non-zero once the native bar's Scaleform is built + resident (and thus drawing on the game
 * thread). The worker render path reads this to suppress its own fallback bar so the two never
 * stack. 0 without the gate, while the movie streams, or when the menu is closed. */
int gtav_features_instructional_active(void);
/* Phase-callback final submission. Scaleform request/loading and data-slot construction remain on
 * the game-thread hook; this bounded function performs only DRAW_SCALEFORM_MOVIE_FULLSCREEN after
 * claiming a prepared handle. Returns one when submitted. No-op without the phase-list gate. */
int gtav_features_instructional_phase_draw(void);
/* Worker-safe: non-zero while the Free Camera mode is flying (the menu is hidden then). The render
 * layer reads it to drive the native instructional bar + the free-cam HUD. 0 without the gate. */
int gtav_features_free_cam_active(void);
/* Diagnostic snapshot of the Route B Scaleform state (cached, worker-safe): handle =
 * REQUEST_SCALEFORM_MOVIE result, loaded = HAS_SCALEFORM_MOVIE_LOADED, ready = scaleform natives
 * resolved, open = menu-open seen game-thread, draws = DRAW_SCALEFORM_MOVIE_FULLSCREEN count. Each
 * is -1 without the gate. Used to surface why the native bar is/ isn't drawing while tuning on
 * hardware. Any out pointer may be NULL. */
void gtav_features_instructional_debug(int* handle, int* loaded, int* ready, int* open,
                                       uint32_t* draws);
void gtav_features_run_spawn_hash_job(uint32_t action, uint32_t model, void* ctx);
/* Per-tick worker re-apply pass. `menu_visible` is non-zero while the menu panel is open
 * so worker-tick effects that consume gameplay input (noclip) can stand down and not steal
 * it from menu navigation. */
void gtav_features_worker_tick(int menu_visible);
/* Set the menu-open mirror immediately on a visibility transition (called from
 * gtav_menu_set_visible) so the game-thread input-suppression predicate flips on the same
 * frame, not a worker tick later. The per-tick worker pass keeps it in sync afterwards. */
void gtav_features_set_menu_open(int menu_open);
/* Worker tells the game-thread wardrobe-cam driver whether the Wardrobe submenu is open (frame the
 * full player in a scripted cam) or not (tear the cam down). */
void gtav_features_set_wardrobe_cam_active(int active);
void gtav_features_snapshot(GtavFeatureState* out);
/* Bitmask of feature toggle states for menu display (see GTAV_FEATURE_TOGGLE_*). */
uint64_t gtav_features_toggle_mask(void);
/* Current display value for a list-cycler action (e.g. weather/time), for the menu
 * to render "< VALUE >". Empty string for non-list actions. */
const char* gtav_features_value_label(uint32_t action);
/* GTA V vehicle class (0..21) for catalog entry `index`, or -1 if unknown/unavailable.
 * Cached after the first call. Lets the menu group/filter spawn rows by class. */
int gtav_features_vehicle_class(uint32_t index);
const char* gtav_feature_action_name(uint32_t action);
const char* gtav_feature_result_name(uint32_t result);
/* Non-zero if the action is gated off this build (needs the main-thread hook).
 * The menu uses this to render the row as locked/unavailable. */
int gtav_features_action_is_gated(uint32_t action);
/* 1 when a SAVE_VEHICLE / SAVE_OUTFIT would overwrite an occupied slot (menu arms a confirm). */
int gtav_features_save_slot_occupied(uint32_t action);
/* Non-zero if the action must run on the game/script thread (entity creation,
 * weapon manager). Build-flag independent; identifies the actions that are
 * marshalled onto the game thread (via the frame-hook job queue) rather than run
 * worker-direct. */
int gtav_features_action_needs_game_thread(uint32_t action);

/* Fill `out` with the current HUD readout (speed/coords/heading + element flags).
 * Returns non-zero if the HUD master toggle is on (i.e. the overlay should draw).
 * Safe to call every render tick; values are 0 when their getter natives are absent. */
int gtav_features_hud_readout(GtavHudReadout* out);

/* Fill `out` with the Zombie Outbreak survival readout (wave/kills/remaining + phase).
 * Returns non-zero on success; out->active is 1 only while Zombie Outbreak is the active
 * minigame, which is the survival HUD's draw gate. Safe to call every render tick (plain
 * reads of the game-thread-owned survival state -- no native call). */
int gtav_features_zombie_readout(GtavZombieReadout* out);

/* Fill `out` with the interactive object-move readout (latched object pose + control state).
 * Returns non-zero; out->active is 1 only while the move mode is engaged and an object is
 * acquired, which is the placement HUD's draw gate. Safe to call every render tick (plain
 * reads of the game-thread-owned latch -- no native call). */
int gtav_features_object_move_readout(GtavObjectMoveReadout* out);

/* Consume a pending menu-visibility request raised by the interactive object-move driver
 * (and its worker enter path): returns 0 (none), 1 (hide the menu) or 2 (show it), and
 * clears the request. The game-thread driver cannot call gtav_menu_set_visible itself (it
 * does worker-thread-only logging/status writes), so the worker applies the request from
 * gtav_menu_worker_tick. No-op (returns 0) in a build without native features. */
int gtav_features_take_menu_visibility_request(void);

/* Explosive/fire-ammo diagnostic counters for the Debug page (any out-param may be
 * NULL). They make the otherwise-silent failure observable: tick_fired counts game-thread
 * tick runs with an ammo effect on, impact_hit counts fresh weapon impacts read, and
 * effect_requested counts explosion/fire natives actually called. Stay 0 in builds
 * without the frame hook. See src/module/features/weapon_fx.inc. */
void gtav_features_weapon_fx_stats(uint32_t* tick_fired, uint32_t* impact_hit,
                                   uint32_t* effect_requested);

/* Read-only diagnostics snapshot for the debug overlay (TOGGLE_DEBUG_OVERLAY). Surfaces
 * the live internals an educational menu wants to show: frame-hook health, job-queue
 * throughput, resolved-native count, and the weapon-fx counters above. Filled by
 * gtav_features_debug_stats(); fields are 0 in builds without the frame hook. */
/* Max script-global slots sampled into the debug overlay's "global watch" readout. */
#define GTAV_DEBUG_GLOBAL_WATCH_MAX 4u

typedef struct GtavDebugStats {
  uint32_t overlay_enabled;
  uint32_t hook_active;
  uint32_t hook_calls;
  uint32_t jobs_run;
  uint32_t jobs_dropped;
  uint32_t native_count;
  uint32_t native_ready;
  uint32_t wfx_tick_fired;
  uint32_t wfx_impact_hit;
  uint32_t wfx_effect_requested;
  float wfx_last_x; /* last impact coord read by the ammo tick (debug bisection) */
  float wfx_last_y;
  float wfx_last_z;
  uint32_t gun_tick_fired;     /* gun toolkit: tick ran with a gun mode active */
  uint32_t gun_fire_detected;  /* gun toolkit: Fire (R2) recognized this frame */
  uint32_t gun_raycast_hit;    /* gun toolkit: aim shape test returned a hit */
  uint32_t gun_effect_applied; /* gun toolkit: an effect native was invoked */
  float gun_last_x;            /* last gun raycast hit coord (debug bisection) */
  float gun_last_y;
  float gun_last_z;
  uint32_t last_action;
  uint32_t last_result;
  /* Script-global watch: live values of the globals the subsystem cares about (despawn /
   * prologue / recording-UI indices), read read-only via the script-globals base. Empty
   * (sg_watch_count == 0) in any build without GTAV_MENU_ENABLE_SCRIPT_GLOBALS or with the
   * base anchor unset. sg_watch_valid[i] is 0 when the slot is not resolvable yet (block
   * pool not up); sg_watch_value[i] is then meaningless. */
  uint32_t sg_watch_count;
  uint32_t sg_watch_index[GTAV_DEBUG_GLOBAL_WATCH_MAX];
  int32_t sg_watch_value[GTAV_DEBUG_GLOBAL_WATCH_MAX];
  uint8_t sg_watch_valid[GTAV_DEBUG_GLOBAL_WATCH_MAX];
} GtavDebugStats;

/* Fill `out` with the diagnostics snapshot. Returns non-zero if the debug overlay toggle
 * is on (i.e. the overlay should draw). Safe to call every render tick. */
int gtav_features_debug_stats(GtavDebugStats* out);

/* Settings persistence: capture the current toggle/cycler state into a profile and restore it
 * (re-applying live state). The loader mounts the profile storage directory into GTA's sandbox;
 * feature_profile.c performs ordinary INI IO there. Failures are non-fatal. */
struct GtavFeatureProfile; /* defined in feature_profile.h */
void gtav_features_export_profile(struct GtavFeatureProfile* out);
void gtav_features_import_profile(const struct GtavFeatureProfile* in);
/* Save/load the default profile path (/data/GTAVMenu/state/profile.cfg). Return 0 on
 * success, non-zero if the file could not be written/read. */
int gtav_features_profile_save_default(void);
int gtav_features_profile_load_default(void);

enum {
  GTAV_FEATURE_TOGGLE_GOD_MODE = 1u << 0,
  GTAV_FEATURE_TOGGLE_SUPER_RUN = 1u << 1,
  GTAV_FEATURE_TOGGLE_FAST_SWIM = 1u << 2,
  GTAV_FEATURE_TOGGLE_INFINITE_AMMO = 1u << 3,
  GTAV_FEATURE_TOGGLE_SUPER_JUMP = 1u << 4,
  GTAV_FEATURE_TOGGLE_INVISIBLE = 1u << 5,
  GTAV_FEATURE_TOGGLE_NEVER_WANTED = 1u << 6,
  GTAV_FEATURE_TOGGLE_VEHICLE_GOD = 1u << 7,
  GTAV_FEATURE_TOGGLE_NITRO = 1u << 8,
  GTAV_FEATURE_TOGGLE_ENGINE_ON = 1u << 9,
  GTAV_FEATURE_TOGGLE_FAST_MOVE = 1u << 10,
  GTAV_FEATURE_TOGGLE_NO_RAGDOLL = 1u << 11,
  GTAV_FEATURE_TOGGLE_SEATBELT = 1u << 12,
  GTAV_FEATURE_TOGGLE_KEEP_CLEAN = 1u << 13,
  GTAV_FEATURE_TOGGLE_SUPER_PROOFS = 1u << 14,
  GTAV_FEATURE_TOGGLE_NO_CRIT = 1u << 15,
  GTAV_FEATURE_TOGGLE_INF_STAMINA = 1u << 16,
  GTAV_FEATURE_TOGGLE_INF_SPECIAL = 1u << 17,
  GTAV_FEATURE_TOGGLE_LOW_GRAVITY = 1u << 18,
  GTAV_FEATURE_TOGGLE_SLOW_MOTION = 1u << 19,
  GTAV_FEATURE_TOGGLE_FREEZE_TIME = 1u << 20,
  GTAV_FEATURE_TOGGLE_AUTO_HEAL = 1u << 21,
  GTAV_FEATURE_TOGGLE_WANTED_LOCK = 1u << 22,
  GTAV_FEATURE_TOGGLE_KEEP_REPAIRED = 1u << 23,
  GTAV_FEATURE_TOGGLE_HUD = 1u << 24,
  GTAV_FEATURE_TOGGLE_HUD_SPEEDO = 1u << 25,
  GTAV_FEATURE_TOGGLE_HUD_COORDS = 1u << 26,
  GTAV_FEATURE_TOGGLE_HUD_FPS = 1u << 27,
  GTAV_FEATURE_TOGGLE_HUD_DISTANCE = 1u << 28,
  GTAV_FEATURE_TOGGLE_STICK_TO_GROUND = 1u << 29,
  GTAV_FEATURE_TOGGLE_DRIFT_MODE = 1u << 30,
  GTAV_FEATURE_TOGGLE_XENON =
      1u << 31,  // xenon headlights (LSC; reuses TOGGLE_VEHICLE_MOD slot 22)
};

/* Toggle bits 32+ live in the uint64_t mask (gtav_features_toggle_mask), but the enum
 * above is int-typed in C (its top bit, 1u<<31, is already the last value an int-range
 * enumerator can hold), so these high bits are 64-bit macros rather than enum members.
 * Always shift a 64-bit literal -- `1u << 32` is undefined behaviour. */
#define GTAV_FEATURE_TOGGLE_RAINBOW_NEON (1ull << 32)   /* LSC: per-tick neon hue cycle */
#define GTAV_FEATURE_TOGGLE_EXPLOSIVE_AMMO (1ull << 33) /* explode at each weapon impact */
#define GTAV_FEATURE_TOGGLE_FIRE_AMMO (1ull << 34)      /* start a fire at each weapon impact */
#define GTAV_FEATURE_TOGGLE_RAINBOW_PAINT (1ull << 35)  /* LSC: per-tick body paint hue cycle */
#define GTAV_FEATURE_TOGGLE_NOCLIP (1ull << 36)         /* free-move the frozen player entity */
#define GTAV_FEATURE_TOGGLE_DEBUG_OVERLAY (1ull << 37)  /* read-only diagnostics overlay */
#define GTAV_FEATURE_TOGGLE_SUPER_BRAKE (1ull << 38)    /* hard brake: bleed speed while braking */
#define GTAV_FEATURE_TOGGLE_NIGHT_VISION (1ull << 39)   /* SET_NIGHTVISION (game-thread tick) */
#define GTAV_FEATURE_TOGGLE_SEETHROUGH (1ull << 40)     /* SET_SEETHROUGH thermal (game-thread) */
/* Minigame "fun modes" -- mutually exclusive, so at most one of these bits is ever set.
 * Each maps to GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_<NAME>; the per-frame work runs on
 * the game thread (frame-hook tick). Phase B (Blackout..Flood) stays off until its native
 * is pinned. */
#define GTAV_FEATURE_TOGGLE_MINIGAME_RIOT (1ull << 41)     /* nearby peds hostile + swarm */
#define GTAV_FEATURE_TOGGLE_MINIGAME_METEOR (1ull << 42)   /* explosions rain on the player */
#define GTAV_FEATURE_TOGGLE_MINIGAME_STORM (1ull << 43)    /* thunderstorm + midnight */
#define GTAV_FEATURE_TOGGLE_MINIGAME_INFERNO (1ull << 44)  /* fires ignite around you */
#define GTAV_FEATURE_TOGGLE_MINIGAME_RAGDOLL (1ull << 45)  /* nearby peds keep ragdolling */
#define GTAV_FEATURE_TOGGLE_MINIGAME_BLACKOUT (1ull << 46) /* Phase B: city lights off */
#define GTAV_FEATURE_TOGGLE_MINIGAME_ZOMBIES (1ull << 47)  /* Phase B: hostile horde */
#define GTAV_FEATURE_TOGGLE_MINIGAME_FLOOD (1ull << 48)    /* Phase B: experimental water */

/* Menyoo-gap feature wave 1 toggle bits (uint64_t mask, continuing past the minigames). */
#define GTAV_FEATURE_TOGGLE_INFINITE_PARACHUTES \
  (1ull << 49)                                       /* re-grant GADGET_PARACHUTE (game-thread) */
#define GTAV_FEATURE_TOGGLE_SPAWN_MAXED (1ull << 50) /* new vehicles spawn fully modded */
#define GTAV_FEATURE_TOGGLE_SPAWN_INVINCIBLE (1ull << 51) /* new vehicles spawn indestructible */
#define GTAV_FEATURE_TOGGLE_THIN_POPULATION \
  (1ull << 52) /* SET_*_POPULATION_BUDGET 0 (game-thread) */
#define GTAV_FEATURE_TOGGLE_BODYGUARD_INVINCIBLE \
  (1ull << 53) /* spawned bodyguards are made invincible/reasserted */
#define GTAV_FEATURE_TOGGLE_CRUISE_CONTROL \
  (1ull << 54) /* hold the current vehicle at the selected cruise speed */
/* Menyoo-gap feature wave 2. */
#define GTAV_FEATURE_TOGGLE_IGNORED_BY_ALL \
  (1ull << 55) /* police + peds + gangs ignore the player (game-thread re-assert) */
#define GTAV_FEATURE_TOGGLE_BODYGUARD_BLIPS \
  (1ull << 56) /* tag tracked bodyguards on the minimap (game-thread blip tick) */
#define GTAV_FEATURE_TOGGLE_AUTO_EDIT_OBJECT \
  (1ull << 57) /* auto-enter interactive object move mode after a spawn */
/* Fun + parity wave. */
#define GTAV_FEATURE_TOGGLE_NO_RELOAD \
  (1ull << 58) /* SET_PED_INFINITE_AMMO_CLIP, re-asserted on the game-thread tick */
#define GTAV_FEATURE_TOGGLE_SLIPPERY \
  (1ull << 59) /* constant SET_VEHICLE_REDUCE_GRIP (ice physics), game-thread vehicle tick */
#define GTAV_FEATURE_TOGGLE_EXPLOSIVE_MELEE \
  (1ull << 60) /* spawn a small explosion at each melee impact (game-thread tick) */
#define GTAV_FEATURE_TOGGLE_EMOTE_LOOP \
  (1ull << 61) /* loop the emote until Stop, vs play once (default off) */

/* Live HUD readout, filled by gtav_features_hud_readout() from worker-safe getter
 * natives (GET_ENTITY_SPEED/COORDS/HEADING). The bridge render path consumes this to
 * draw the overlay; element flags mirror the GTAV_FEATURE_TOGGLE_HUD_* toggles. */
typedef struct GtavHudReadout {
  uint32_t hud_enabled;
  uint32_t show_speedo;
  uint32_t show_coords;
  uint32_t show_fps;
  uint32_t show_distance;
  uint32_t in_vehicle;
  uint32_t has_waypoint;
  float speed_kmh;
  float x;
  float y;
  float z;
  float heading;
  float distance_m;
} GtavHudReadout;

/* Zombie Outbreak survival phase (GtavZombieReadout.state). Shared between the game-thread
 * driver (minigame.inc) and the worker render path so the survival HUD can show the right
 * banner without duplicating the enum. Append-only. */
enum {
  GTAV_ZOMBIE_STATE_IDLE = 0,
  GTAV_ZOMBIE_STATE_SPAWNING = 1,     /* emitting this wave's peds */
  GTAV_ZOMBIE_STATE_FIGHTING = 2,     /* wave fully spawned; clearing the horde */
  GTAV_ZOMBIE_STATE_INTERMISSION = 3, /* breather before the next wave */
  GTAV_ZOMBIE_STATE_GAMEOVER = 4      /* player died; banner held, then mode auto-stops */
};

/* Live Zombie Outbreak survival readout, filled by gtav_features_zombie_readout() from the
 * game-thread-owned survival state (plain reads, no native call). The bridge render path draws
 * a HUD from this while `active` is set; `state` is the GTAV_ZOMBIE_STATE_* phase so the overlay
 * can show wave / cleared / game-over banners. */
typedef struct GtavZombieReadout {
  uint32_t active;    /* 1 while Zombie Outbreak is the active minigame (HUD draw gate) */
  uint32_t wave;      /* current wave number (1..N) */
  uint32_t kills;     /* confirmed kills this session */
  uint32_t remaining; /* zombies left this wave (alive + not yet spawned) */
  uint32_t state;     /* GTAV_ZOMBIE_STATE_* survival phase */
} GtavZombieReadout;

/* Live interactive object-move readout, filled by gtav_features_object_move_readout() from the
 * game-thread-owned latch (plain reads, no native call). The bridge render path draws a
 * placement HUD (control hints + the live pose) while `active` is set. Rotation fields are the
 * latched euler angles in degrees; pitch/roll are meaningful only when the 3-axis rotation
 * natives resolved (else they stay 0 and only heading is driven). */
typedef struct GtavObjectMoveReadout {
  uint32_t active;      /* 1 while move mode is engaged and an object is acquired (HUD gate) */
  uint32_t can_rotate3; /* 1 if SET/GET_ENTITY_ROTATION resolved (pitch/roll available) */
  uint32_t coarse;      /* 1 while the coarse-speed modifier (Sprint) is held */
  float x;              /* latched object position */
  float y;
  float z;
  float heading;        /* latched yaw (degrees) */
  float pitch;          /* latched pitch (degrees; 0 unless can_rotate3) */
  float roll;           /* latched roll (degrees; 0 unless can_rotate3) */
  uint32_t aim_mode;    /* 1 = aim-follow sub-mode (prop tracks the camera) */
  uint32_t aim_avail;   /* 1 if the aim native (GET_GAMEPLAY_CAM_COORD) resolved */
  uint32_t snap_mode;   /* 1 = surface raycast snap on (aim sub-layer) */
  uint32_t snap_avail;  /* 1 if the shape-test natives resolved */
  uint32_t ground_lock; /* 1 = free-move continuous ground-lock on (Z follows terrain) */
  float aim_dist;       /* current aim-follow distance (metres) */
  uint32_t sel_index;   /* 1-based edit-target position in the live roster (0 if none) */
  uint32_t sel_count;   /* number of live menu-spawned objects */
} GtavObjectMoveReadout;

#ifdef __cplusplus
}
#endif
