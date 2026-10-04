#include "gtavmenu/features.h"

#include "gtavmenu/abi.h"
#include "gtavmenu/custom_assets.h"
#include "gtavmenu/feature_catalog.h"
#include "gtavmenu/feature_profile.h"
#include "gtavmenu/log.h"
#include "gtavmenu/menu_draw_list.h"
#include "gtavmenu/pad_input.h"
#include "gtavmenu/rootdir.h"
#include "gtavmenu/script_globals.h"
#include "gtavmenu/status.h"
#include "gtavmenu/strutil.h"
#include "gtavmenu/tls_layout.h"

#ifndef GTAV_MENU_ENABLE_FRAME_HOOK
#define GTAV_MENU_ENABLE_FRAME_HOOK 0
#endif
// Native gameplay features re-assert the held toggles (god mode, vehicle toggles, movement
// multipliers, ...) every tick, and those natives walk LIVE engine entities -- only safe on
// the game/script thread, reached via the frame hook (apply_held_toggles ->
// reassert_toggles_game_thread). Without the frame hook the only place left to run them is the
// scePad worker, the documented intermittent-SIGSEGV lane (see the gtav_features_worker_tick
// fallback below). Make the *accidental* native-features-without-hook build a hard error rather
// than a console crash; set GTAV_MENU_ALLOW_WORKER_REASSERT=1 to opt into the worker lane
// deliberately (legacy diagnostics only).
#ifndef GTAV_MENU_ALLOW_WORKER_REASSERT
#define GTAV_MENU_ALLOW_WORKER_REASSERT 0
#endif
#if GTAV_MENU_ENABLE_NATIVE_FEATURES && !GTAV_MENU_ENABLE_FRAME_HOOK && \
    !GTAV_MENU_ALLOW_WORKER_REASSERT
#error \
    "Native gameplay features need the game-thread frame hook (GTAV_MENU_ENABLE_FRAME_HOOK): " \
    "without it the held-toggle re-assertions run on the scePad worker (apply_held_toggles), " \
    "the documented intermittent-SIGSEGV lane. Build the feature-menu frame-hook target, or set " \
    "GTAV_MENU_ALLOW_WORKER_REASSERT=1 to opt into the worker lane deliberately."
#endif
#ifndef GTAV_FEATURES_NO_LIBC_FORMAT
#define GTAV_FEATURES_NO_LIBC_FORMAT 0
#endif
#ifndef GTAV_FEATURES_SKIP_PROFILE_LOAD
#define GTAV_FEATURES_SKIP_PROFILE_LOAD 0
#endif
#ifndef GTAV_FEATURES_SKIP_FRAME_HOOK_SETUP
#define GTAV_FEATURES_SKIP_FRAME_HOOK_SETUP 0
#endif
#if GTAV_MENU_ENABLE_FRAME_HOOK
#include "gtavmenu/frame_hook.h"
#include "gtavmenu/patch_broker.h"
#include "gtavmenu/script_handler_census.h"
// Per-frame native handler to trampoline for main-thread execution. Default is
// GET_VEHICLE_PED_IS_IN (0x18c8060), whose register-save prologue is
// trampoline-safe (see the gtav-mainthread-native-execution note).
#ifndef GTAV_MENU_FRAME_HOOK_TARGET
#define GTAV_MENU_FRAME_HOOK_TARGET 0x18c8060ull
#endif
// External install: the .text patch is delivered by ps5debug's kernel write, not
// in-process. Off by default (in-process detour).
#ifndef GTAV_FRAME_HOOK_EXTERNAL_INSTALL
#define GTAV_FRAME_HOOK_EXTERNAL_INSTALL 0
#endif
#ifndef GTAV_MENU_HANDLER_SLOT_JOB_DRAIN
#define GTAV_MENU_HANDLER_SLOT_JOB_DRAIN 0
#endif
#ifndef GTAV_MENU_DEFAULT_TARGET_ID
#define GTAV_MENU_DEFAULT_TARGET_ID "GTAV_PS5_SAFE_PROBE"
#endif
#endif

// Declared in gtavmenu/menu.h; the frame-hook game-thread tick calls back so the
// hook-side telemetry counter (hookSideTicks) and the native-bridge canary update.
extern "C" void gtav_menu_frame_tick(void);

// Warm the highlighted vehicle's model on the worker thread so the streamer has it
// resident before the user confirms the spawn, collapsing the model-streaming
// requeue delay (and the "model not loaded" timeouts while driving). Only
// CREATE_VEHICLE stays game-thread-only; REQUEST_MODEL is issued with the same plain
// worker-thread invoke the streaming-aware teleport tick already uses on hardware
// (NOT the fs:-GTAV_TLS_GAME_CTX_OFFSET borrowed-context wrapper, which has never been
// exercised and is unreachable in any shipping build).
//
// DEFAULT OFF: retained gate for the old hover-preload experiment. Production cold-model
// streaming runs through the game-thread PREWARM_MODEL/spawn job path; it does not need an
// off-thread REQUEST_MODEL call.
#ifndef GTAV_MENU_ENABLE_VEHICLE_PRELOAD
#define GTAV_MENU_ENABLE_VEHICLE_PRELOAD 0
#endif

// Script-resource owner census + owner gate for the model-streaming lane.
//
// Offline RE of the 01.010.002 eboot showed model streaming is SCRIPT-scoped, not frame-scoped:
// REQUEST_MODEL (0x1c09550) tail-jumps to a shared helper (0x1c07fb0) that registers the model as
// a type-0xE script resource on the CALLING script's CGameScriptHandler ([scrThread + 0x198],
// vtable slot 13), and SET_MODEL_AS_NO_LONGER_NEEDED (0x1c09760) releases it from that same
// handler (slot 15). Our hook chains PLAYER_PED_ID -- a native every script calls -- so an
// ungated request is owned by whichever transient script ran that fire and is reclaimed when that
// script ends. This explained the old worker/pre-owner failures. The current explicit spawn path
// keeps streaming on the game-thread job and has spawned a cold model on 01.010.002; the census and
// filter below remain research-only alternatives.
//
// CENSUS (read-only) watches which handler owns each fire; FILTER makes the streaming jobs wait
// for a fire owned by a long-lived script. Both default off. See
// include/gtavmenu/script_handler_census.h.
#ifndef GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_CENSUS
#define GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_CENSUS 0
#endif
#ifndef GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_FILTER
#define GTAV_MENU_ENABLE_MODEL_STREAM_OWNER_FILTER 0
#endif

// Issue streaming requests through REQUEST_MENU_PED_MODEL (0x1c09560) instead of REQUEST_MODEL.
// It is the same native plus two extra streaming flags before the identical shared helper, so it
// is a one-line A/B on whether the flags -- rather than resource ownership -- are what a cold
// model was missing. Default off.
#ifndef GTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST
#define GTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST 0
#endif

// Force the streamer to SERVICE its queued requests, via LOAD_ALL_OBJECTS_NOW (0x1c092f0).
//
// A read-only probe of the live streamer (2026-09-18, pid 626) showed the real stage this lane was
// failing at. Our requests ARE recorded: a cold model sits in strStreamingInfo at status 2
// (requested) carrying our exact flags 0x216, while a resident model reads status 1 (loaded) --
// and it stays at status 2 indefinitely, so nothing is dropping it either. That falsified both
// earlier theories at once (the request is neither mis-issued nor reclaimed); what never happens
// is the streamer finishing the load. LOAD_ALL_OBJECTS_NOW is the native scripts use to flush
// every already-requested object synchronously.
//
// RESULT: this DEADLOCKS GTA and must not be enabled on the frame-hook lane (hardware
// 2026-09-18, pid 639: one call froze the game thread with g_draining == 1, requests == 1 and
// force_loads == 0 -- the counter increments after the call, so it never returned). The native
// blocks until the streamer drains its queue, but the streamer only advances while the main loop
// pumps it, and we run inside a chained native on that very thread mid-frame. It is therefore
// unfixable by throttling: a single call cannot return. Left compiled-but-off as a signpost.
#ifndef GTAV_MENU_ENABLE_STREAM_FORCE_LOAD
#define GTAV_MENU_ENABLE_STREAM_FORCE_LOAD 0
#endif

// Only spawn ALREADY-RESIDENT models; never request a cold one. Defaults to 1 -- see
// make/config.mk and docs/ptrace-free-injection.md: a single cold request freezes the streamer's
// I/O ring for the rest of the session and starves the game's own streaming. This is the one
// streaming gate whose SAFE value is on.
#ifndef GTAV_MENU_STREAM_RESIDENT_ONLY
#define GTAV_MENU_STREAM_RESIDENT_ONLY 1
#endif

// Vehicle-spawner image preview (default off; built via GTAV_MENU_ENABLE_VEHICLE_PREVIEW=1).
// Streams the highlighted vehicle's website thumbnail on the game-thread frame-hook drain. The
// renderer submits DRAW_SPRITE directly on the legacy lane or through the immutable phase list.
#ifndef GTAV_MENU_ENABLE_VEHICLE_PREVIEW
#define GTAV_MENU_ENABLE_VEHICLE_PREVIEW 0
#endif

// Experimental engine device mount of the custom root as gtavmenu:/ (features/custom_device.inc).
#ifndef GTAV_MENU_ENABLE_CUSTOM_DEVICE
#define GTAV_MENU_ENABLE_CUSTOM_DEVICE 0
#endif

#include "gtavmenu/native_invoke.hpp"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Feature addresses are pinned to the reproducible offline resolution
// (native name -> public joaat -> crossmap -> handler). They are only compiled
// in when native features are enabled for this build; default builds keep the
// table empty so every feature reports "unavailable" and no address is embedded.
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
// Per-target native-address table. The build passes the exact header for the
// selected GTA build; defaulting to the original generated header keeps existing
// flows and the 01.005.000 validated build working.
#ifndef GTAV_MENU_NATIVE_ADDRESSES_HEADER
#define GTAV_MENU_NATIVE_ADDRESSES_HEADER \
  "gtavmenu/native_addresses_ppsa04264-01.010.002_generated.h"
#endif
#include GTAV_MENU_NATIVE_ADDRESSES_HEADER
#endif

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Per-frame re-assertion of the held toggles (god mode, vehicle toggles, multipliers, ...),
// run on the game/script thread from gtav_features_game_thread_tick() (weapon_fx.inc, composed
// inside the anonymous namespace below). Declared here in the GLOBAL namespace -- matching its
// definition further down -- so the in-namespace call site binds to that one entity. An
// anonymous-namespace forward declaration would instead bind to a distinct, never-defined
// symbol (the global definition would then read as unused).
static void reassert_toggles_game_thread(void);
#endif

namespace {

// Self-contained feature native table. Independent of the loader's core
// GtavNativeAddressTable (render/input) so adding gameplay natives does not
// churn the loader init block, runtime config, or the manifest tests.
struct FeatureNatives {
#define GTAV_FEATURE_NATIVE(field, suffix) uint64_t field;
#include "gtavmenu/feature_natives.def"
#undef GTAV_FEATURE_NATIVE
  // Build-gated natives: declared unconditionally so the struct layout stays build-flag
  // independent, but assigned only under their build gate in load_feature_natives() (left 0
  // by the memset otherwise). See feature_natives.def.
  // vehicle preview (streamed texture dict; game-thread streaming, worker draws the sprite).
  uint64_t request_streamed_texture_dict;
  uint64_t has_streamed_texture_dict_loaded;
  uint64_t set_streamed_texture_dict_as_no_longer_needed;
  uint64_t get_texture_resolution;  // GET_TEXTURE_RESOLUTION(dict, texture) diagnostic getter
  // REQUEST_MENU_PED_MODEL: REQUEST_MODEL + two extra streaming flags, bound only under
  // GTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST.
  uint64_t request_menu_ped_model;
  // LOAD_ALL_OBJECTS_NOW: synchronous flush of already-requested streaming objects, bound only
  // under GTAV_MENU_ENABLE_STREAM_FORCE_LOAD.
  uint64_t load_all_objects_now;
  // skip-prologue: TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, bound only under ENABLE_PROLOGUE_SKIP.
  uint64_t terminate_all_scripts_with_this_name;
  // Native instructional-button bar (the "instructional_buttons" Scaleform), bound only under
  // GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM. The whole bar lives + draws on the game thread, so
  // these all sit in the feature table (never the worker render lane). See
  // instructional_buttons.inc.
  uint64_t request_scaleform_movie;
  uint64_t has_scaleform_movie_loaded;
  uint64_t begin_scaleform_movie_method;
  uint64_t end_scaleform_movie_method;
  uint64_t scaleform_movie_method_add_param_int;
  uint64_t scaleform_movie_method_add_param_player_name_string;  // pushes the glyph string param
  uint64_t begin_text_command_scaleform_string;
  uint64_t end_text_command_scaleform_string;
  uint64_t
      add_text_component_substring_player_name;  // shared text component (scaleform string body)
  uint64_t draw_scaleform_movie_fullscreen;
  uint64_t set_scaleform_movie_as_no_longer_needed;
  uint64_t get_control_instructional_buttons_string;  // control id -> platform-correct glyph string
};

// Nitro-on-horn tuning. INPUT_VEH_HORN is GTA V control id 86. The boost nudges
// forward speed up by STEP each worker tick (90 Hz) and clamps to MAX (~252 km/h),
// so it ramps in ~0.2s then holds at the cap instead of accelerating without bound.
static const int GTAV_NITRO_HORN_CONTROL = 86;

// joaat("gadget_parachute") -- the parachute gadget hash given to the player by Infinite
// Parachutes. A weapon-hash argument to GIVE_WEAPON_TO_PED, not a native handler, so it stays a
// plain constant here (host-verified in test_stat_hashes.py).
static const uint32_t GTAV_GADGET_PARACHUTE_HASH = 0xFBAB5776u;

// Super-brake tuning. INPUT_VEH_BRAKE is GTA V control id 72. While the brake is held the
// worker tick drives forward speed toward a fraction of its current value each tick, so the
// car sheds speed hard but smoothly instead of snapping to a dead stop.
static const int GTAV_SUPER_BRAKE_CONTROL = 72;
static const float GTAV_SUPER_BRAKE_RETAIN = 0.30f;  // keep 30% of speed per tick while braking
// Per-frame forward displacement (metres) below which the car is treated as stopped/reversing
// rather than braking-forward, so the bleed stands down and lets reverse build. ~0.15 m/s at
// 60 fps -- small enough not to mask real forward braking, large enough to ignore jitter.
static const float GTAV_SUPER_BRAKE_FWD_EPS = 0.0025f;

struct NitroProfile {
  char label[16];  // inline (no relocation; see feature_catalog.h)
  float step_mps;
  float max_mps;
};

static const NitroProfile kNitroProfiles[] = {
    {"LOW", 3.0f, 50.0f},
    {"MED", 5.0f, 70.0f},
    {"HIGH", 8.0f, 90.0f},
};

static FeatureNatives g_n;
static GtavFeatureState g_state;
static int g_last_spawned_vehicle;
// Most-recently-spawned object handle (the interactive move mode's edit target). Like
// g_last_spawned_vehicle this can go stale, so validate with roster_handle_alive before use.
static int g_last_spawned_object;
// New toggle/cycle state kept OUT of GtavFeatureState so the status-block ABI the
// client decodes stays unchanged; surfaced only via gtav_features_toggle_mask().
// Every default is stated explicitly here and re-applied authoritatively in
// gtav_features_init(); keep the two in sync when adding state.
static int g_super_jump_enabled = 0;
static int g_invisible_enabled = 0;
static int g_never_wanted_enabled = 0;
static int g_vehicle_god_enabled = 0;
static int g_nitro_enabled = 0;
static int g_engine_on_enabled = 0;
static int g_fast_move_enabled = 0;
static int g_no_ragdoll_enabled = 0;
static int g_seatbelt_enabled = 0;
static int g_keep_clean_enabled = 0;
static int g_super_proofs_enabled = 0;
static int g_no_crit_enabled = 0;
static int g_inf_stamina_enabled = 0;
static int g_inf_special_enabled = 0;
static int g_low_gravity_enabled = 0;
static int g_slow_motion_enabled = 0;
static int g_freeze_time_enabled = 0;
static int g_saved_location_valid = 0;
static int g_auto_heal_enabled = 0;
static int g_wanted_lock_enabled = 0;
static int g_keep_repaired_enabled = 0;
static int g_stick_to_ground_enabled = 0;
static int g_drift_mode_enabled = 0;
// Slippery Roads: constant reduced grip (ice physics), distinct from Drift's speed-gated grip.
static int g_slippery_enabled = 0;
static int g_super_brake_enabled = 0;
// Super Brake direction tracking. The brake control (72) is ALSO reverse, and GET_ENTITY_SPEED
// is unsigned, so the bleed must only fire while the car is actually moving forward (otherwise it
// force-sets a positive forward speed every frame and the car can never reverse). We recover the
// signed travel direction from the per-frame position delta projected onto the car's facing.
static int g_super_brake_have_prev = 0;
static float g_super_brake_prev_x = 0.0f;
static float g_super_brake_prev_y = 0.0f;
static int g_cruise_control_enabled = 0;
static uint32_t g_cruise_speed_index = 2u;  // index into kCruiseSpeeds; default 60 km/h
static int g_night_vision_enabled = 0;
static int g_seethrough_enabled = 0;
// Menyoo-gap wave 1 toggles. Infinite Parachutes re-grants GADGET_PARACHUTE on the game-thread
// tick; Spawn Maxed/Invincible are pure display flags read at vehicle create-time; Thin
// Population re-asserts the population budgets to 0 on the game-thread tick.
static int g_inf_parachute_enabled = 0;
static int g_spawn_maxed_enabled = 0;
static int g_spawn_invincible_enabled = 0;
static int g_thin_population_enabled = 0;
// Menyoo-gap wave 2: police + peds + gangs ignore the player (game-thread re-assert).
static int g_ignored_by_all_enabled = 0;
static int g_bodyguard_invincible_enabled = 0;
static uint32_t g_bodyguard_health_index = 1u;      // 200 HP
static uint32_t g_bodyguard_armor_index = 1u;       // 100 armor
static uint32_t g_bodyguard_formation_index = 0u;   // default group formation
static uint32_t g_bodyguard_aggression_index = 0u;  // 0 = DEFENSIVE (react & protect)
static uint32_t g_bodyguard_weapon_index = 0u;      // 0 = CARBINE (armed so guards can defend)
static uint32_t g_bodyguard_accuracy_index = 2u;    // 2 = HIGH (effective return fire)
static int g_bodyguard_blips_enabled = 0;           // tag tracked guards on the minimap
// Combat config (driven by the aggression preset) is applied at spawn and re-applied on the
// game-thread bodyguard tick whenever the preset changes -- this flag marks that pending work.
static int g_bodyguard_combat_dirty = 0;
static uint32_t g_timecycle_index = 0;
static uint32_t g_animpostfx_index = 0;
static int g_xenon_enabled = 0;
static int g_hud_enabled = 0;
static int g_hud_speedo_enabled = 1;
static int g_hud_coords_enabled = 1;
static int g_hud_fps_enabled = 1;
static int g_hud_distance_enabled = 0;
static uint32_t g_weather_index = 0;
static uint32_t g_time_index = 0;
static uint32_t g_wanted_index = 0;
static uint32_t g_nitro_power_index = 1u;
static uint32_t g_timescale_index = 0;
static uint32_t g_gravity_index = 0;
static uint32_t g_clock_hour = 12u;
static uint32_t g_move_rate_index = 1u;       // index into kMoveRates; default MED (1.4x)
static uint32_t g_paint_primary_index = 0;    // index into kPaintColors
static uint32_t g_paint_secondary_index = 0;  // index into kPaintColors
static uint32_t g_window_tint_index = 0;      // index into kWindowTints
static uint32_t g_neon_index = 0;             // index into kColorPresets (0 = Off)
static uint32_t g_tyre_smoke_index = 0;       // index into kColorPresets (0 = Off)
// LSC expansion 2 cycler state.
static uint32_t g_wheel_type_index = 0;        // index into kWheelTypes
static int32_t g_livery_stage = -1;            // -1 = none, else livery index (live count)
static uint32_t g_plate_style_index = 0;       // number-plate style id (0..5)
static uint32_t g_plate_text_index = 0;        // index into kPlatePresets
static uint32_t g_pearl_color_index = 0;       // index into kPaintColors (pearlescent)
static uint32_t g_wheel_color_index = 0;       // index into kPaintColors (wheel tint)
static uint32_t g_custom_primary_index = 0;    // index into kCustomColors
static uint32_t g_custom_secondary_index = 0;  // index into kCustomColors
static int g_rainbow_neon_enabled = 0;         // per-tick neon hue cycle
static uint32_t g_rainbow_neon_phase = 0;      // 0..359 hue, advanced each tick
// Weapon-effect flags: written by the worker (toggle handler), read by the game/script
// thread (the frame-hook tick). volatile gives single-writer/single-reader visibility;
// relaxed ordering is fine for a 0/1 toggle (no dependent data is published with it).
static volatile int g_explosive_ammo_enabled = 0;
static volatile int g_fire_ammo_enabled = 0;
// Explosive Melee: like explosive ammo, but the game-thread tick only fires on a melee impact.
static volatile int g_explosive_melee_enabled = 0;
// No-Reload / Infinite Clip: flag flip on the worker; re-asserted on the game-thread player tick.
static int g_no_reload_enabled = 0;
// Weapon-damage multiplier: index into kWeaponDamageMults (0 = x1 / off). Re-asserted
// each worker tick because the engine resets the modifier on weapon switch.
static uint32_t g_weapon_damage_index = 0;
// Rainbow body-paint: per-tick custom-primary hue sweep (same shape as rainbow neon).
static int g_rainbow_paint_enabled = 0;
static uint32_t g_rainbow_paint_phase = 0;
// Noclip / free-move: while enabled the chosen player entity is frozen and re-placed
// each frame along the look vector. The per-frame driver runs on the GAME thread (the
// frame-hook tick), while the toggle + disable_all run on the scePad worker thread, so
// these two are touched from both threads -> volatile. g_noclip_entity is the frozen
// handle so the driver can unfreeze the previous entity if the player swaps
// ped<->vehicle, and the disable path can release whatever is currently frozen.
static volatile int g_noclip_enabled = 0;
static volatile int g_noclip_entity = 0;
// Menu-visible state mirrored from the worker tick so the game-thread noclip driver can
// stand down while the menu is open (else it would steal the movement stick from nav).
static volatile int g_menu_open = 0;
// Fly modes (fly.inc): velocity-driven flight, run on the GAME thread from
// gtav_features_game_thread_tick(). Volatile mode state (NOT saved toggle bits, conserving the
// scarce toggle mask): g_fly_mode = on-foot player fly (Off/On/Fast), g_vehicle_fly_mode = current
// vehicle fly (Off/On/Fast). Touched from both threads (the cycler runs on the worker, the driver
// on the game thread), like g_noclip_enabled.
static volatile int g_fly_mode = 0;
static volatile int g_vehicle_fly_mode = 0;
// Gun toolkit (gun_toolkit.inc): the active gun (0 Off / 1 Gravity / 2 Teleport / 3 Kaboom). The
// cycler runs on the worker; the raycast driver runs on the game thread -> volatile, like the fly
// modes. Not a saved toggle bit.
static volatile int g_active_gun = 0;
// World spectacle (spectacle.inc). g_wind_index is worker-only (cycler + value label), runtime-only
// (not persisted). g_cam_shake_index is the camera-shake mode: the worker cycler writes it and the
// game-thread tick re-asserts the shake -> volatile.
static uint32_t g_wind_index = 0;
static volatile int g_cam_shake_index = 0;
// Free / cinematic camera (free_cam.inc). g_free_cam_active is the transient master flag (NOT a
// saved toggle): set on enter by the worker (FREE_CAM action), cleared by the game-thread driver
// when the user exits (Circle) -- so it is touched from both threads, like
// g_object_move_mode_enabled. g_free_cam_speed_index selects the fly speed (worker cycler + value
// label). g_free_cam_vis_request is the worker-consumed menu-visibility request (0 none / 1 hide /
// 2 show), exactly like the object move mode: the game-thread driver cannot call
// gtav_menu_set_visible itself.
static volatile int g_free_cam_active = 0;
static uint32_t g_free_cam_speed_index = 1u;  // index into kFreeCamSpeeds; default Normal
static volatile int g_free_cam_vis_request = 0;
// Wardrobe full-player camera (wardrobe.inc). Worker sets this true while the Wardrobe submenu is
// open (visible, not parked); the game-thread driver creates a scripted cam in front of the ped so
// the user sees outfit changes "in store" style, and tears it down when the flag clears. Outside
// the frame-hook gate so the worker setter + a no-hook build both compile.
static volatile int g_wardrobe_cam_active = 0;
// Interactive object placement. g_object_move_mode_enabled is the transient master flag
// (NOT a saved toggle): set on enter (worker, via Move Last Object, or game thread, via
// auto-edit-on-spawn), read + cleared by the game-thread driver (object_move.inc). It runs
// while the menu is CLOSED -- the inverse of noclip's stand-down. g_object_move_entity is the
// driver's currently-acquired (frozen) object handle (0 = re-acquire next tick); shared here so
// the auto-edit hook in spawn_entity.inc can force a re-acquire by zeroing it. g_auto_edit_object
// is the saved display toggle. g_object_move_vis_request is the worker-consumed menu-visibility
// request (0 none / 1 hide / 2 show): the driver cannot call gtav_menu_set_visible itself (it
// does worker-thread-only logging), so the worker applies it from gtav_menu_worker_tick.
static volatile int g_object_move_mode_enabled = 0;
static int g_object_move_entity = 0;
static int g_auto_edit_object_enabled = 0;
static volatile int g_object_move_vis_request = 0;
// Move-mode sub-state (driven by the game-thread driver; read by the worker readout for the HUD).
// g_object_move_aim_mode: 0 = free-move (stick translate), 1 = aim-follow (prop tracks the camera
// look at g_object_move_aim_dist metres). g_object_move_surface_snap: aim sub-layer that raycasts
// the prop onto the first surface under the camera forward. g_object_move_sel_index: edit-target
// cursor into the live object roster (-1 = the last-spawned default, back-compatible).
static volatile int g_object_move_aim_mode = 0;
static volatile int g_object_move_surface_snap = 0;
static int g_object_move_sel_index = -1;
// Free-move ground-lock: when on, the prop's Z snaps to the terrain under its XY every frame
// (D-pad Down toggles it in free-move). Transient per session.
static volatile int g_object_move_ground_lock = 0;
// Set at every entry (manual or auto-edit-on-spawn) so the driver seeds the button latches from the
// live pad state on its first tick -- a button still held from the spawn/entry press is then NOT
// misread as a fresh press (which used to instant-commit/place the prop). Declared here, before the
// .inc includes, so spawn_entity.inc's auto-edit path can set it too.
static volatile int g_object_move_consume_entry = 0;
// When set, Circle in move mode CANCELS THE SPAWN -- it deletes the prop instead of reverting it to
// the entry pose. Set only on auto-edit-on-spawn entry (the prop didn't exist before, so "cancel"
// means "undo the spawn"); cleared on manual Move-Last entry and whenever the edit target changes
// (select/duplicate/delete) so Circle can never delete an unrelated or pre-existing prop.
static volatile int g_object_move_cancel_deletes = 0;
static float g_object_move_aim_dist = 3.0f;
// Skip-Prologue: once armed (by the Skip Prologue action), the WORKER tick continuously
// stamps the prologue1 mission-complete script global. The worker runs during a story load
// (the game-thread hook does not), so re-stamping every tick wins the flow-controller's
// prologue-launch read after the corrupt save re-zeros the global. Plain global store, no
// native, no cloud sync -- cannot trigger the profile-setting crash. Idempotent (value=1).
// Only meaningful when the script-globals write path is compiled in.
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
static volatile int g_prologue_skip_armed = 0;
#endif
// Debug overlay: read-only on-screen diagnostics panel (render-only, worker-safe).
static int g_debug_overlay_enabled = 0;

// Minigame "fun modes". Internal dense ids (0 = none); distinct from the action enum.
// Keep the order in sync with the GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_* mapping in
// minigame.inc and the kMinigamesItems[] menu rows.
enum {
  GTAV_MINIGAME_NONE = 0,
  GTAV_MINIGAME_RIOT,
  GTAV_MINIGAME_METEOR,
  GTAV_MINIGAME_STORM,
  GTAV_MINIGAME_INFERNO,
  GTAV_MINIGAME_RAGDOLL,
  GTAV_MINIGAME_BLACKOUT,  // Phase B
  GTAV_MINIGAME_ZOMBIES,   // Phase B
  GTAV_MINIGAME_FLOOD,     // Phase B
  GTAV_MINIGAME_COUNT,
};
// Mutually-exclusive display flags: at most one element is 1. One array (not N scalars) so a
// feature_toggles.def row can name an lvalue (g_minigame_on[GTAV_MINIGAME_X]) for the mask /
// init / restore / checkmark expansions. g_active_minigame is the authoritative active id the
// game-thread driver reads. volatile: written on the scePad worker (toggle handler /
// disable_all), read on the game thread (the frame-hook driver).
static volatile int g_minigame_on[GTAV_MINIGAME_COUNT];
static volatile int g_active_minigame = GTAV_MINIGAME_NONE;

// Streaming-aware teleport state, advanced from the worker tick. While active the
// chosen entity is frozen at the destination so it can't fault on unstreamed void;
// we keep requesting collision and release it once the area streams in (or times
// out), letting it settle onto the now-loaded ground.
static int g_tp_active;
static int g_tp_entity;
static float g_tp_x;
static float g_tp_y;
static float g_tp_z;
static int g_tp_ticks_left;
// How to resolve the destination Z when the freeze is released (set per teleport kind by
// start_teleport). Waypoint/objective blips carry only X/Y, so they SNAP onto the streamed
// ground -- the fix for arriving in the sky or under the terrain. Curated presets and the
// saved location keep their stored Z but get a FLOOR safety net: raised onto the ground only
// when they would otherwise sit below it, which can never lower an intentional rooftop /
// mountain height. NONE keeps the exact Z (legacy escape hatch). Resolved in the poll once
// collision has streamed in -- GET_GROUND_Z reads false on an unloaded cell.
enum GtavTeleportClamp {
  GTAV_TP_CLAMP_NONE = 0,
  GTAV_TP_CLAMP_FLOOR = 1,
  GTAV_TP_CLAMP_SNAP = 2,
};
static int g_tp_clamp;       // GtavTeleportClamp for the in-flight teleport
static int g_tp_is_vehicle;  // destination entity is a vehicle (seat it ON_GROUND_PROPERLY)
// ~3s at the 90 Hz worker cadence: long enough to stream a far cell, bounded so a
// failed stream still releases the freeze instead of stranding the player.
static const int GTAV_TELEPORT_TIMEOUT_TICKS = 270;
// Lift applied above the resolved ground when a teleport settles: small for a ped (just clear
// the mesh), a touch more for a vehicle so it drops onto its wheels (then squared up by
// SET_VEHICLE_ON_GROUND_PROPERLY). HW-tunable, like Stick-to-Ground's airborne band.
static const float GTAV_TP_GROUND_OFFSET_PED = 1.0f;
static const float GTAV_TP_GROUND_OFFSET_VEHICLE = 1.5f;
// Full vehicle health for engine/body/petrol-tank setters (GTA's stock maximum).
static const float GTAV_VEHICLE_FULL_HEALTH = 1000.0f;

struct Vec3 {
  float x;
  float y;
  float z;
};

static Vec3 g_saved_location;

static void set_message(const char* message) {
#if GTAV_FEATURES_NO_LIBC_FORMAT
  gtav_copy_string(g_state.last_message, sizeof(g_state.last_message), message);
#else
  snprintf(g_state.last_message, sizeof(g_state.last_message), "%s", message ? message : "");
#endif
}

// Short call-site spellings shared with native_bridge.cpp (native_invoke.hpp).
using gtavmenu::invoke_return;
using gtavmenu::invoke_void;

// Drift Mode speed gate. SET_VEHICLE_REDUCE_GRIP is binary at the native level and GTA
// documents it as reducing grip "so it's hard to go anywhere" -- held on at a standstill
// the wheels spin without traction and the car can never launch. So we only assert the
// grip cut once the car is already rolling and restore full grip below the threshold,
// letting it accelerate away normally and only break loose when there is speed to slide.
// SET_VEHICLE_REDUCE_GRIP is a hard on/off (there is no graded grip native bound), so the
// only control we have over feel is WHEN it engages. Engaging at a low speed made the car
// slip under throttle almost from launch -- the reported "accelerating is a serious
// challenge". Raise the engage point to ~40 km/h so low/mid-speed driving and launches keep
// full grip, and use a hysteresis band (engage high, release lower) so the grip doesn't
// chatter on/off around a single threshold, which felt twitchy and inconsistent.
static const float GTAV_DRIFT_ENGAGE_MPS = 11.0f;  // ~40 km/h: only slide once moving fast
static const float GTAV_DRIFT_RELEASE_MPS = 8.0f;  // ~29 km/h: restore grip below this

// Whether drift grip-reduction should be asserted on vehicle `v` right now. Stateful so the
// engage/release thresholds form a hysteresis band. Falls back to 1 (the old always-on
// behaviour) if the speed getter is unavailable.
static int drift_reduce_grip_now(int v) {
  if (!g_n.get_entity_speed) return 1;
  static int s_drift_engaged = 0;
  const float spd = invoke_return<float>(g_n.get_entity_speed, v);
  if (s_drift_engaged) {
    if (spd < GTAV_DRIFT_RELEASE_MPS) s_drift_engaged = 0;
  } else if (spd >= GTAV_DRIFT_ENGAGE_MPS) {
    s_drift_engaged = 1;
  }
  return s_drift_engaged;
}

// Some native wrappers inspect the game-thread context pointer at TLS
// fs:-GTAV_TLS_GAME_CTX_OFFSET. On a real game/script thread that slot is populated;
// on the injected scePad worker it is not a valid game-thread context. The worker
// wrapper below is kept for legacy diagnostics and for calls that only needed the
// wrapper prologue to survive, but it is NOT a safe vehicle-spawn lane: CREATE_VEHICLE
// still allocates through non-thread-safe entity/streaming systems and must be queued
// to a real game-thread consumer.
#if defined(__x86_64__) || defined(__amd64__)
// Address of the script-context TLS slot for the CURRENT thread, or nullptr if fs:0 does
// not look like a TLS base. The guard matters because the slot is computed by SUBTRACTING
// from fs:0: without it an unexpected small value underflows to a near-2^64 address that
// faults the whole game the moment a caller dereferences it. Every caller must null-check.
static inline void** gtav_tls_game_ctx_slot() {
  uintptr_t fsbase;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(fsbase));
  if (fsbase < GTAV_TLS_GAME_CTX_OFFSET) return nullptr;
  return reinterpret_cast<void**>(fsbase - GTAV_TLS_GAME_CTX_OFFSET);
}
// Borrowed game-thread context (rage sysThreadType*). Written by an external
// kernel-capable agent in experimental builds. Borrowing this pointer can satisfy
// wrappers that only require a non-garbage script context, but it does not move
// execution onto the game thread and does not make entity allocation safe.
// Exposed via gtav_features_borrowed_ctx_slot() for the agent to fill.
extern "C" {
volatile uintptr_t gtav_borrowed_game_thread_ctx __attribute__((used, visibility("default"))) = 0;
}
extern "C" uintptr_t gtav_features_borrowed_ctx_slot() {
  return reinterpret_cast<uintptr_t>(&gtav_borrowed_game_thread_ctx);
}
static inline void* gtav_worker_ctx_value() {
  uintptr_t b = gtav_borrowed_game_thread_ctx;
  return b ? reinterpret_cast<void*>(b) : nullptr;
}
template <typename... Args>
static void invoke_void_worker(uint64_t address, Args... args) {
  void** slot = gtav_tls_game_ctx_slot();
  if (!slot) {  // no usable TLS base: call plainly rather than write through a bad slot
    gtavmenu::invoke_native_void(address, args...);
    return;
  }
  void* saved = *slot;
  *slot = gtav_worker_ctx_value();
  gtavmenu::invoke_native_void(address, args...);
  *slot = saved;
}
template <typename R, typename... Args>
static R invoke_return_worker(uint64_t address, Args... args) {
  void** slot = gtav_tls_game_ctx_slot();
  if (!slot) {
    return gtavmenu::invoke_native_return<R>(address, args...);
  }
  void* saved = *slot;
  *slot = gtav_worker_ctx_value();
  R r = gtavmenu::invoke_native_return<R>(address, args...);
  *slot = saved;
  return r;
}
#else
template <typename... Args>
static void invoke_void_worker(uint64_t address, Args... args) {
  gtavmenu::invoke_native_void(address, args...);
}
template <typename R, typename... Args>
static R invoke_return_worker(uint64_t address, Args... args) {
  return gtavmenu::invoke_native_return<R>(address, args...);
}
#endif

// Context-selecting invoke for legacy worker diagnostics versus the real
// game-thread lane:
//   worker=1: scePad worker path; may block for model preload, but must not be
//             treated as safe for CREATE_VEHICLE in production builds.
//   worker=0: queue consumer running on a real script/native context; call natives
//             plainly so GTA sees the thread-local game context it set up.
// wait_for_model's allow_block flag and spawn_vehicle_model_hash's worker arg carry
// this same distinction (1=worker/preload-or-diagnostic, 0=game-thread/non-blocking).
template <typename... Args>
static void invoke_void_ctx(int worker, uint64_t address, Args... args) {
  if (worker) {
    invoke_void_worker(address, args...);
  } else {
    gtavmenu::invoke_native_void(address, args...);
  }
}
template <typename R, typename... Args>
static R invoke_return_ctx(int worker, uint64_t address, Args... args) {
  return worker ? invoke_return_worker<R>(address, args...)
                : gtavmenu::invoke_native_return<R>(address, args...);
}

// GET_ENTITY_COORDS returns a rage scrVector: three floats each padded to 8
// bytes (x at returns[0], y at returns[1], z at returns[2]).
static Vec3 invoke_vector(uint64_t address, int entity, int alive) {
  Vec3 zero = {0.0f, 0.0f, 0.0f};
  if (!address) return zero;  // unresolved native slot: zeroed vec, never a null jump
  gtavmenu::NativeCallContext ctx;
  ctx.reset();
  ctx.push(entity);
  ctx.push(alive);
  gtavmenu::note_native_call(address);
  ((void (*)(gtavmenu::NativeArg*))(uintptr_t)address)(&ctx.native_arg);
  Vec3 v;
  v.x = *reinterpret_cast<const float*>(&ctx.returns[0]);
  v.y = *reinterpret_cast<const float*>(&ctx.returns[1]);
  v.z = *reinterpret_cast<const float*>(&ctx.returns[2]);
  return v;
}

// GET_BLIP_COORDS(blip) returns a rage scrVector the same way GET_ENTITY_COORDS
// does, but takes a single blip-id arg. (Waypoint blips populate x/y; z is 0.)
static Vec3 invoke_vector_blip(uint64_t address, int blip) {
  Vec3 zero = {0.0f, 0.0f, 0.0f};
  if (!address) return zero;  // unresolved native slot: zeroed vec, never a null jump
  gtavmenu::NativeCallContext ctx;
  ctx.reset();
  ctx.push(blip);
  gtavmenu::note_native_call(address);
  ((void (*)(gtavmenu::NativeArg*))(uintptr_t)address)(&ctx.native_arg);
  Vec3 v;
  v.x = *reinterpret_cast<const float*>(&ctx.returns[0]);
  v.y = *reinterpret_cast<const float*>(&ctx.returns[1]);
  v.z = *reinterpret_cast<const float*>(&ctx.returns[2]);
  return v;
}

static int native_player_id() {
  return invoke_return<int>(g_n.player_id);
}
static int native_player_ped_id() {
  return invoke_return<int>(g_n.player_ped_id);
}

// Validated handle accessors for the per-tick re-assertions. PLAYER_PED_ID returns 0
// when there is no usable player ped (loading screen, dead, cutscene), so passing it
// to a ped native faults the same way the spawn path guards against (ped <= 0).
// PLAYER_ID is different: index 0 is the VALID local player and -1 means invalid, so
// callers must treat >= 0 as valid -- never > 0, which would reject the local player.
static int valid_player_ped() {
  int ped = native_player_ped_id();
  return ped > 0 ? ped : 0;
}
static int valid_player_id() {
  int player = native_player_id();
  return player >= 0 ? player : -1;
}

static int has_core_player_natives() {
  return g_n.player_id && g_n.player_ped_id;
}

static uint32_t unavailable(uint32_t action, const char* message) {
  g_state.last_action = action;
  g_state.last_result = GTAV_FEATURE_RESULT_UNAVAILABLE;
  set_message(message);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "feature unavailable action=%s reason=%s",
                     gtav_feature_action_name(action), message);
  return g_state.last_result;
}

static uint32_t ok(uint32_t action, const char* message) {
  g_state.last_action = action;
  g_state.last_result = GTAV_FEATURE_RESULT_OK;
  set_message(message);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "feature ok action=%s message=%s",
                     gtav_feature_action_name(action), message);
  return g_state.last_result;
}

static uint32_t failed(uint32_t action, const char* message) {
  g_state.last_action = action;
  g_state.last_result = GTAV_FEATURE_RESULT_FAILED;
  set_message(message);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "feature failed action=%s reason=%s",
                     gtav_feature_action_name(action), message);
  return g_state.last_result;
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Defined in noclip.inc / minigame.inc / autopilot.inc (all included after weapon_fx.inc,
// whose game-thread tick calls them).
static void noclip_game_thread_tick(void);
static void minigame_game_thread_tick(void);
static void autopilot_game_thread_tick(void);
static void object_move_game_thread_tick(void);
static void emote_game_thread_tick(void);
static void fly_game_thread_tick(void);
static void gun_toolkit_game_thread_tick(void);
static void free_cam_game_thread_tick(void);
static void spectacle_game_thread_tick(void);
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && \
    GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && GTAV_MENU_ENABLE_NATIVE_FEATURES
// Native instructional-button bar (instructional_buttons.inc): requests/builds/draws the Scaleform
// on the game thread. Forward-declared here so weapon_fx.inc's game-thread tick can call it.
static void instructional_game_thread_tick(void);
#endif
#endif
// Clear every minigame mode (and the active id). Defined in minigame.inc; called from
// disable_all_features (teleport.inc, included later). Flag-clear only -- no native call --
// so it is safe to run at init via import_profile().
static void minigame_stop_all(void);

// Apply the spawner-default toggles (Spawn Maxed / Spawn Invincible) to a freshly created
// vehicle. Defined in lsc.inc (where the vehicle-mod slot ids live) but called from
// spawn_vehicle_model_hash in spawn_skin.inc, which is included earlier -- hence the forward
// declaration. The `worker` arg is the spawn job's context flag (= allow_block), so the body
// uses invoke_*_ctx to run on the correct (game) thread.
static void apply_spawn_vehicle_defaults(int worker, int vehicle);

// Minimal libm-free sine/cosine (the payload ELF and host tests do not link -lm; only
// builtin-lowered sqrtf is safe). Bhaskara-style parabola + one refinement pass, accurate to
// ~0.001 -- ample for a placement-direction vector. Input in radians. (noclip.inc keeps its
// own copy because those are gated behind the frame-hook build; these stay always-available so
// the object spawner can place props in any build.)
static const float GTAV_FEAT_PI = 3.14159265358979323846f;
static const float GTAV_FEAT_DEG2RAD = 0.017453292519943295f;
static float feat_sinf(float x) {
  const float two_pi = 2.0f * GTAV_FEAT_PI;
  while (x > GTAV_FEAT_PI) x -= two_pi;
  while (x < -GTAV_FEAT_PI) x += two_pi;
  const float b = 4.0f / GTAV_FEAT_PI;
  const float c = -4.0f / (GTAV_FEAT_PI * GTAV_FEAT_PI);
  float y = b * x + c * x * (x < 0.0f ? -x : x);
  y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;  // refinement (P = 0.225)
  return y;
}
static float feat_cosf(float x) {
  return feat_sinf(x + 0.5f * GTAV_FEAT_PI);
}

// Feature implementations, split into per-group fragments for readability. These
// are textually composed (not separate translation units), so every symbol stays
// a file-local static sharing the state and helpers above. The order matters
// (definitions before the dispatch/worker-tick below), so keep clang-format from
// sorting these includes.
// clang-format off
#include "features/self_actions.inc"
#include "features/spawned_entities.inc"
#include "features/spawn_skin.inc"
#include "features/vehicle_preview.inc"
#include "features/vehicle_weapons.inc"
#include "features/toggles.inc"
#include "features/world.inc"
#include "features/weapon_tint.inc"
#include "features/weapon_attachments.inc"
#include "features/vehicle_controls.inc"
#include "features/wardrobe.inc"
#include "features/lsc.inc"
#include "features/saved_vehicles.inc"
#include "features/effects.inc"
#include "features/spawn_entity.inc"
#include "features/companions.inc"
#include "features/ped_control.inc"
#include "features/scenarios.inc"
#include "features/emotes.inc"
#include "features/weapon_fx.inc"
#include "features/noclip.inc"
#include "features/fly.inc"
#include "features/object_move.inc"
#include "features/gun_toolkit.inc"
#include "features/free_cam.inc"
#include "features/spectacle.inc"
#include "features/autopilot.inc"
#include "features/minigame.inc"
#include "features/player_money.inc"
#include "features/keybinds.inc"
#include "features/session.inc"
#include "features/spooner.inc"
#include "features/custom_device.inc"
#include "features/spooner_tweaks.inc"
#include "features/instructional_buttons.inc"
// clang-format on

// Index a label table by a wraparound index: LABEL_OF(kFoo, idx) is the element
// kFoo[idx % count]. Collapses the repeated kFoo[idx % (sizeof(kFoo)/sizeof(kFoo[0]))]
// spelling in the value-label switch below to one readable call (#undef'd after it).
#define LABEL_OF(table, idx) ((table)[(idx) % (sizeof(table) / sizeof((table)[0]))])

// Current display value for a list-cycler action (weather/time), pushed to the menu
// so it can render "< VALUE >". Empty string for non-list actions.
extern "C" const char* gtav_features_value_label(uint32_t action) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEATHER:
      return LABEL_OF(kWeatherTypes, g_weather_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME:
      return LABEL_OF(kTimesOfDay, g_time_index).name;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WANTED:
      return LABEL_OF(kWantedLevels, g_wanted_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NITRO_POWER:
      return current_nitro_profile().label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CRUISE_SPEED:
      return LABEL_OF(kCruiseSpeeds, g_cruise_speed_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_DOOR:
      return LABEL_OF(kVehicleDoorNames, g_vehicle_door_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_HEADLIGHTS:
      return LABEL_OF(kHeadlightNames, g_headlight_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_LOCK:
      return LABEL_OF(kVehicleLockNames, g_vehicle_lock_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_TINT:
      return LABEL_OF(kWeaponTintNames, g_weapon_tint_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SUPP:
      return g_wattach_on[GTAV_WATTACH_SUPP] ? "On" : "Off";
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SCOPE:
      return g_wattach_on[GTAV_WATTACH_SCOPE] ? "On" : "Off";
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_GRIP:
      return g_wattach_on[GTAV_WATTACH_GRIP] ? "On" : "Off";
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_CLIP:
      return g_wattach_on[GTAV_WATTACH_CLIP] ? "On" : "Off";
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_FLASH:
      return g_wattach_on[GTAV_WATTACH_FLASH] ? "On" : "Off";
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_SLOT:
      return kPedSlots[g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS].name;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_STYLE:
      snprintf(g_wardrobe_style_label, sizeof(g_wardrobe_style_label), "%u",
               g_wardrobe_drawable[g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS]);
      return g_wardrobe_style_label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_TEXTURE:
      snprintf(g_wardrobe_texture_label, sizeof(g_wardrobe_texture_label), "%u",
               g_wardrobe_texture[g_wardrobe_slot_index % GTAV_WARDROBE_SLOTS]);
      return g_wardrobe_texture_label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_SAVED_VEHICLE_SLOT:
      return saved_vehicle_slot_value_label();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OUTFIT_SLOT:
      return outfit_slot_value_label();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE:
      return LABEL_OF(kApModes, (uint32_t)g_autopilot_mode).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION:
      return LABEL_OF(kAp, g_autopilot_aggression_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED:
      return LABEL_OF(kApSpd, g_autopilot_speed_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE:
      return LABEL_OF(kFlyModes, (uint32_t)g_fly_mode).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY:
      return LABEL_OF(kFlyModes, (uint32_t)g_vehicle_fly_mode).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN:
      return LABEL_OF(kGunModes, (uint32_t)g_active_gun).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND:
      return LABEL_OF(kWindLevels, g_wind_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE:
      return LABEL_OF(kCamShakeLevels, (uint32_t)g_cam_shake_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED:
      return LABEL_OF(kFreeCamSpeeds, g_free_cam_speed_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA:
      return LABEL_OF(kEntityAlphaLevels, g_entity_alpha_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME_SCALE:
      return LABEL_OF(kTimeScales, g_timescale_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_GRAVITY:
      return LABEL_OF(kGravityLevels, g_gravity_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_MOVE_RATE:
      return LABEL_OF(kMoveRates, g_move_rate_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_SPAWN_AT:
      return LABEL_OF(kObjSpawnAtLabels, g_obj_spawn_at_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_DISTANCE:
      return LABEL_OF(kObjDistances, g_obj_distance_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_HEADING:
      return LABEL_OF(kObjHeadings, g_obj_heading_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_ON_GROUND:
      return LABEL_OF(kObjOnGroundLabels, g_obj_on_ground_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_1:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_2:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_3:
      return keybind_value_label(action - GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE:
      return timecycle_value_label();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX:
      return animpostfx_value_label();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_PRIMARY:
      return LABEL_OF(kPaintColors, g_paint_primary_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_SECONDARY:
      return LABEL_OF(kPaintColors, g_paint_secondary_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WINDOW_TINT:
      return LABEL_OF(kWindowTints, g_window_tint_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NEON:
      return LABEL_OF(kColorPresets, g_neon_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TYRE_SMOKE:
      return LABEL_OF(kColorPresets, g_tyre_smoke_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_TYPE:
      return LABEL_OF(kWheelTypes, g_wheel_type_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_LIVERY: {
      const int count = live_livery_count();
      int lvl = g_livery_stage;
      if (lvl > count - 1) lvl = count - 1;
      if (lvl < -1) lvl = -1;
      return livery_label(lvl, count);
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_STYLE:
      return LABEL_OF(kPlateStyles, g_plate_style_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_TEXT:
      return LABEL_OF(kPlatePresets, g_plate_text_index);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PEARLESCENT:
      return LABEL_OF(kPaintColors, g_pearl_color_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_COLOR:
      return LABEL_OF(kPaintColors, g_wheel_color_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_PRIMARY:
      return LABEL_OF(kCustomColors, g_custom_primary_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_SECONDARY:
      return LABEL_OF(kCustomColors, g_custom_secondary_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CLOCK_HOUR: {
      static char s_clock_label[8];
      snprintf(s_clock_label, sizeof(s_clock_label), "%02u:00", g_clock_hour % 24u);
      return s_clock_label;
    }
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_DAMAGE:
      return LABEL_OF(kWeaponDamageMults, g_weapon_damage_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CASH:
      return cash_value_label();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_HEALTH:
      return LABEL_OF(kBodyguardHealthPresets, g_bodyguard_health_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ARMOR:
      return LABEL_OF(kBodyguardArmorPresets, g_bodyguard_armor_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_FORMATION:
      return LABEL_OF(kBodyguardFormationPresets, g_bodyguard_formation_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_AGGRESSION:
      return LABEL_OF(kBodyguardAggression, g_bodyguard_aggression_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_WEAPON:
      return LABEL_OF(kBodyguardWeapons, g_bodyguard_weapon_index).label;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ACCURACY:
      return LABEL_OF(kBodyguardAccuracy, g_bodyguard_accuracy_index).label;
    default:
      break;
  }
  // Per-slot mod pickers share one range handler (contiguous action block). The "/M"
  // tracks the live vehicle, so recompute the count here rather than caching it.
  if (action >= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST &&
      action <= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_LAST) {
    sync_mod_stage_to_vehicle();  // reflect the car's actual installed tiers
    const uint32_t idx = action - GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST;
    const int count = live_mod_count(kVehicleModSlots[idx].slot);
    int lvl = g_mod_stage[idx];
    if (lvl > count - 1) lvl = count - 1;
    if (lvl < -1) lvl = -1;
    return mod_level_label(kVehicleModSlots[idx].slot, lvl, count);
  }
  return "";
}
#undef LABEL_OF

// Defined further down; import_profile resets state through it before re-applying.
static uint32_t disable_all_features();

// Fill the HUD readout from worker-safe getter natives. Speed is converted m/s ->
// km/h. Coords/heading come from the player entity (the vehicle when driving, so the
// readout tracks the car). Each field is guarded on its getter so a partial table
// still yields a usable (if sparser) overlay. Returns whether the overlay should draw.
extern "C" int gtav_features_hud_readout(GtavHudReadout* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  out->hud_enabled = g_hud_enabled ? 1u : 0u;
  out->show_speedo = g_hud_speedo_enabled ? 1u : 0u;
  out->show_coords = g_hud_coords_enabled ? 1u : 0u;
  out->show_fps = g_hud_fps_enabled ? 1u : 0u;
  out->show_distance = g_hud_distance_enabled ? 1u : 0u;
  if (!g_hud_enabled || !g_n.player_ped_id) return g_hud_enabled ? 1 : 0;

  int ped = native_player_ped_id();
  if (!ped) return 1;
  int in_vehicle = 0;
  int v = 0;
  if (g_n.is_ped_in_any_vehicle && g_n.get_vehicle_ped_is_in &&
      invoke_return<int>(g_n.is_ped_in_any_vehicle, ped, 0)) {
    v = invoke_return<int>(g_n.get_vehicle_ped_is_in, ped, 0);
    in_vehicle = v ? 1 : 0;
  }
  out->in_vehicle = in_vehicle ? 1u : 0u;
  int entity = in_vehicle ? v : ped;
  if (g_n.get_entity_speed) {
    out->speed_kmh = invoke_return<float>(g_n.get_entity_speed, entity) * 3.6f;
  }
  if (g_n.get_entity_coords) {
    Vec3 c = invoke_vector(g_n.get_entity_coords, entity, 1);
    out->x = c.x;
    out->y = c.y;
    out->z = c.z;
  }
  if (g_n.get_entity_heading) {
    out->heading = invoke_return<float>(g_n.get_entity_heading, entity);
  }
  // Distance to the active waypoint blip (planar X/Y), reusing the same blip natives as
  // teleport-to-waypoint. Player position comes from the ped, not the vehicle, so the
  // readout tracks the player. has_waypoint stays 0 when no waypoint is set.
  if (g_hud_distance_enabled && g_n.get_first_blip_info_id && g_n.get_blip_coords &&
      g_n.get_entity_coords) {
    int blip = invoke_return<int>(g_n.get_first_blip_info_id, 8);
    if (blip != 0 &&
        !(g_n.get_blip_info_id_type && invoke_return<int>(g_n.get_blip_info_id_type, blip) == 0)) {
      Vec3 wp = invoke_vector_blip(g_n.get_blip_coords, blip);
      Vec3 cur = invoke_vector(g_n.get_entity_coords, ped, 1);
      float dx = wp.x - cur.x;
      float dy = wp.y - cur.y;
      out->distance_m = sqrtf(dx * dx + dy * dy);
      out->has_waypoint = 1u;
    }
  }
  return 1;
}

// Fill the Zombie Outbreak survival HUD snapshot for the worker render path. Worker-safe: plain
// reads of the game-thread-owned survival state (defined in minigame.inc, under the frame-hook
// gate) -- never a native call. out->active is 1 only while Zombie Outbreak is the active
// minigame, which is the HUD's draw gate; the survival state is inert without the frame hook.
extern "C" int gtav_features_zombie_readout(GtavZombieReadout* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (g_active_minigame != GTAV_MINIGAME_ZOMBIES) return 1;
  out->active = 1u;
  out->wave = (uint32_t)(g_zombie_wave < 0 ? 0 : g_zombie_wave);
  out->kills = g_zombie_kills;
  const int remaining = g_zombie_roster_n + g_zombie_to_spawn;
  out->remaining = (uint32_t)(remaining < 0 ? 0 : remaining);
  out->state = (uint32_t)g_zombie_state;
#endif
  return 1;
}

// Fill the interactive object-move readout for the worker render path. Worker-safe: plain
// reads of the game-thread-owned latch (object_move.inc), never a getter native. out->active
// gates the placement HUD; it is set only while move mode is engaged and an object is
// acquired (the latch holds a real pose).
extern "C" int gtav_features_object_move_readout(GtavObjectMoveReadout* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_object_move_mode_enabled || !g_object_move_entity) return 1;
  out->active = 1u;
  out->can_rotate3 = object_move_can_rotate3() ? 1u : 0u;
  out->coarse = g_object_move_coarse ? 1u : 0u;
  out->x = g_object_move_pos.x;
  out->y = g_object_move_pos.y;
  out->z = g_object_move_pos.z;
  out->heading = g_object_move_heading;
  out->pitch = g_object_move_pitch;
  out->roll = g_object_move_roll;
  out->aim_mode = g_object_move_aim_mode ? 1u : 0u;
  out->aim_avail = g_n.get_gameplay_cam_coord ? 1u : 0u;
  out->snap_mode = g_object_move_surface_snap ? 1u : 0u;
  out->snap_avail = (g_n.start_shape_test_probe && g_n.get_shape_test_result) ? 1u : 0u;
  out->ground_lock = g_object_move_ground_lock ? 1u : 0u;
  out->aim_dist = g_object_move_aim_dist;
  {
    const int cnt = object_move_live_count();
    const int idx = object_move_index_of(g_object_move_entity);
    out->sel_count = (uint32_t)(cnt < 0 ? 0 : cnt);
    out->sel_index = (uint32_t)(idx < 0 ? 0 : idx + 1);
  }
#endif
  return 1;
}

// Consume the game-thread move driver's pending menu-visibility request (0 none / 1 hide /
// 2 show), clearing it. Applied by the worker (gtav_menu_worker_tick) because the driver
// cannot safely call gtav_menu_set_visible from the game thread.
extern "C" int gtav_features_take_menu_visibility_request(void) {
  int r = g_object_move_vis_request;
  if (r) {
    g_object_move_vis_request = 0;
    return r;
  }
  // Free Camera shares the same worker-applied hide/show channel (the two modes are mutually
  // exclusive -- you cannot be placing an object and flying the free cam at once).
  r = g_free_cam_vis_request;
  if (r) g_free_cam_vis_request = 0;
  return r;
}

// Fill the read-only diagnostics snapshot for the debug overlay. Returns whether the
// overlay toggle is on. Pulls frame-hook telemetry (under the frame-hook gate), the
// resolved-native count, and the weapon-fx diagnostic counters (same TU statics).
extern "C" int gtav_features_debug_stats(GtavDebugStats* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  out->overlay_enabled = g_debug_overlay_enabled ? 1u : 0u;
  out->native_ready = g_state.native_ready;
  out->last_action = g_state.last_action;
  out->last_result = g_state.last_result;
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
  out->native_count = GTAV_NATIVE_ADDR_COUNT;
#endif
#if GTAV_MENU_ENABLE_FRAME_HOOK
  out->hook_active = gtav_frame_hook_is_active() ? 1u : 0u;
  out->hook_calls = gtav_frame_hook_call_count();
  out->jobs_run = gtav_frame_hook_jobs_run();
  out->jobs_dropped = gtav_frame_hook_jobs_dropped();
#endif
  out->wfx_tick_fired = g_wfx_tick_fired;
  out->wfx_impact_hit = g_wfx_impact_hit;
  out->wfx_effect_requested = g_wfx_effect_requested;
  out->wfx_last_x = g_wfx_last_x;
  out->wfx_last_y = g_wfx_last_y;
  out->wfx_last_z = g_wfx_last_z;
  out->gun_tick_fired = g_gun_tick_fired;
  out->gun_fire_detected = g_gun_fire_detected;
  out->gun_raycast_hit = g_gun_raycast_hit;
  out->gun_effect_applied = g_gun_effect_applied;
  out->gun_last_x = g_gun_last_x;
  out->gun_last_y = g_gun_last_y;
  out->gun_last_z = g_gun_last_z;
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  // Global watch: sample the globals the subsystem manages so the overlay shows them flip
  // live (e.g. the despawn flag go 0->1 after the fix lands). Read-only plain slot reads via
  // the baked base -- safe from the render path. Only indices that are actually baked
  // (non-zero) are listed, so an un-baked feature contributes no row.
  if ((uintptr_t)GTAV_GLOBALS_BASE_ADDR != 0) {
    const uint32_t kWatch[] = {
        (uint32_t)GTAV_DESPAWN_GLOBAL_INDEX,
        (uint32_t)GTAV_PROLOGUE_GLOBAL_INDEX,
    };
    uint32_t n = 0;
    for (uint32_t i = 0; i < sizeof(kWatch) / sizeof(kWatch[0]) && n < GTAV_DEBUG_GLOBAL_WATCH_MAX;
         ++i) {
      if (kWatch[i] == 0u) continue;  // index not baked -> nothing to watch
      int32_t v = 0;
      out->sg_watch_valid[n] =
          gtav_script_read_global((uintptr_t)GTAV_GLOBALS_BASE_ADDR, kWatch[i], &v) == 0 ? 1u : 0u;
      out->sg_watch_index[n] = kWatch[i];
      out->sg_watch_value[n] = out->sg_watch_valid[n] ? v : 0;
      ++n;
    }
    out->sg_watch_count = n;
  }
#endif
  return g_debug_overlay_enabled ? 1 : 0;
}

// Capture the current menu state (toggles + cycler indices) into a profile struct.
extern "C" void gtav_features_export_profile(GtavFeatureProfile* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->version = GTAV_FEATURE_PROFILE_VERSION;
  out->toggle_mask = gtav_features_toggle_mask();
  out->weather_index = g_weather_index;
  out->time_index = g_time_index;
  out->wanted_index = g_wanted_index;
  out->nitro_power_index = g_nitro_power_index;
  out->timescale_index = g_timescale_index;
  out->gravity_index = g_gravity_index;
  out->clock_hour = g_clock_hour;
  out->move_rate_index = g_move_rate_index;
  out->bodyguard_health_index = g_bodyguard_health_index;
  out->bodyguard_armor_index = g_bodyguard_armor_index;
  out->bodyguard_formation_index = g_bodyguard_formation_index;
  out->bodyguard_aggression_index = g_bodyguard_aggression_index;
  out->bodyguard_weapon_index = g_bodyguard_weapon_index;
  out->bodyguard_accuracy_index = g_bodyguard_accuracy_index;
  out->cruise_speed_index = g_cruise_speed_index;
  // Menu customisation lives in the bridge (render state), not features.cpp's globals.
  out->theme_index = gtav_native_bridge_theme();
  out->region_index = gtav_native_bridge_region();
  // v11: Worker Hz / Toast Time cycler indices (bridge-held render state, applied by menu.c).
  out->worker_hz_index = gtav_native_bridge_worker_hz_index();
  out->toast_time_index = gtav_native_bridge_toast_time_index();
  // v12: Theme Editor custom-theme channels (accent/selection/panel RGB).
  static_assert(GTAV_CUSTOM_THEME_CHANNELS == GTAV_PROFILE_CUSTOM_THEME_CHANNELS,
                "custom-theme channel count must match between the bridge and the profile");
  gtav_native_bridge_get_custom_theme(out->custom_theme);
  // v13: feel/accessibility tuners (bridge-held; Nav Delay/Speed are applied to the pad by menu.c).
  out->reduce_motion = (uint32_t)gtav_native_bridge_reduce_motion();
  out->nav_delay_index = gtav_native_bridge_nav_delay_index();
  out->nav_speed_index = gtav_native_bridge_nav_speed_index();
  out->touchpad_enabled = (uint32_t)gtav_native_bridge_touchpad_enabled();
  out->panel_width_index = gtav_native_bridge_panel_width_index();
  // Browser favorites/recents also live in the bridge; copy them out for persistence.
  gtav_native_bridge_get_favorites(out->favorite_vehicles, out->favorite_peds);
  gtav_native_bridge_get_recents(out->recent_vehicles, out->recent_peds);
  // Global Quick pins (the top-level Quick menu) also live in the bridge.
  gtav_native_bridge_get_quick(out->quick_action, out->quick_param);
  // Live wardrobe editor working set (features.cpp-local) + the saved outfit slots.
  memcpy(out->wardrobe_drawable, g_wardrobe_drawable, sizeof(out->wardrobe_drawable));
  memcpy(out->wardrobe_texture, g_wardrobe_texture, sizeof(out->wardrobe_texture));
  memcpy(out->saved_outfit_drawable, g_saved_outfit_drawable, sizeof(out->saved_outfit_drawable));
  memcpy(out->saved_outfit_texture, g_saved_outfit_texture, sizeof(out->saved_outfit_texture));
  // Saved vehicle builds (per garage slot: model hash + indexed mod loadout).
  memcpy(out->saved_vehicle_model, g_saved_vehicle_model, sizeof(out->saved_vehicle_model));
  memcpy(out->saved_vehicle_mods, g_saved_vehicle_mods, sizeof(out->saved_vehicle_mods));
  // Keybinds: store each slot's resolved combo mask + its fixed feature action.
  for (uint32_t i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
    out->keybind_mask[i] = kKeybindCombos[g_keybind_combo[i] % kKeybindComboCount].mask;
    out->keybind_action[i] = kBindableActions[i].action;
  }
}

// Restore a profile: reset everything to defaults, set the cycler indices, then
// re-activate each toggle present in the mask so both live game state and the menu's
// displayed state match what was saved. Reaching activate() here is worker-safe -- the
// restored toggles are exactly the per-tick/modify-existing features.
extern "C" void gtav_features_import_profile(const GtavFeatureProfile* in) {
  if (!in) return;
  disable_all_features();
  // The profile comes off /data and may be hand-edited or corrupt, so clamp every
  // cycler index to its table at this trust boundary. Read sites already guard with
  // `% count`, but normalising here keeps the stored/displayed index honest.
  g_weather_index =
      in->weather_index % (uint32_t)(sizeof(kWeatherTypes) / sizeof(kWeatherTypes[0]));
  g_time_index = in->time_index % (uint32_t)(sizeof(kTimesOfDay) / sizeof(kTimesOfDay[0]));
  g_wanted_index = in->wanted_index % (uint32_t)(sizeof(kWantedLevels) / sizeof(kWantedLevels[0]));
  g_nitro_power_index =
      in->nitro_power_index % (uint32_t)(sizeof(kNitroProfiles) / sizeof(kNitroProfiles[0]));
  g_timescale_index =
      in->timescale_index % (uint32_t)(sizeof(kTimeScales) / sizeof(kTimeScales[0]));
  g_gravity_index =
      in->gravity_index % (uint32_t)(sizeof(kGravityLevels) / sizeof(kGravityLevels[0]));
  g_clock_hour = in->clock_hour % 24u;
  // HUD element flags are restored directly (their toggles flip state); start from the
  // saved bits so the master toggle below shows the intended elements.
  g_hud_speedo_enabled = (in->toggle_mask & GTAV_FEATURE_TOGGLE_HUD_SPEEDO) ? 1 : 0;
  g_hud_coords_enabled = (in->toggle_mask & GTAV_FEATURE_TOGGLE_HUD_COORDS) ? 1 : 0;
  g_hud_fps_enabled = (in->toggle_mask & GTAV_FEATURE_TOGGLE_HUD_FPS) ? 1 : 0;
  g_hud_distance_enabled = (in->toggle_mask & GTAV_FEATURE_TOGGLE_HUD_DISTANCE) ? 1 : 0;
  g_move_rate_index = in->move_rate_index % (uint32_t)(sizeof(kMoveRates) / sizeof(kMoveRates[0]));
  if (in->version >= 3u) {
    g_bodyguard_health_index =
        in->bodyguard_health_index %
        (uint32_t)(sizeof(kBodyguardHealthPresets) / sizeof(kBodyguardHealthPresets[0]));
    g_bodyguard_armor_index =
        in->bodyguard_armor_index %
        (uint32_t)(sizeof(kBodyguardArmorPresets) / sizeof(kBodyguardArmorPresets[0]));
    g_bodyguard_formation_index =
        in->bodyguard_formation_index %
        (uint32_t)(sizeof(kBodyguardFormationPresets) / sizeof(kBodyguardFormationPresets[0]));
    g_cruise_speed_index =
        in->cruise_speed_index % (uint32_t)(sizeof(kCruiseSpeeds) / sizeof(kCruiseSpeeds[0]));
  } else {
    g_bodyguard_health_index = 1u;
    g_bodyguard_armor_index = 1u;
    g_bodyguard_formation_index = 0u;
    g_cruise_speed_index = 2u;
  }
  // v4 field: pre-v4 profiles were memset to 0 by the loader, so they restore as index 0
  // (DEFENSIVE) -- the sensible default. Clamp unconditionally at this trust boundary.
  g_bodyguard_aggression_index =
      in->bodyguard_aggression_index %
      (uint32_t)(sizeof(kBodyguardAggression) / sizeof(kBodyguardAggression[0]));
  // v5 field: same trust-boundary clamp; pre-v5 profiles restore index 0 (CARBINE).
  g_bodyguard_weapon_index = in->bodyguard_weapon_index %
                             (uint32_t)(sizeof(kBodyguardWeapons) / sizeof(kBodyguardWeapons[0]));
  // v6 field: pre-v6 profiles memset this to 0 (LOW), which is weaker than the intended
  // default, so restore HIGH (index 2) for them instead; clamp at this trust boundary.
  const uint32_t accuracy_count =
      (uint32_t)(sizeof(kBodyguardAccuracy) / sizeof(kBodyguardAccuracy[0]));
  g_bodyguard_accuracy_index =
      (in->version >= 6u) ? (in->bodyguard_accuracy_index % accuracy_count) : 2u;
  g_bodyguard_combat_dirty = 1;
  // Menu customisation: the bridge setters clamp the index into range, so a corrupt
  // /data value can never select an out-of-bounds theme/region.
  gtav_native_bridge_set_theme(in->theme_index);
  gtav_native_bridge_set_region(in->region_index);
  // v11: Worker Hz / Toast Time cycler indices. Version-gated like the other late additions so a
  // pre-v11 file (memset-0 -> index 0) keeps the historical toast default (index 2 == 4.5 s)
  // rather than silently dropping to 2.0 s. menu.c clamps + applies these after features_init.
  gtav_native_bridge_set_worker_hz_index((in->version >= 11u) ? in->worker_hz_index : 0u);
  gtav_native_bridge_set_toast_time_index((in->version >= 11u) ? in->toast_time_index : 2u);
  // v12: custom-theme channels. Pre-v12 files have them zeroed by the memset-0 loader, which would
  // be an all-black custom theme; leave the bridge's default-palette seed in place for those.
  if (in->version >= 15u) {
    gtav_native_bridge_set_custom_theme(in->custom_theme);  // all 30 channels
  } else if (in->version >= 12u) {
    // v12-v14 stored only the 9 chrome channels (accent/selection/panel); apply those and keep the
    // default-palette seed for the colour groups added in v15 (else they'd load as black).
    gtav_native_bridge_set_custom_theme_count(in->custom_theme,
                                              GTAV_PROFILE_CUSTOM_THEME_CHANNELS_V12);
  }
  // v13: Reduce Motion + Nav Delay/Speed. Pre-v13 files (memset-0) version-gate to the historical
  // defaults -- full motion + Normal delay/speed (index 1) -- rather than the memset-0 values
  // (which would be index 0 == Short delay / Slow speed). menu.c clamps + applies after init.
  gtav_native_bridge_set_reduce_motion((in->version >= 13u) ? (int)in->reduce_motion : 0);
  gtav_native_bridge_set_nav_delay_index((in->version >= 13u) ? in->nav_delay_index : 1u);
  gtav_native_bridge_set_nav_speed_index((in->version >= 13u) ? in->nav_speed_index : 1u);
  // v14: optional touchpad input. Pre-v14 (memset-0) loads as off, matching the default-off policy.
  gtav_native_bridge_set_touchpad_enabled((in->version >= 14u) ? (int)in->touchpad_enabled : 0);
  // v16: menu panel width. Pre-v16 version-gates to Normal (index 1), not the memset-0 Narrow.
  gtav_native_bridge_set_panel_width_index((in->version >= 16u) ? in->panel_width_index : 1u);
  // Browser favorites/recents: restore into the bridge (it rebuilds the browsers so the markers
  // and synthetic filter views reflect the loaded lists). Pre-v7 files have these zeroed by the
  // memset-0 loader, so older profiles simply load with empty favorites/recents.
  gtav_native_bridge_set_favorites(in->favorite_vehicles, in->favorite_peds);
  gtav_native_bridge_set_recents(in->recent_vehicles, in->recent_peds);
  // Global Quick pins: restore into the bridge (it rebuilds the Quick menu). Pre-v10 files have
  // these zeroed by the memset-0 loader, so older profiles simply load with no pins.
  gtav_native_bridge_set_quick(in->quick_action, in->quick_param);
  // Wardrobe: load the live editor working set + the saved outfit slots. NOT auto-applied here
  // (this runs at init via disable_all -> import_profile, and dressing the ped needs a valid player
  // ped); the user presses Apply Outfit. The arrays are bounds-safe -- each value is clamped
  // against the live slot's variation count when applied. Pre-v16 files have no saved_outfit_*
  // keys, so the memset-0 loader leaves the slots empty and the single pre-v16 outfit stays in the
  // live editor.
  memcpy(g_wardrobe_drawable, in->wardrobe_drawable, sizeof(g_wardrobe_drawable));
  memcpy(g_wardrobe_texture, in->wardrobe_texture, sizeof(g_wardrobe_texture));
  memcpy(g_saved_outfit_drawable, in->saved_outfit_drawable, sizeof(g_saved_outfit_drawable));
  memcpy(g_saved_outfit_texture, in->saved_outfit_texture, sizeof(g_saved_outfit_texture));
  // Saved vehicle builds: restore so Spawn Saved Vehicle works after a restart. Pre-v16 single
  // vehicle migrates into garage slot 0 (handled in feature_profile.c's key parser).
  memcpy(g_saved_vehicle_model, in->saved_vehicle_model, sizeof(g_saved_vehicle_model));
  memcpy(g_saved_vehicle_mods, in->saved_vehicle_mods, sizeof(g_saved_vehicle_mods));
  // Keybinds: accept a slot's combo only if its mask exactly matches a curated combo AND the
  // stored action matches the slot's fixed feature; anything else (corrupt/hand-edited) falls
  // back to Off. This clamps the /data trust boundary the same way the cycler indices are.
  for (uint32_t i = 0; i < GTAV_KEYBIND_SLOTS; ++i) {
    uint32_t idx = 0u;  // Off
    for (uint32_t c = 1u; c < kKeybindComboCount; ++c) {
      if (kKeybindCombos[c].mask == in->keybind_mask[i] &&
          in->keybind_action[i] == kBindableActions[i].action) {
        idx = c;
        break;
      }
    }
    g_keybind_combo[i] = idx;
  }
  apply_keybinds();

  // Re-activate every saved toggle so live state and the menu's displayed state both match
  // what was saved. Generated from the shared toggle list (feature_toggles.def) so the
  // restore set can never fall out of sync with gtav_features_toggle_mask() -- every bit
  // that can be saved is restored here. The HUD sub-elements carry reactivate = 0: they are
  // already written directly from the saved mask above (re-activating would flip them).
  static const struct {
    uint64_t bit;
    uint32_t action;
    int reactivate;
  } kRestoreToggles[] = {
#define GTAV_TOGGLE(name, state, reactivate, default_on) \
  {GTAV_FEATURE_TOGGLE_##name, GTAV_NATIVE_SHELL_ACTION_TOGGLE_##name, reactivate},
#include "gtavmenu/feature_toggles.def"
  };
  for (uint32_t i = 0; i < (uint32_t)(sizeof(kRestoreToggles) / sizeof(kRestoreToggles[0])); ++i) {
    if (kRestoreToggles[i].reactivate && (in->toggle_mask & kRestoreToggles[i].bit)) {
      gtav_features_activate(kRestoreToggles[i].action);
    }
  }
}

extern "C" int gtav_features_profile_save_default(void) {
  GtavFeatureProfile profile;
  gtav_features_export_profile(&profile);
  return gtav_feature_profile_save(GTAV_FEATURE_PROFILE_DEFAULT_PATH, &profile);
}

extern "C" int gtav_features_profile_load_default(void) {
  GtavFeatureProfile profile;
  if (gtav_feature_profile_load(GTAV_FEATURE_PROFILE_DEFAULT_PATH, &profile) != 0) {
    return -1;
  }
  gtav_features_import_profile(&profile);
  return 0;
}

// GTA V vehicle class (0..21) for catalog entry `index`, via
// GET_VEHICLE_CLASS_FROM_NAME(GET_HASH_KEY(model)). The menu calls this while filtering
// the spawner list, so repeat reads must be cheap (a plain cache read).
//
// Build thread: with the frame hook live the cache is computed on the GAME/script thread
// (warm_vehicle_class_cache_game_thread, reached from reassert_toggles_game_thread's
// first valid-context frame) because GET_VEHICLE_CLASS_FROM_NAME dereferences internal
// script context and faults from the scePad worker on 01.010.002. Only in the worker-only
// lane (no frame hook, so there is no game-thread warmer) does the worker build it
// directly, where the pure hash lookup is proven safe. Returns -1 until warmed -- callers
// treat -1 as "unknown" (shown under no class).
// Sized to cover the vehicle catalog (mirrors native_bridge.cpp's spawn-row cap).
#ifndef GTAV_MENU_VEHICLE_ROWS_MAX
#define GTAV_MENU_VEHICLE_ROWS_MAX 1024u
#endif
static int8_t g_vehicle_class_cache[GTAV_MENU_VEHICLE_ROWS_MAX];
static int g_vehicle_class_cached;

static void build_vehicle_class_cache() {
  uint32_t n = gtav_vehicle_catalog_count;
  const uint32_t cap = (uint32_t)(sizeof(g_vehicle_class_cache) / sizeof(g_vehicle_class_cache[0]));
  if (n > cap) n = cap;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t h = gtav_vehicle_catalog[i].model_hash;
    int c = invoke_return<int>(g_n.get_vehicle_class_from_name, h);
    g_vehicle_class_cache[i] = (int8_t)((c >= 0 && c <= 31) ? c : -1);
  }
  g_vehicle_class_cached = 1;
}

// Game-thread warmer: reached from the frame-hook tick's first valid-context frame.
// No-op after the first build (a single volatile read per tick). The worker never calls it
// (a worker GET_VEHICLE_CLASS_FROM_NAME dereferences a null script context on 01.010.002).
#if GTAV_MENU_ENABLE_FRAME_HOOK
static void warm_vehicle_class_cache_game_thread() {
  if (g_vehicle_class_cached) return;
  if (!g_n.get_hash_key || !g_n.get_vehicle_class_from_name) return;
  build_vehicle_class_cache();
}
#endif

extern "C" int gtav_features_vehicle_class(uint32_t index) {
  if (index >= gtav_vehicle_catalog_count) return -1;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  // Frame-hook build: ONLY the game-thread warmer may build the cache. A worker-direct
  // GET_VEHICLE_CLASS_FROM_NAME on 01.010.002 dereferences a null script context, so the
  // worker never runs the native here; until the warmer lands the filter reads -1 ("All").
  if (!g_vehicle_class_cached) return -1;
#else
  // Worker-only lane: no frame hook -> no game-thread warmer -> the worker builds the
  // cache itself (the getter is a pure hash lookup, proven safe on this lane).
  if (!g_vehicle_class_cached) {
    if (!g_n.get_hash_key || !g_n.get_vehicle_class_from_name) return -1;
    build_vehicle_class_cache();
  }
#endif
  if (index >= (uint32_t)(sizeof(g_vehicle_class_cache) / sizeof(g_vehicle_class_cache[0])))
    return -1;
  return g_vehicle_class_cache[index];
}

#include "features/teleport.inc"
static void load_feature_natives() {
  memset(&g_n, 0, sizeof(g_n));
#if GTAV_MENU_ENABLE_NATIVE_FEATURES
#define GTAV_FEATURE_NATIVE(field, suffix) g_n.field = GTAV_NATIVE_ADDR_##suffix;
#include "gtavmenu/feature_natives.def"
#undef GTAV_FEATURE_NATIVE
  // Build-gated natives (see feature_natives.def): bound only when the build flag and the
  // resolved address are present; left 0 by the memset above otherwise.
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW
#ifdef GTAV_NATIVE_ADDR_REQUEST_STREAMED_TEXTURE_DICT
  g_n.request_streamed_texture_dict = GTAV_NATIVE_ADDR_REQUEST_STREAMED_TEXTURE_DICT;
  g_n.has_streamed_texture_dict_loaded = GTAV_NATIVE_ADDR_HAS_STREAMED_TEXTURE_DICT_LOADED;
  g_n.set_streamed_texture_dict_as_no_longer_needed =
      GTAV_NATIVE_ADDR_SET_STREAMED_TEXTURE_DICT_AS_NO_LONGER_NEEDED;
#ifdef GTAV_NATIVE_ADDR_GET_TEXTURE_RESOLUTION
  g_n.get_texture_resolution = GTAV_NATIVE_ADDR_GET_TEXTURE_RESOLUTION;
#endif
#endif
#endif
#if GTAV_MENU_ENABLE_MENU_PED_STREAM_REQUEST
#ifdef GTAV_NATIVE_ADDR_REQUEST_MENU_PED_MODEL
  g_n.request_menu_ped_model = GTAV_NATIVE_ADDR_REQUEST_MENU_PED_MODEL;
#endif
#endif
#if GTAV_MENU_ENABLE_STREAM_FORCE_LOAD
#ifdef GTAV_NATIVE_ADDR_LOAD_ALL_OBJECTS_NOW
  g_n.load_all_objects_now = GTAV_NATIVE_ADDR_LOAD_ALL_OBJECTS_NOW;
#endif
#endif
#if GTAV_MENU_ENABLE_PROLOGUE_SKIP
#ifdef GTAV_NATIVE_ADDR_TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME
  g_n.terminate_all_scripts_with_this_name = GTAV_NATIVE_ADDR_TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME;
#endif
#endif
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM
#ifdef GTAV_NATIVE_ADDR_REQUEST_SCALEFORM_MOVIE
  g_n.request_scaleform_movie = GTAV_NATIVE_ADDR_REQUEST_SCALEFORM_MOVIE;
  g_n.has_scaleform_movie_loaded = GTAV_NATIVE_ADDR_HAS_SCALEFORM_MOVIE_LOADED;
  g_n.begin_scaleform_movie_method = GTAV_NATIVE_ADDR_BEGIN_SCALEFORM_MOVIE_METHOD;
  g_n.end_scaleform_movie_method = GTAV_NATIVE_ADDR_END_SCALEFORM_MOVIE_METHOD;
  g_n.scaleform_movie_method_add_param_int = GTAV_NATIVE_ADDR_SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT;
  g_n.scaleform_movie_method_add_param_player_name_string =
      GTAV_NATIVE_ADDR_SCALEFORM_MOVIE_METHOD_ADD_PARAM_PLAYER_NAME_STRING;
  g_n.begin_text_command_scaleform_string = GTAV_NATIVE_ADDR_BEGIN_TEXT_COMMAND_SCALEFORM_STRING;
  g_n.end_text_command_scaleform_string = GTAV_NATIVE_ADDR_END_TEXT_COMMAND_SCALEFORM_STRING;
  g_n.add_text_component_substring_player_name =
      GTAV_NATIVE_ADDR_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME;
  g_n.draw_scaleform_movie_fullscreen = GTAV_NATIVE_ADDR_DRAW_SCALEFORM_MOVIE_FULLSCREEN;
#if GTAV_MENU_RENDER_DIAGNOSTICS
  gtav_render_diag_bind(GTAV_RD_SCALEFORM, g_n.draw_scaleform_movie_fullscreen);
  gtav_render_diag_bind(GTAV_RD_TEXT_BEGIN, g_n.begin_text_command_scaleform_string);
  gtav_render_diag_bind(GTAV_RD_TEXT_END, g_n.end_text_command_scaleform_string);
#endif
  g_n.set_scaleform_movie_as_no_longer_needed =
      GTAV_NATIVE_ADDR_SET_SCALEFORM_MOVIE_AS_NO_LONGER_NEEDED;
  g_n.get_control_instructional_buttons_string =
      GTAV_NATIVE_ADDR_GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING;
#endif
#endif
#endif
}

}  // namespace

#if GTAV_MENU_ENABLE_FRAME_HOOK
extern "C" void gtav_features_run_job(uint32_t action, uint32_t param, void* ctx);
#endif

// Telemetry drives log verbosity: ON => DEBUG (verbose tracing reaches the worker log ring /
// loader file), OFF => INFO (light but useful). Called at init and on every telemetry flip.
// Session-only: telemetry resets to off each inject, so verbosity starts light.
static void apply_telemetry_log_level(void) {
  gtav_log_set_level(g_state.telemetry_enabled ? GTAV_LOG_DEBUG : GTAV_LOG_INFO);
}

extern "C" void gtav_features_init(const GtavNativeAddressTable* table) {
  (void)table;  // Feature addresses come from the pinned generated table.
  memset(&g_state, 0, sizeof(g_state));
  apply_telemetry_log_level();  // telemetry just reset to off => INFO baseline
  g_last_spawned_vehicle = 0;
  g_last_spawned_object = 0;
  // Interactive object move mode is transient (not in feature_toggles.def), so reset it here.
  g_object_move_mode_enabled = 0;
  g_object_move_entity = 0;
  g_object_move_vis_request = 0;
  g_object_move_aim_mode = 0;
  g_object_move_surface_snap = 0;
  g_object_move_sel_index = -1;
  g_object_move_aim_dist = 3.0f;
  memset(g_spawned_vehicle_roster, 0, sizeof(g_spawned_vehicle_roster));
  memset(g_spawned_ped_roster, 0, sizeof(g_spawned_ped_roster));
  memset(g_spawned_object_roster, 0, sizeof(g_spawned_object_roster));
  memset(g_spawned_object_model, 0, sizeof(g_spawned_object_model));
  memset(g_bodyguard_roster, 0, sizeof(g_bodyguard_roster));
  g_spawned_vehicle_count = 0;
  g_spawned_ped_count = 0;
  g_spawned_object_count = 0;
  g_bodyguard_count = 0;
  // Reset every display/persistence toggle to its default from the shared list, so a new
  // toggle can never be left out of init (the speed/coords/fps HUD elements default on).
#define GTAV_TOGGLE(name, state, reactivate, default_on) state = default_on;
#include "gtavmenu/feature_toggles.def"
  g_saved_location_valid = 0;
  g_weather_index = 0;
  g_time_index = 0;
  g_wanted_index = 0;
  g_nitro_power_index = 1u;
  g_timescale_index = 0;
  g_gravity_index = 0;
  g_clock_hour = 12u;
  g_move_rate_index = 1u;
  g_bodyguard_health_index = 1u;
  g_bodyguard_armor_index = 1u;
  g_bodyguard_formation_index = 0u;
  g_bodyguard_aggression_index = 0u;
  g_bodyguard_weapon_index = 0u;
  g_bodyguard_accuracy_index = 2u;
  g_bodyguard_blips_enabled = 0;
  g_bodyguard_combat_dirty = 0;
  g_cruise_speed_index = 2u;
  g_paint_primary_index = 0;
  g_paint_secondary_index = 0;
  g_window_tint_index = 0;
  g_neon_index = 0;
  g_tyre_smoke_index = 0;
  g_wheel_type_index = 0;
  g_livery_stage = -1;
  g_plate_style_index = 0;
  g_plate_text_index = 0;
  g_pearl_color_index = 0;
  g_wheel_color_index = 0;
  g_custom_primary_index = 0;
  g_custom_secondary_index = 0;
  // The toggle flags themselves were reset by the feature_toggles.def loop above; these are
  // the non-toggle bits of animation/selection state that ride alongside them.
  g_rainbow_neon_phase = 0;
  g_weapon_damage_index = 0;
  g_rainbow_paint_phase = 0;
  g_noclip_entity = 0;
  // Fly modes are volatile mode state (not toggle bits), so the feature_toggles.def reset loop
  // doesn't clear them -- do it here so "Disable All Features" / profile load lands fly off.
  g_fly_mode = 0;
  g_vehicle_fly_mode = 0;
  g_active_gun = 0;
  g_wind_index = 0;
  g_cam_shake_index = 0;
  // Free camera is a transient mode (not a saved toggle); land it off on disable-all / profile
  // load.
  g_free_cam_active = 0;
  g_free_cam_vis_request = 0;
  // The g_minigame_on[] display flags were cleared by the feature_toggles.def loop above;
  // also reset the authoritative active id (it is not a toggle row).
  g_active_minigame = GTAV_MINIGAME_NONE;
  for (uint32_t i = 0; i < GTAV_NUM_MOD_SLOTS; ++i) g_mod_stage[i] = -1;
  g_mod_stage_ready = 1;
  g_tp_active = 0;
  g_state.abi_version = GTAV_FEATURES_ABI_VERSION;
  load_feature_natives();
  g_state.native_ready = has_core_player_natives() ? 1u : 0u;
  set_message(g_state.native_ready ? "native table ready" : "native table unavailable");
#if !GTAV_FEATURES_SKIP_PROFILE_LOAD
  // Restore the saved profile (toggles + cycler selections) so a setup survives a
  // restart. Non-fatal: a missing/unreadable file just leaves defaults in place.
  gtav_features_profile_load_default();
#endif
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "features initialized native_ready=%u",
                     g_state.native_ready);
}

// Arm the main-thread execution consumer so gated entity-creating actions can run in a valid
// game-thread context. Split out of gtav_features_init so probe/calibration lanes (native
// features off) still wire the hook: a failure here is non-fatal — the actions simply stay
// gated (unavailable) instead of crashing — and the frame-hook context probe still fires.
extern "C" void gtav_features_init_frame_hook(void) {
#if GTAV_MENU_ENABLE_FRAME_HOOK && !GTAV_FEATURES_SKIP_FRAME_HOOK_SETUP
#if GTAV_MENU_HANDLER_SLOT_JOB_DRAIN
  // Handler-slot mode: a native registration table qword points at our wrapper.
  // The wrapper calls the original handler normally, then drains this queue in the
  // same script/native context. No executable prologue patch or broker is used.
  const int hook_rc = gtav_frame_hook_prepare_jobs(gtav_features_run_job, nullptr, 0);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "handler slot job drain rc=%d active=%d",
                     hook_rc, gtav_frame_hook_is_active());
#elif GTAV_FRAME_HOOK_EXTERNAL_INSTALL
  // External mode: we set up the thunk + gateway but do NOT patch .text in-process
  // (PS5 faults on that). Emit the thunk address so an external kernel-write agent
  // (ps5debug) can jump the target here. The hook only fires once that write lands.
  const int hook_rc = gtav_frame_hook_install_external(gtav_features_run_job, nullptr, 0);
  uint32_t broker_expected_len = 0;
  const uint8_t* broker_expected = gtav_frame_hook_expected_bytes(&broker_expected_len);
  if (hook_rc == 0) {
    gtav_patch_broker_publish_frame_hook(
        (uint64_t)GTAV_MENU_FRAME_HOOK_TARGET, gtav_frame_hook_patch_len(),
        gtav_frame_hook_stolen_len(), broker_expected, broker_expected_len,
        (uint64_t)gtav_frame_hook_thunk_address(), (uint64_t)gtav_frame_hook_gateway_address(),
        (uint64_t)gtav_frame_hook_gateway_continuation(), GTAV_MENU_DEFAULT_TARGET_ID);
  } else {
    gtav_patch_broker_set_state(GTAV_PATCH_BROKER_STATE_ERROR,
                                GTAV_PATCH_BROKER_ERROR_INVALID_REQUEST,
                                "frame hook external setup failed");
  }
  gtav_status_eventf(
      GTAV_MENU_EVENT_NATIVE_BRIDGE,
      "frame hook external rc=%d active=%d thunk=0x%llx target=0x%llx broker=%u", hook_rc,
      gtav_frame_hook_is_active(), (unsigned long long)gtav_frame_hook_thunk_address(),
      (unsigned long long)GTAV_MENU_FRAME_HOOK_TARGET, gtav_patch_broker_state.state);
#elif GTAV_FRAME_HOOK_REPLACE
  // Legacy in-process REPLACE install (prologue-replace model). The PS5
  // DOES_CAM_EXIST prologue-replace lane was live-confirmed unsafe on 2026-06-14;
  // keep this path only for explicit diagnostics.
  const int hook_rc = gtav_frame_hook_install_replace((uintptr_t)GTAV_MENU_FRAME_HOOK_TARGET,
                                                      gtav_features_run_job, nullptr, 0);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                     "frame hook replace install target=0x%llx rc=%d active=%d thunk=0x%llx",
                     (unsigned long long)GTAV_MENU_FRAME_HOOK_TARGET, hook_rc,
                     gtav_frame_hook_is_active(),
                     (unsigned long long)gtav_frame_hook_thunk_address());
#else
  const int hook_rc = gtav_frame_hook_install((uintptr_t)GTAV_MENU_FRAME_HOOK_TARGET,
                                              gtav_features_run_job, nullptr, 0);
  gtav_status_eventf(
      GTAV_MENU_EVENT_NATIVE_BRIDGE, "frame hook install target=0x%llx rc=%d active=%d",
      (unsigned long long)GTAV_MENU_FRAME_HOOK_TARGET, hook_rc, gtav_frame_hook_is_active());
#endif
  // Register the per-frame effect tick (explosive/fire ammo), independent of install mode.
  // It runs on the game/script thread each fire once the hook is live; the effects stay
  // inert until their toggle is enabled.
  gtav_frame_hook_set_tick_fn(gtav_features_game_thread_tick);
#endif
}

extern "C" void gtav_features_shutdown(void) {
  g_menu_open = 0;
  disable_all_features();
#if GTAV_MENU_ENABLE_FRAME_HOOK
  gtav_frame_hook_clear_jobs();
#endif
  gtav_status_event(GTAV_MENU_EVENT_SHUTDOWN, "features shutdown complete");
}

extern "C" uint32_t gtav_features_activate(uint32_t action) {
  return gtav_features_activate_param(action, 0);
}

// Actions whose natives create entities or drive model streaming
// (CREATE_VEHICLE, GIVE_WEAPON_TO_PED) require the game's main/script thread and
// crash when run from the scePad worker. Until a script-context consumer
// (currently the native-handler table slot wrapper) is active, gate them off so
// the live menu stays crash-free. Per-frame flag/field setters (god mode, heal, wanted,
// multipliers) do not ALLOCATE, but they still resolve + walk live engine entities, so they
// re-assert on the game-thread frame-hook tick (reassert_toggles_game_thread), NOT
// worker-direct -- running them off the game thread raced the entity lifecycle and crashed
// intermittently during state transitions.
#ifndef GTAV_MENU_GATE_MAINTHREAD_ACTIONS
#define GTAV_MENU_GATE_MAINTHREAD_ACTIONS 0
#endif

#ifndef GTAV_MENU_ALLOW_WORKER_VEHICLE_SPAWN
#define GTAV_MENU_ALLOW_WORKER_VEHICLE_SPAWN 0
#endif

// Historical research builds could expose actions that had not passed target gates. Production
// fixes this to zero; the retained source gate keeps those rows visibly locked.
#ifndef GTAV_MENU_UNLOCK_ALL_ACTIONS
#define GTAV_MENU_UNLOCK_ALL_ACTIONS 0
#endif

[[maybe_unused]] static int action_needs_main_thread(uint32_t action) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS:
    // A single weapon give goes through the same ped weapon manager as GIVE_WEAPONS,
    // so it must run on the game thread too.
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON:
    // REMOVE_ALL_PED_WEAPONS manipulates the ped weapon manager (vtable dispatch +
    // weapon-object teardown), which crashes off the game/script thread just like
    // GIVE_WEAPON_TO_PED. Gate it to the main-thread lane instead of crashing.
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS:
    // SET_PLAYER_MODEL deletes and recreates the player ped (allocation-class), which
    // crashes off the game thread just like CREATE_VEHICLE. Route it to the game thread.
    case GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL:
    // ADD_OWNED_EXPLOSION creates an entity, so the spawn-explosion actions must run on
    // the game thread (like the explosive-ammo tick). FORCE_RAGDOLL mutates the ped task
    // tree, which is likewise unsafe off the script thread.
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER:
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT:
    case GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL:
    // Ped/object spawn allocate entities (like CREATE_VEHICLE). Ped-control enumerates the
    // script ped pool + mutates ped task trees. All must run on the game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_PED:
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT:
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD:
    case GTAV_NATIVE_SHELL_ACTION_PEDS_ATTACK_PLAYER:
    case GTAV_NATIVE_SHELL_ACTION_PEDS_FLEE_PLAYER:
    case GTAV_NATIVE_SHELL_ACTION_PEDS_STOP:
    // Scenarios mutate the player's ped task tree (TASK_START_SCENARIO_IN_PLACE / CLEAR_PED_TASKS),
    // not safe on the worker -> game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_START_SCENARIO:
    case GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO:
    // Ragdoll-nearby enumerates the ped pool + mutates ped tasks, same as the other
    // ped-control actions -> game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_RAGDOLL_NEARBY:
    // Give Max Ammo walks the ped weapon manager (SET_PED_AMMO), same class as GIVE_WEAPON ->
    // game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO:
    // Weapon attachments give/remove components via the ped weapon manager (GIVE/REMOVE_WEAPON_
    // COMPONENT_*), the same game-thread-only class as GIVE_WEAPON_TO_PED.
    case GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS:
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS:
    // DELETE_ENTITY tears down a vehicle entity (allocation-class, like CREATE_VEHICLE), so it
    // must run on the game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL:
    case GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS:
    case GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS:
    // SET_VEHICLE_FIXED is a damage/deformation/fragment rebuild that deadlocks the console
    // off the worker thread -- the per-tick Keep-Repaired path already gates that exact native
    // to the frame-hook lane (see tick_vehicle_toggles). The one-shot Fix / Repair+Clean
    // actions and the Driver preset (which fixes the current car) call it too, so they marshal
    // onto the game thread for the same reason instead of running it worker-direct.
    case GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER:
    // Skip-prologue relaunches the SP session (SHUTDOWN_AND_LAUNCH_SINGLE_PLAYER_GAME),
    // which rebuilds the script context -- never safe off the game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_SKIP_PROLOGUE:
    // Spawn Saved Vehicle creates a vehicle (CREATE_VEHICLE) and re-applies its mods, same
    // allocation-class lane as SPAWN_VEHICLE -> game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE:
    // Spooner load step CREATE_*s an entity at a saved transform (allocation-class). It is enqueued
    // directly by load_map(), but list it here too so the gating model stays honest.
    case GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP:
    // The engine device mount uses the game thread's allocator and file-system lock.
    case GTAV_NATIVE_SHELL_ACTION_MOUNT_CUSTOM_DEVICE:
    // Fireworks streams a named PTFX asset (allocation/streaming class, like REQUEST_MODEL) then
    // starts the fx -> game/script thread, with non-blocking self-requeue while the asset loads.
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS:
    // Teleport Into Last Vehicle walks the ped/vehicle managers (GET_LAST_DRIVEN_VEHICLE +
    // SET_PED_INTO_VEHICLE) -> game/script thread.
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_LAST_VEHICLE:
    // Rocket Boost applies an impulse to the live CVehicle (APPLY_FORCE_TO_ENTITY) -> game thread.
    case GTAV_NATIVE_SHELL_ACTION_VEHICLE_ROCKET_BOOST:
    // Spooner entity ops: Duplicate re-spawns the model (CREATE_*); Attach/Detach walk the entity
    // manager (ATTACH_ENTITY_TO_ENTITY / DETACH_ENTITY) -> game thread. The alpha cycler is
    // worker-safe (modify-existing field store) and is NOT listed here.
    case GTAV_NATIVE_SHELL_ACTION_DUPLICATE_LAST_ENTITY:
    case GTAV_NATIVE_SHELL_ACTION_ATTACH_LAST_ENTITY:
    case GTAV_NATIVE_SHELL_ACTION_DETACH_LAST_ENTITY:
      return 1;
    // Teleport (waypoint + presets) must run on the game/script thread. The
    // streaming-aware start_teleport() freezes the entity and SET_ENTITY_COORDS /
    // FREEZE_ENTITY_POSITION fault from the scePad worker on 01.010.002 (and
    // likely other builds) because they resolve internal script context; the poll
    // half already lives in tick_player_toggles() which runs on the game thread
    // when the frame hook is active, so gating the start keeps both halves on the
    // correct thread.
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT:
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE:
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET:
    // Return To Saved Location goes through the same start_teleport() as the presets
    // (FREEZE_ENTITY_POSITION + SET_ENTITY_COORDS fault from the scePad worker on
    // 01.010.002), so it must ride the game/script-thread lane too.
    case GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION:
      return 1;
    default:
      return 0;
  }
}

// Exported single source of truth for "this action must run on the game/script
// thread" (entity creation, weapon manager teardown). Build-flag independent so a
// caller can decide which actions to marshal onto the game thread vs run worker-direct.
extern "C" int gtav_features_action_needs_game_thread(uint32_t action) {
  return action_needs_main_thread(action);
}

// UI lock predicate: is this row unavailable / refused if activated? Distinct from
// needs_game_thread() above -- a game-thread action drains via the frame hook in valid
// script context, so it reports ungated once the hook is live (otherwise activation would
// refuse before reaching the dispatch).
extern "C" int gtav_features_action_is_gated(uint32_t action) {
  // Every game-thread action (vehicle spawn, weapons, skin) drains via the frame hook in
  // valid script context, so each row is "runnable once the hook is live" and unlocks then
  // -- hardware-confirmed for spawn, weapons and skin on the always-firing PLAYER_PED_ID
  // pump. The dev GTAV_MENU_UNLOCK_ALL_ACTIONS / --all flag is now a no-op (kept for
  // back-compat) since these rows unlock by default.
  int hook_runnable = action_needs_main_thread(action);
  if (hook_runnable) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    return !gtav_frame_hook_is_active();
#else
    return 1;
#endif
  }
#if GTAV_MENU_GATE_MAINTHREAD_ACTIONS
  return action_needs_main_thread(action);
#else
  (void)action;
  return 0;
#endif
}

// True when a Save action would overwrite an already-occupied slot, so the menu can arm its
// one-press confirm only then (saving to an empty slot stays immediate). Reads worker-safe state.
extern "C" int gtav_features_save_slot_occupied(uint32_t action) {
  if (action == GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE) return saved_vehicle_slot_occupied();
  if (action == GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT) return saved_outfit_slot_occupied();
  return 0;
}

// Runs a gated action's real natives. Reached on the game's main/script thread
// (from a hook/handler-slot consumer) so CREATE_VEHICLE / GIVE_WEAPON_TO_PED
// execute in a valid context, or directly when main-thread gating is disabled.
[[maybe_unused]] static uint32_t run_gated_action(uint32_t action, uint32_t param) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE:
      // Queue contract: param is already the model joaat hash. Streaming and
      // CREATE_VEHICLE both run from this game-thread job, with non-blocking
      // requeue while the model is pending.
      gtav_features_run_spawn_hash_job(action, param, nullptr);
      return g_state.last_result;
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE:
      return spawn_saved_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_LOAD_MAP_STEP:
      return load_map_step(param);
    case GTAV_NATIVE_SHELL_ACTION_MOUNT_CUSTOM_DEVICE:
      return mount_custom_device();
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS:
      return spawn_fireworks();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_LAST_VEHICLE:
      return teleport_last_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT:
      return teleport_to_waypoint();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE:
      return teleport_to_objective();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET:
      return teleport_preset(param);
    case GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION:
      return return_saved_location();
    case GTAV_NATIVE_SHELL_ACTION_VEHICLE_ROCKET_BOOST:
      return vehicle_rocket_boost();
    case GTAV_NATIVE_SHELL_ACTION_DUPLICATE_LAST_ENTITY:
      return duplicate_last_entity();
    case GTAV_NATIVE_SHELL_ACTION_ATTACH_LAST_ENTITY:
      return attach_last_entity();
    case GTAV_NATIVE_SHELL_ACTION_DETACH_LAST_ENTITY:
      return detach_last_entity();
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS:
      return give_weapons();
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON:
      return give_weapon_single(param);
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS:
      return remove_weapons();
    case GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS:
      return apply_weapon_attachments();
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS:
      return remove_weapon_attachments();
    case GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL:
      // Like SPAWN_VEHICLE: param is the model joaat hash; streaming + SET_PLAYER_MODEL
      // run from this game-thread job, with non-blocking requeue while the model loads.
      gtav_features_run_skin_hash_job(action, param, nullptr);
      return g_state.last_result;
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER:
      return spawn_explosion_at_player();
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT:
      return spawn_explosion_at_waypoint();
    case GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL:
      return force_ragdoll();
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_PED:
      run_entity_spawn_job(action, param, /*is_ped=*/1);
      return g_state.last_result;
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT:
      run_entity_spawn_job(action, param, /*is_ped=*/0);
      return g_state.last_result;
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD:
      run_bodyguard_spawn_job(param);
      return g_state.last_result;
    case GTAV_NATIVE_SHELL_ACTION_PREWARM_MODEL:
      // Silent background accelerator: warm a model into the streamer on the game thread so a
      // confirmed spawn lands on its first fire. Does not touch g_state / the toast.
      run_prewarm_model_job(param);
      return GTAV_FEATURE_RESULT_NONE;
    case GTAV_NATIVE_SHELL_ACTION_PREVIEW_TXD:
      // Silent background helper: (re)stream the highlighted vehicle's preview texture dict on
      // the game thread so the renderer can submit its thumbnail. Does not touch g_state.
      run_preview_txd_job();
      return GTAV_FEATURE_RESULT_NONE;
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE:
      return apply_timecycle();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX:
      return apply_animpostfx();
    case GTAV_NATIVE_SHELL_ACTION_PEDS_ATTACK_PLAYER:
      return peds_attack_player();
    case GTAV_NATIVE_SHELL_ACTION_PEDS_FLEE_PLAYER:
      return peds_flee_player();
    case GTAV_NATIVE_SHELL_ACTION_PEDS_STOP:
      return peds_stop();
    case GTAV_NATIVE_SHELL_ACTION_START_SCENARIO:
      // Queue contract: param is the gtav_scenario_catalog index of the chosen scenario.
      return start_scenario(param);
    case GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO:
      return stop_scenario();
    case GTAV_NATIVE_SHELL_ACTION_RAGDOLL_NEARBY:
      return ragdoll_nearby_peds();
    case GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE:
      return delete_current_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO:
      return give_max_ammo();
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES:
      return clear_spawned_vehicles();
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS:
      return clear_spawned_peds();
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS:
      return clear_spawned_objects();
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL:
      return clear_spawned_all_with_bodyguards();
    case GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS:
      return bring_bodyguards();
    case GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS:
      return dismiss_bodyguards();
    case GTAV_NATIVE_SHELL_ACTION_SKIP_PROLOGUE:
      return skip_prologue();
    // Vehicle Fix / Repair+Clean and the Driver preset run SET_VEHICLE_FIXED, which deadlocks
    // off the worker thread -- drained here so the rebuild runs in valid script context.
    case GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE:
      return fix_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE:
      return repair_clean_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER:
      return preset_driver();
    default:
      return unavailable(action, "unsupported gated action");
  }
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Consumer callback for the frame-hook job queue (main thread).
extern "C" void gtav_features_run_job(uint32_t action, uint32_t param, void* ctx) {
  (void)ctx;
  run_gated_action(action, param);
}
#endif

// Canonical list of every feature action: the switch below maps each
// GTAV_NATIVE_SHELL_ACTION_* to its handler. Adding a feature means adding a
// case here (plus the action enum and a menu row in native_bridge.cpp).
// Marshal a game-thread-only action onto the game thread via the live frame hook: when the
// hook is active, enqueue the job (ok) or report the queue full (failed); when the hook is
// not live -- or this build has no frame hook -- the action is unavailable. Collapses the
// three identical enqueue-or-fail sites below (spawn vehicle, spawn ped/object, and the
// gated main-thread actions). param is only consulted under the frame-hook build.
static uint32_t enqueue_game_thread_action(uint32_t action, [[maybe_unused]] uint32_t param) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (gtav_frame_hook_is_active()) {
    if (gtav_frame_hook_enqueue(action, param)) {
      return ok(action, "queued for main thread");
    }
    return failed(action, "main-thread job queue full");
  }
#endif
  return unavailable(action, "needs main-thread hook (not yet enabled)");
}

// Worker-direct dispatch table (defined below activate_param so the gating prelude reads first).
static uint32_t activate_worker_direct_action(uint32_t action, uint32_t param);

extern "C" uint32_t gtav_features_activate_param(uint32_t action, uint32_t param) {
  g_state.activation_count++;
  g_state.last_param = param;
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  // Publish intent only: no GTA memory is touched from this worker-side path. The
  // verified game-thread tick consumes it before the vehicle job reaches CREATE_VEHICLE.
  if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE) {
    gtav_script_globals_arm();
  }
#endif
  // Pre-warm at QUEUE time so an instant select starts model streaming now instead of at
  // the first game-thread hook fire (collapses the spawn requeue delay). Non-blocking and
  // a no-op unless built with vehicle preload; only model-bearing actions carry a hash.
  if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE ||
      action == GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_PED ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD) {
    gtav_features_prewarm_model_hash(param);
  }
  if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
    if (gtav_frame_hook_is_active()) return enqueue_game_thread_action(action, param);
#endif
#if GTAV_MENU_ALLOW_WORKER_VEHICLE_SPAWN
    // Ungated path runs on the worker, where blocking the streaming wait is fine.
    return spawn_vehicle_model_hash(param, /*allow_block=*/1);
#else
    return unavailable(action, "needs main-thread hook (not yet enabled)");
#endif
  }
  // Ped/object spawner: same game-thread, model-streaming lane as SPAWN_VEHICLE.
  if (action == GTAV_NATIVE_SHELL_ACTION_SPAWN_PED ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT ||
      action == GTAV_NATIVE_SHELL_ACTION_SPAWN_BODYGUARD) {
    return enqueue_game_thread_action(action, param);
  }
#if GTAV_MENU_GATE_MAINTHREAD_ACTIONS
  // Hook live: marshal the call onto the game thread instead of refusing.
  if (action_needs_main_thread(action)) {
    return enqueue_game_thread_action(action, param);
  }
#endif
  // Per-slot mod pickers are a contiguous worker-safe range; one handler covers them all.
  if (action >= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST &&
      action <= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_LAST) {
    return cycle_vehicle_mod(action - GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST, param);
  }
  // Everything past the routing prelude runs on the worker thread: hand off to the flat
  // action -> handler table.
  return activate_worker_direct_action(action, param);
}

// Worker-direct dispatch: every action that runs ON the worker thread. The routing prelude in
// gtav_features_activate_param has already peeled off the model-streaming spawns, the
// game-thread-gated actions (action_needs_main_thread), and the contiguous mod-slot range, so
// this is a flat action -> handler map with no thread-routing concerns. Adding a worker-safe
// feature adds a case here (plus the action enum + feature_actions.def name + a menu row).
static uint32_t activate_worker_direct_action(uint32_t action, uint32_t param) {
  switch (action) {
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_TELEMETRY:
      g_state.telemetry_enabled = !g_state.telemetry_enabled;
      apply_telemetry_log_level();
      return ok(action, g_state.telemetry_enabled ? "telemetry enabled" : "telemetry disabled");
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_GOD_MODE:
      return toggle_god_mode();
    case GTAV_NATIVE_SHELL_ACTION_HEAL_ARMOR:
      return heal_armor();
    case GTAV_NATIVE_SHELL_ACTION_CLEAN_SELF:
      return clean_self();
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_WANTED:
      return clear_wanted();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_RUN:
      return toggle_super_run();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_SWIM:
      return toggle_fast_swim();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_AMMO:
      return toggle_infinite_ammo();
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE:
      // Ungated path runs on the worker, where blocking the streaming wait is fine.
#if GTAV_MENU_ALLOW_WORKER_VEHICLE_SPAWN
      return spawn_vehicle_model_hash(param, /*allow_block=*/1);
#else
      return unavailable(action, "needs main-thread hook (not yet enabled)");
#endif
    case GTAV_NATIVE_SHELL_ACTION_SET_PLAYER_MODEL:
      // Reaches here only in a non-gated build; rebuilding the player ped is never
      // safe off the game thread, so refuse rather than run it worker-direct.
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE:
      // Non-gated build: respawning allocates (CREATE_VEHICLE), unsafe off the game thread.
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_FIX_VEHICLE:
      // Reaches here only in a non-gated build; SET_VEHICLE_FIXED deadlocks off the game
      // thread, so refuse rather than run it worker-direct (gated builds enqueue it above).
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_CLEAN_VEHICLE:
      return clean_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPONS:
      return give_weapons();
    case GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON:
      return give_weapon_single(param);
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_WEAPONS:
      return remove_weapons();
    case GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS:
      return apply_weapon_attachments();
    case GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS:
      return remove_weapon_attachments();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_JUMP:
      return toggle_super_jump();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_INVISIBLE:
      return toggle_invisible();
    // List cyclers: param = 1 stage next, 2 stage prev (Left/Right), 0 apply (Select).
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEATHER:
      return cycle_weather(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME:
      return cycle_time(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_FREEZE_TIME:
      return toggle_freeze_time();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WANTED:
      return cycle_wanted(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NITRO_POWER:
      return cycle_nitro_power(param);
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_WAYPOINT:
      return teleport_to_waypoint();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_OBJECTIVE:
      return teleport_to_objective();
    case GTAV_NATIVE_SHELL_ACTION_SAVE_LOCATION:
      return save_location();
    case GTAV_NATIVE_SHELL_ACTION_RETURN_SAVED_LOCATION:
      return return_saved_location();
    case GTAV_NATIVE_SHELL_ACTION_TELEPORT_PRESET:
      return teleport_preset(param);
    case GTAV_NATIVE_SHELL_ACTION_DISABLE_ALL:
      return disable_all_features();
    case GTAV_NATIVE_SHELL_ACTION_PLACE_ON_WHEELS:
      return place_on_wheels();
    case GTAV_NATIVE_SHELL_ACTION_REPAIR_CLEAN_VEHICLE:
      // Non-gated build only; SET_VEHICLE_FIXED is unsafe off the game thread (gated above).
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_LAUNCH_BOOST:
      return launch_boost();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_STEALTH:
      return preset_stealth();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_STUNT:
      return preset_stunt();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_SURVIVAL:
      return preset_survival();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_DRIVER:
      // Driver preset runs SET_VEHICLE_FIXED on the current car, unsafe off the game thread;
      // non-gated build refuses (gated builds enqueue it above). Stealth/Stunt/Survival/Parkour
      // touch only worker-safe field stores and stay worker-direct.
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NEVER_WANTED:
      return toggle_never_wanted();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_VEHICLE_GOD:
      return toggle_vehicle_god();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NITRO:
      return toggle_nitro();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_ENGINE_ON:
      return toggle_engine_on();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_FAST_MOVE:
      return toggle_fast_move();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_LOW_GRAVITY:
      return toggle_low_gravity();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLOW_MOTION:
      return toggle_slow_motion();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_STAMINA:
      return toggle_inf_stamina();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_INF_SPECIAL:
      return toggle_inf_special();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_PROOFS:
      return toggle_super_proofs();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_CRIT:
      return toggle_no_crit();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RAGDOLL:
      return toggle_no_ragdoll();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEATBELT:
      return toggle_seatbelt();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_CLEAN:
      return toggle_keep_clean();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_HEAL:
      return toggle_auto_heal();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_WANTED_LOCK:
      return toggle_wanted_lock();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_KEEP_REPAIRED:
      return toggle_keep_repaired();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_STICK_TO_GROUND:
      return toggle_stick_to_ground();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_DRIFT_MODE:
      return toggle_drift_mode();
    case GTAV_NATIVE_SHELL_ACTION_LS_MAX_PERFORMANCE:
      return ls_max_performance();
    case GTAV_NATIVE_SHELL_ACTION_LS_STOCK_VEHICLE:
      return ls_stock_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_PRIMARY:
      return cycle_paint_primary(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_SECONDARY:
      return cycle_paint_secondary(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WINDOW_TINT:
      return cycle_window_tint(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_NEON:
      return cycle_neon(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TYRE_SMOKE:
      return cycle_tyre_smoke(param);
    case GTAV_NATIVE_SHELL_ACTION_LS_INSTALL_COSMETICS:
      return ls_install_cosmetics();
    case GTAV_NATIVE_SHELL_ACTION_LS_BULLETPROOF_TYRES:
      return ls_bulletproof_tyres();
    case GTAV_NATIVE_SHELL_ACTION_BURST_TYRES:
      return burst_tyres();
    case GTAV_NATIVE_SHELL_ACTION_FIX_TYRES:
      return fix_tyres();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_XENON:
      return toggle_xenon_lights();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_TYPE:
      return cycle_wheel_type(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_LIVERY:
      return cycle_livery(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_STYLE:
      return cycle_plate_style(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_TEXT:
      return cycle_plate_text(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_PEARLESCENT:
      return cycle_pearlescent(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_COLOR:
      return cycle_wheel_color(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_PRIMARY:
      return cycle_custom_primary(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_SECONDARY:
      return cycle_custom_secondary(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_NEON:
      return toggle_rainbow_neon();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_AMMO:
      return toggle_explosive_ammo();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_FIRE_AMMO:
      return toggle_fire_ammo();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_DAMAGE:
      return cycle_weapon_damage(param);
    case GTAV_NATIVE_SHELL_ACTION_KILL_SELF:
      return kill_self();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_PAINT:
      return toggle_rainbow_paint();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NOCLIP:
      return toggle_noclip();
    // Minigame modes: one mutually-exclusive toggle handler covers all of them (it maps the
    // action to an internal id). Like noclip, the per-frame work runs on the game thread, so
    // the handler refuses until the frame hook is live.
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RIOT:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_METEOR:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_STORM:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_INFERNO:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_RAGDOLL:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_BLACKOUT:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_ZOMBIES:
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_MINIGAME_FLOOD:
      return toggle_minigame(action);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_DEBUG_OVERLAY:
      g_debug_overlay_enabled = !g_debug_overlay_enabled;
      return ok(action, g_debug_overlay_enabled ? "debug overlay on" : "debug overlay off");
    case GTAV_NATIVE_SHELL_ACTION_GIVE_MONEY:
      return give_money(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CASH:
      return set_cash(param);
    case GTAV_NATIVE_SHELL_ACTION_MAX_ALL_STATS:
      return max_all_stats();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_INFINITE_PARACHUTES:
      return toggle_infinite_parachutes();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_MAXED:
      return toggle_spawn_maxed();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_INVINCIBLE:
      return toggle_spawn_invincible();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_THIN_POPULATION:
      return toggle_thin_population();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_IGNORED_BY_ALL:
      return toggle_ignored_by_all();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SUPER_BRAKE:
      return toggle_super_brake();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CRUISE_SPEED:
      return cycle_cruise_speed(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_CRUISE_CONTROL:
      return toggle_cruise_control();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_DOOR:
      return cycle_vehicle_door(param);
    case GTAV_NATIVE_SHELL_ACTION_OPEN_VEHICLE_DOOR:
      return open_close_vehicle_door(GTAV_NATIVE_SHELL_ACTION_OPEN_VEHICLE_DOOR, 1);
    case GTAV_NATIVE_SHELL_ACTION_CLOSE_VEHICLE_DOOR:
      return open_close_vehicle_door(GTAV_NATIVE_SHELL_ACTION_CLOSE_VEHICLE_DOOR, 0);
    case GTAV_NATIVE_SHELL_ACTION_ROLL_DOWN_WINDOWS:
      return roll_vehicle_windows(GTAV_NATIVE_SHELL_ACTION_ROLL_DOWN_WINDOWS, 1);
    case GTAV_NATIVE_SHELL_ACTION_ROLL_UP_WINDOWS:
      return roll_vehicle_windows(GTAV_NATIVE_SHELL_ACTION_ROLL_UP_WINDOWS, 0);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_HEADLIGHTS:
      return cycle_vehicle_headlights(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_LOCK:
      return cycle_vehicle_lock(param);
    case GTAV_NATIVE_SHELL_ACTION_RAISE_VEHICLE_ROOF:
      return convertible_roof(GTAV_NATIVE_SHELL_ACTION_RAISE_VEHICLE_ROOF, 1);
    case GTAV_NATIVE_SHELL_ACTION_LOWER_VEHICLE_ROOF:
      return convertible_roof(GTAV_NATIVE_SHELL_ACTION_LOWER_VEHICLE_ROOF, 0);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_SLOT:
      return cycle_wardrobe_slot(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_STYLE:
      return cycle_wardrobe_style(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WARDROBE_TEXTURE:
      return cycle_wardrobe_texture(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_SAVED_VEHICLE_SLOT:
      return cycle_saved_vehicle_slot(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OUTFIT_SLOT:
      return cycle_outfit_slot(param);
    case GTAV_NATIVE_SHELL_ACTION_WARDROBE_RESET:
      return wardrobe_reset();
    case GTAV_NATIVE_SHELL_ACTION_WARDROBE_CLEAR_PROP:
      return wardrobe_clear_prop();
    case GTAV_NATIVE_SHELL_ACTION_SAVE_OUTFIT:
      return save_outfit();
    case GTAV_NATIVE_SHELL_ACTION_APPLY_OUTFIT:
      return apply_outfit();
    case GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE:
      return save_vehicle();
    case GTAV_NATIVE_SHELL_ACTION_PLAY_EMOTE:
      return play_emote(param);
    case GTAV_NATIVE_SHELL_ACTION_STOP_EMOTE:
      return stop_emote();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_EMOTE_LOOP:
      return toggle_emote_loop();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_TINT:
      return cycle_weapon_tint(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SUPP:
      return cycle_weapon_attachment(action, GTAV_WATTACH_SUPP, param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SCOPE:
      return cycle_weapon_attachment(action, GTAV_WATTACH_SCOPE, param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_GRIP:
      return cycle_weapon_attachment(action, GTAV_WATTACH_GRIP, param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_CLIP:
      return cycle_weapon_attachment(action, GTAV_WATTACH_CLIP, param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_FLASH:
      return cycle_weapon_attachment(action, GTAV_WATTACH_FLASH, param);
    case GTAV_NATIVE_SHELL_ACTION_SAVE_MAP:
      return save_map();
    case GTAV_NATIVE_SHELL_ACTION_LOAD_MAP:
      return load_map();
    case GTAV_NATIVE_SHELL_ACTION_CANCEL_MAP_LOAD:
      return cancel_map_load();
    case GTAV_NATIVE_SHELL_ACTION_PROBE_CUSTOM_MOUNT:
      return probe_custom_mount();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE:
      return cycle_autopilot_mode(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION:
      return cycle_autopilot_aggression(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED:
      return cycle_autopilot_speed(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE:
      return cycle_fly_mode(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY:
      return cycle_vehicle_fly(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN:
      return cycle_active_gun(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND:
      return cycle_wind(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE:
      return cycle_cam_shake(param);
    case GTAV_NATIVE_SHELL_ACTION_FREE_CAM:
      return enter_free_cam();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED:
      return cycle_free_cam_speed(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NO_RELOAD:
      return toggle_no_reload();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SLIPPERY:
      return toggle_slippery();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_EXPLOSIVE_MELEE:
      return toggle_explosive_melee();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA:
      return cycle_entity_alpha(param);
    case GTAV_NATIVE_SHELL_ACTION_STOP_AUTOPILOT:
      return stop_autopilot();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_HEALTH:
      return cycle_bodyguard_health(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ARMOR:
      return cycle_bodyguard_armor(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_INVINCIBLE:
      return toggle_bodyguard_invincible();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_FORMATION:
      return cycle_bodyguard_formation(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_AGGRESSION:
      return cycle_bodyguard_aggression(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_WEAPON:
      return cycle_bodyguard_weapon(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_BODYGUARD_ACCURACY:
      return cycle_bodyguard_accuracy(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_BODYGUARD_BLIPS:
      return toggle_bodyguard_blips();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_1:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_2:
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_3:
      return cycle_keybind(action - GTAV_NATIVE_SHELL_ACTION_CYCLE_KEYBIND_0, param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_NIGHT_VISION:
      return toggle_night_vision();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEETHROUGH:
      return toggle_seethrough();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE:
      return cycle_timecycle(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX:
      return cycle_animpostfx(param);
    // Game-thread actions reach here only in a non-gated build; their entity/ped-task
    // natives are never safe worker-direct, so refuse (the gated build enqueues them).
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_PLAYER:
    case GTAV_NATIVE_SHELL_ACTION_SPAWN_EXPLOSION_WAYPOINT:
    case GTAV_NATIVE_SHELL_ACTION_FORCE_RAGDOLL:
    case GTAV_NATIVE_SHELL_ACTION_DELETE_VEHICLE:
    case GTAV_NATIVE_SHELL_ACTION_GIVE_MAX_AMMO:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_VEHICLES:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_PEDS:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_OBJECTS:
    case GTAV_NATIVE_SHELL_ACTION_CLEAR_SPAWNED_ALL:
    case GTAV_NATIVE_SHELL_ACTION_BRING_BODYGUARDS:
    case GTAV_NATIVE_SHELL_ACTION_DISMISS_BODYGUARDS:
    case GTAV_NATIVE_SHELL_ACTION_MOUNT_CUSTOM_DEVICE:
      return unavailable(action, "needs main-thread hook (not yet enabled)");
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_CLOCK_HOUR:
      return cycle_clock_hour(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_TIME_SCALE:
      return cycle_time_scale(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_GRAVITY:
      return cycle_gravity(param);
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD:
      return toggle_hud();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_SPEEDO:
      return toggle_hud_speedo();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_COORDS:
      return toggle_hud_coords();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_FPS:
      return toggle_hud_fps();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_HUD_DISTANCE:
      return toggle_hud_distance();
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_MOVE_RATE:
      return cycle_move_rate(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_SPAWN_AT:
      return cycle_object_spawn_at(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_DISTANCE:
      return cycle_object_distance(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_HEADING:
      return cycle_object_heading(param);
    case GTAV_NATIVE_SHELL_ACTION_CYCLE_OBJECT_ON_GROUND:
      return cycle_object_on_ground(param);
    case GTAV_NATIVE_SHELL_ACTION_MOVE_LAST_OBJECT:
      // Worker-direct: only flips transient flags + raises a menu-hide request (no entity
      // native here). The game-thread driver acquires + drives the object next frame.
      return enter_object_move_mode();
    case GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_EDIT_OBJECT:
      return toggle_auto_edit_object();
    case GTAV_NATIVE_SHELL_ACTION_PRESET_PARKOUR:
      return preset_parkour();
    case GTAV_NATIVE_SHELL_ACTION_SAVE_PROFILE:
      return gtav_features_profile_save_default() == 0 ? ok(action, "profile saved")
                                                       : failed(action, "profile save failed");
    case GTAV_NATIVE_SHELL_ACTION_LOAD_PROFILE:
      return gtav_features_profile_load_default() == 0 ? ok(action, "profile loaded")
                                                       : unavailable(action, "no saved profile");
    default:
      return unavailable(action, "unsupported feature action");
  }
}

// Per-tick work, split out of gtav_features_worker_tick for readability. Order is
// load-bearing (see the call site): telemetry, then player re-assertions, then the
// per-vehicle re-assertions -- do not reorder. Defined below; forward-declared here.
static void tick_frame_hook_telemetry(void);
static void tick_player_toggles(void);
static void tick_vehicle_toggles(int v);

// The per-tick "held" toggle re-assertions (god mode, vehicle god/health/grip, nitro/brake,
// auto-heal, wanted lock, movement multipliers, ...). Every native below resolves a script
// handle to a LIVE engine entity and walks/mutates it, so this MUST run on the game/script
// thread, serialized against the engine's own writers. Off the game thread it races the
// entity lifecycle and faults on a transiently-null internal sub-pointer during respawns /
// cutscenes / SP<->MP transitions -- the intermittent in-gameplay SIGSEGV this lane caused.
static void apply_held_toggles(void) {
  if (!has_core_player_natives()) return;
  tick_player_toggles();
  // Vehicle toggles all act on the current vehicle, so resolve it ONCE (re-resolving handles
  // the player switching cars) and reuse it -- one GET_VEHICLE_PED_IS_IN call instead of one
  // per feature. v==0 means on foot, and every vehicle native is guarded on it.
  if (g_vehicle_god_enabled || g_nitro_enabled || g_engine_on_enabled || g_keep_clean_enabled ||
      g_keep_repaired_enabled || g_stick_to_ground_enabled || g_drift_mode_enabled ||
      g_super_brake_enabled || g_cruise_control_enabled || g_slippery_enabled) {
    int v = current_vehicle_strict();
    tick_vehicle_toggles(v);
  }
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Game-thread driver for the held-toggle re-assertions, run from gtav_features_game_thread_tick
// on the PLAYER_PED_ID frame hook (valid script context). The hook fires many times per
// rendered frame, so collapse to a single re-assertion per frame with GET_FRAME_COUNT: the
// speed-ramp toggles (nitro / super-brake) are cadence-sensitive and would slam to their cap
// otherwise, and the idempotent setters are simply wasteful to repeat thousands of times a
// second inside the hook's drain critical section.
static void reassert_toggles_game_thread(void) {
  if (g_n.get_frame_count) {
    static uint32_t s_last_frame;
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == s_last_frame) return;  // already re-asserted this frame
    s_last_frame = frame;
  }
  // Vehicle-class cache: built here on the game/script thread (valid context) the first
  // frame the hook fires, so the spawner class filter never calls
  // GET_VEHICLE_CLASS_FROM_NAME from the scePad worker (which dereferences a null script
  // context on 01.010.002). No-op after the first build.
  warm_vehicle_class_cache_game_thread();
  apply_held_toggles();
}
#endif

extern "C" void gtav_features_set_menu_open(int menu_open) {
  // Called from gtav_menu_set_visible the instant visibility flips, so the game-thread
  // input-suppression predicate (weapon_fx.inc / noclip.inc read g_menu_open) is current on
  // the open/close frame itself rather than a worker tick later.
  g_menu_open = menu_open ? 1 : 0;
}

extern "C" void gtav_features_set_wardrobe_cam_active(int active) {
  // Worker writes this each tick from the open-submenu state (native_bridge worker tick); the
  // game-thread wardrobe-cam driver reads it. Plain volatile store -> thread-safe.
  g_wardrobe_cam_active = active ? 1 : 0;
}

extern "C" void gtav_features_worker_tick(int menu_visible) {
  // Mirror menu visibility for the game-thread noclip driver (it has no parameter and must
  // stand down while the menu is open). Set before any early return so it stays fresh.
  g_menu_open = menu_visible;
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  // Skip-Prologue: while armed, stamp the prologue's flow-completion flags every worker
  // tick (BEFORE the native-readiness gate, so it runs during a story load). The gate
  // f_330[53] is one of a diff-derived candidate set; stamping all of them replicates the
  // post-prologue flag state and re-stamping wins the flow-controller's launch read on a
  // reload. Null-guarded plain stores; no native, safe off the game thread.
  if (g_prologue_skip_armed && (uintptr_t)GTAV_GLOBALS_BASE_ADDR != 0) {
    static const uint32_t kPrologueCandidates[] = GTAV_PROLOGUE_GLOBAL_CANDIDATES;
    for (unsigned i = 0; i < sizeof(kPrologueCandidates) / sizeof(kPrologueCandidates[0]); ++i) {
      if (kPrologueCandidates[i] != 0u)
        gtav_script_write_global((uintptr_t)GTAV_GLOBALS_BASE_ADDR, kPrologueCandidates[i], 1);
    }
  }
#endif
  tick_frame_hook_telemetry();
#if !GTAV_MENU_ENABLE_FRAME_HOOK
  // No game-thread frame-hook lane in this build, so fall back to re-asserting the held
  // toggles here on the worker. This is the historical lane and is NOT transition-safe (the
  // re-assertion natives walk live engine entities the game thread frees/rebuilds across state
  // changes). The shipped live menu always has the frame hook and re-asserts on the game/script
  // thread instead -- see reassert_toggles_game_thread / gtav_features_game_thread_tick. With
  // native features compiled in, reaching this line requires GTAV_MENU_ALLOW_WORKER_REASSERT=1
  // (the build-time guard at the top of this file otherwise hard-errors the combination); in the
  // inert / host-test builds there is no resolved native table, so apply_held_toggles is a no-op.
  apply_held_toggles();
#endif
}

// Re-assert the per-vehicle held toggles on the current vehicle handle `v` (0 = on foot).
// Runs from the game-thread frame-hook tick via apply_held_toggles (see
// reassert_toggles_game_thread). The "modify-existing" natives below do not allocate, but they
// still walk the live CVehicle, so they belong on the game/script thread, not the scePad
// worker -- the "(worker-safe)" notes below mean only "does not allocate", not "thread-safe".
static void tick_vehicle_toggles(int v) {
  // Vehicle God Mode: reassert invincibility on the current vehicle.
  if (v && g_vehicle_god_enabled && g_n.set_entity_invincible) {
    invoke_void(g_n.set_entity_invincible, v, 1);
  }
  // Nitro on Horn: while the horn is held, re-drive the vehicle's forward speed toward a hard
  // cap. SET_VEHICLE_FORWARD_SPEED only modifies an existing entity (no allocation). Reading
  // GET_ENTITY_SPEED and nudging up with an absolute clamp means it ramps quickly then
  // plateaus; the caller frame-gates this to one application per rendered frame (GET_FRAME_COUNT)
  // so it cannot run away at the hook's many-fires-per-frame rate. Releasing the horn returns
  // to normal physics.
  if (v && g_nitro_enabled && g_n.set_vehicle_forward_speed && g_n.get_entity_speed &&
      g_n.is_control_pressed &&
      invoke_return<int>(g_n.is_control_pressed, 0, GTAV_NITRO_HORN_CONTROL)) {
    // INPUT_VEH_HORN (control id 86), pad index 0.
    const NitroProfile& profile = current_nitro_profile();
    float speed = invoke_return<float>(g_n.get_entity_speed, v);
    float target = speed + profile.step_mps;
    if (target > profile.max_mps) target = profile.max_mps;
    invoke_void(g_n.set_vehicle_forward_speed, v, target);
  }
  // Super Brake: while the brake control is held, scale forward speed down toward zero
  // each frame so the car stops hard but smoothly. SET_VEHICLE_FORWARD_SPEED is
  // modify-existing (no allocation), same lane as Nitro above. Releasing the brake returns
  // to normal physics. Read the brake via IS_DISABLED_CONTROL_PRESSED as well so it still
  // works while the menu disables controls (see disabled-control read gotcha).
  //
  // The brake control (72) is ALSO the reverse control, and GET_ENTITY_SPEED is unsigned, so
  // a blind SET_VEHICLE_FORWARD_SPEED(v, speed * RETAIN) sets a POSITIVE forward speed every
  // frame and pins the car -- the player can brake to a stop but can never reverse. Gate the
  // bleed on the car actually moving FORWARD, recovered from the per-frame position delta
  // projected onto the car's facing (fwd = (-sin h, cos h), the engine's heading convention).
  if (v && g_super_brake_enabled && g_n.set_vehicle_forward_speed && g_n.get_entity_speed &&
      g_n.is_control_pressed) {
    // INPUT_VEH_BRAKE (control id 72), pad index 0.
    int braking = invoke_return<int>(g_n.is_control_pressed, 0, GTAV_SUPER_BRAKE_CONTROL);
    if (!braking && g_n.is_disabled_control_pressed) {
      braking = invoke_return<int>(g_n.is_disabled_control_pressed, 0, GTAV_SUPER_BRAKE_CONTROL);
    }
    // Default permissive only when we cannot read direction (coords/heading natives missing) so
    // the feature still hard-stops in a degraded build; the live build always resolves both.
    int moving_forward = 1;
    if (g_n.get_entity_coords && g_n.get_entity_heading) {
      Vec3 cur = invoke_vector(g_n.get_entity_coords, v, 1);
      // No prior sample (just entered the car / first frame) or an implausible jump (teleport,
      // car switch) -> direction is unknown this frame; don't force a forward speed.
      moving_forward = 0;
      if (g_super_brake_have_prev) {
        const float dx = cur.x - g_super_brake_prev_x;
        const float dy = cur.y - g_super_brake_prev_y;
        if (dx * dx + dy * dy < 100.0f) {  // < 10 m in one frame: a real driving delta
          const float h = invoke_return<float>(g_n.get_entity_heading, v) * GTAV_FEAT_DEG2RAD;
          const float fwd_x = -feat_sinf(h);
          const float fwd_y = feat_cosf(h);
          moving_forward = (dx * fwd_x + dy * fwd_y) > GTAV_SUPER_BRAKE_FWD_EPS;
        }
      }
      g_super_brake_prev_x = cur.x;
      g_super_brake_prev_y = cur.y;
      g_super_brake_have_prev = 1;
    }
    if (braking && moving_forward) {
      float speed = invoke_return<float>(g_n.get_entity_speed, v);
      invoke_void(g_n.set_vehicle_forward_speed, v, speed * GTAV_SUPER_BRAKE_RETAIN);
    }
  } else {
    // Not braking-eligible this frame (toggle off / on foot / natives missing): drop the
    // tracked sample so re-entering a vehicle can't project a stale, far-away delta.
    g_super_brake_have_prev = 0;
  }
  // Cruise Control: pin the current vehicle to the selected forward speed. If the player is
  // not in a vehicle, the caller passes v=0 and the toggle simply idles until they enter one.
  if (v && g_cruise_control_enabled && g_n.set_vehicle_forward_speed) {
    invoke_void(g_n.set_vehicle_forward_speed, v, selected_cruise_speed_mps());
  }
  // Engine Always On: keep the current vehicle's engine running (e.g. so it never
  // stalls). Modify-existing-entity native, worker-safe.
  if (v && g_engine_on_enabled && g_n.set_vehicle_engine_on) {
    invoke_void(g_n.set_vehicle_engine_on, v, 1, 1, 0);
  }
  // Keep Vehicle Clean: hold the dirt level at zero.
  if (v && g_keep_clean_enabled && g_n.set_vehicle_dirt_level) {
    invoke_void(g_n.set_vehicle_dirt_level, v, 0.0f);
  }
  // Keep Repaired: two layers.
  //  1) Pin engine/body/petrol-tank health to full every tick. These are plain field
  //     stores (like SET_VEHICLE_DIRT_LEVEL) and catch health loss that leaves no visual
  //     mark (engine shot, overheat) -- but they CANNOT undo body deformation or an
  //     "engine destroyed / on fire" latch.
  //  2) Run the full CVehicle::Fix (SET_VEHICLE_FIXED) to recover those states. Fix is a
  //     damage/deformation/fragment rebuild that deadlocks the console when hammered off
  //     the worker thread, so it is compiled ONLY into the game-thread frame-hook lane
  //     (this tick runs in valid script context there) and gated on actual damage so it
  //     does not rebuild a pristine car every frame. Because the health pin above masks
  //     the health fields, the gate reads GET_DOES_VEHICLE_HAVE_DAMAGE_DECALS -- the one
  //     damage signal the pin does not touch. A light throttle caps the rebuild rate.
  if (v && g_keep_repaired_enabled) {
    if (g_n.set_vehicle_engine_health) {
      invoke_void(g_n.set_vehicle_engine_health, v, GTAV_VEHICLE_FULL_HEALTH);
    }
    if (g_n.set_vehicle_petrol_tank_health) {
      invoke_void(g_n.set_vehicle_petrol_tank_health, v, GTAV_VEHICLE_FULL_HEALTH);
    }
    if (g_n.set_vehicle_body_health) {
      invoke_void(g_n.set_vehicle_body_health, v, GTAV_VEHICLE_FULL_HEALTH);
    }
#if GTAV_MENU_ENABLE_FRAME_HOOK
    // Game-thread-only: full Fix recovery for deformation / engine-destroyed states.
    if (g_n.set_vehicle_fixed && g_n.get_does_vehicle_have_damage_decals) {
      static uint32_t s_repair_tick;
      if ((s_repair_tick++ % 5u) == 0u &&  // ~12 Hz check (frame hook is ~60 Hz)
          invoke_return<int>(g_n.get_does_vehicle_have_damage_decals, v)) {
        invoke_void(g_n.set_vehicle_fixed, v);
        if (g_n.set_vehicle_undriveable) invoke_void(g_n.set_vehicle_undriveable, v, 0);
      }
    }
#endif
  }
  // Drift Mode: cut the current vehicle's grip so it slides under throttle, but only
  // once it is rolling -- at a standstill full grip makes the wheels spin without
  // moving (see drift_reduce_grip_now). SET_VEHICLE_REDUCE_GRIP is a modify-existing
  // field store (worker-safe). Re-asserted each tick so it tracks speed and follows the
  // player across car swaps.
  if (v && g_drift_mode_enabled && g_n.set_vehicle_reduce_grip) {
    invoke_void(g_n.set_vehicle_reduce_grip, v, drift_reduce_grip_now(v));
  }
  // Slippery Roads: constant reduced grip (ice physics) regardless of speed -- distinct from
  // Drift's speed-gated grip. Re-asserted each tick so it tracks car swaps. If both are on, this
  // wins (last writer per frame), which is the more-slippery behaviour the user asked for.
  if (v && g_slippery_enabled && g_n.set_vehicle_reduce_grip) {
    invoke_void(g_n.set_vehicle_reduce_grip, v, 1);
  }
  // Stick to Ground: keep the car on its wheels when it leaves the road. Only snap when
  // the vehicle sits within a sensible height BAND above the ground below it, and throttle
  // so a jump isn't re-snapped every tick. SET_VEHICLE_ON_GROUND_PROPERLY is modify-existing
  // (worker-safe).
  //   - Below MIN: normal driving / suspension travel / small bumps -> leave it alone so we
  //     don't fight the suspension (the old 1.5 m floor still jittered on rough roads).
  //   - Above MAX: GET_GROUND_Z reports the terrain UNDER an overpass/bridge, so a car
  //     driving on an elevated road reads as "tens of metres airborne" and the old code
  //     yanked it down through the structure -- the main source of the buggy/inconsistent
  //     feel. Treat an implausibly large gap as a false reading and skip the snap.
  if (v && g_stick_to_ground_enabled && g_n.set_vehicle_on_ground_properly) {
    static const float kStickMinAirborneM = 2.0f;  // ignore suspension travel / bumps
    static const float kStickMaxAirborneM = 8.0f;  // beyond this it's an overpass false read
    static uint32_t s_stick_tick;
    if ((s_stick_tick++ % 6u) == 0u) {  // ~15 Hz check
      bool snap = false;
      if (g_n.get_entity_coords && g_n.get_ground_z_for_3d_coord) {
        Vec3 p = invoke_vector(g_n.get_entity_coords, v, 1);
        float gz = 0.0f;
        // GET_GROUND_Z_FOR_3D_COORD(x, y, z, groundZ*, ignoreWater, p5) writes ground Z through a
        // float* out-param; the NativeArg ABI passes it as a pushed pointer arg. Returns BOOL
        // (0 = no ground). Pass all 6 args -- p5 was omitted (relied on the slot being zeroed).
        int found = invoke_return<int>(g_n.get_ground_z_for_3d_coord, p.x, p.y, p.z,
                                       (uint64_t)(uintptr_t)&gz, 0, 0);
        // No ground (over water/void) -> don't yank the car down.
        const float gap = p.z - gz;
        snap = found && gap > kStickMinAirborneM && gap < kStickMaxAirborneM;
      }
      if (snap) invoke_void(g_n.set_vehicle_on_ground_properly, v);
    }
  }
}

static void tick_frame_hook_telemetry(void) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  // Periodic fire-rate telemetry so the live test can confirm the hooked native
  // is actually called every frame (call_count climbing) and see job throughput.
  // The (+N/report) deltas are the per-report heartbeat: at the default ~60-tick
  // report cadence (~1s) they read directly as fires/sec and ctx-hits/sec -- the two
  // numbers that pick a steady, in-script-context per-frame hook target. A starved
  // target (e.g. DISABLE_CONTROL_ACTION idle) shows +0/report until the D-pad drives
  // the game's scripts; a true per-frame fn shows a steady non-zero delta while idle.
  static uint32_t s_hook_report_tick;
  static uint32_t s_prev_calls;
  static uint64_t s_prev_ctx_hits;
  gtav_patch_broker_tick(s_hook_report_tick, gtav_frame_hook_call_count(),
                         gtav_frame_hook_jobs_run());
  if ((s_hook_report_tick++ % 60u) == 0u) {
    GtavFrameHookProbeSnapshot probe{};
    gtav_frame_hook_probe_snapshot(&probe);
    const uint32_t calls = gtav_frame_hook_call_count();
    const uint32_t d_calls = calls - s_prev_calls;
    const uint64_t d_ctx = probe.calls_with_context - s_prev_ctx_hits;
    s_prev_calls = calls;
    s_prev_ctx_hits = probe.calls_with_context;
    // (+N) are the per-report deltas (see comment above): at the ~1s cadence they read
    // as fires/sec and ctx-hits/sec.
    //
    // Split across several events ON PURPOSE. A status event message is capped at
    // GTAV_MENU_STATUS_EVENT_LEN (80) and vsnprintf truncates silently, so a single
    // wide line loses its tail -- and, worse, cuts a hex field mid-number so the
    // surviving prefix still parses as a plausible (but wrong) value. That is exactly
    // what happened here: the pointer fields were reported as "fs=0x6f" / "fs=0x6fd",
    // which are the first 2-3 nibbles of a real 0x6fd......  TLS base surviving the cut
    // at 80 chars as the call counter grew a digit. A round of TLS "calibration" work
    // chased that artifact. Keep each line comfortably short, and put at most two
    // 64-bit hex fields on any one of them.
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "hook a=%d calls=%u (+%u) ctx+%llu jobs=%u",
                       gtav_frame_hook_is_active(), calls, d_calls, (unsigned long long)d_ctx,
                       gtav_frame_hook_jobs_run());
    // The detail lines go out far less often than the summary. The status event ring holds
    // GTAV_MENU_STATUS_EVENT_COUNT (16) slots: emitting five events every second evicts the
    // whole ring in ~3s, which hides exactly the feature results (spawn/weapon/skin outcomes)
    // an operator reads the ring for. One summary per second, the pointer detail every ~30s.
    if ((s_hook_report_tick % 1800u) == 0u) {
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE,
                         "frame hook ctx_hits=%llu (+%llu) ctx=0x%llx",
                         (unsigned long long)probe.calls_with_context, (unsigned long long)d_ctx,
                         (unsigned long long)probe.last_script_context);
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "frame hook fs=0x%llx rfs=0x%llx",
                         (unsigned long long)probe.last_fsbase,
                         (unsigned long long)probe.last_real_fsbase);
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "frame hook last_native=0x%llx nseq=%u",
                         (unsigned long long)gtavmenu::g_last_native_addr,
                         (unsigned)gtavmenu::g_last_native_seq);
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "frame hook probe nt=0x%llx ntv=0x%llx",
                         (unsigned long long)probe.last_native_thread,
                         (unsigned long long)probe.last_native_thread_vtable);
    }
  }
#endif
}

// Drop the in-flight teleport entity onto the streamed ground at its destination XY, called
// from the teleport poll when the freeze is released. GET_GROUND_Z_FOR_3D_COORD casts DOWN from
// the probe point, so we probe from well above any terrain (a short descending ladder beats the
// native's occasional miss at extreme heights) and take the first hit. SNAP (waypoint /
// objective: no real Z) lands exactly on that ground; FLOOR (curated preset / saved location)
// raises only a Z that would otherwise sit below the ground, so an intentional rooftop or
// mountain height is preserved (the ground mesh under a rooftop is the street far below, so the
// max() keeps the roof). Over open water -- no solid ground -- we fall back to the water surface
// so the player lands on the sea instead of the seabed or mid-air; failing even that we leave
// the requested Z untouched. Self-guarding: if nothing resolves (e.g. the cell never streamed on
// a timeout release) the entity is simply unfrozen at its requested Z, the pre-clamp behaviour.
static void settle_teleport_onto_ground(void) {
  const uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  if (!set_coords || !g_n.get_ground_z_for_3d_coord) return;

  // Probe from the top down; the first height that resolves wins. Covers the tallest reachable
  // ground (Mt Chiliad ~800 m) with margin while still working down at sea level.
  static const float kProbeHeights[] = {1500.0f, 800.0f, 300.0f, 80.0f};
  float ground = 0.0f;
  int found = 0;
  for (unsigned i = 0; i < sizeof(kProbeHeights) / sizeof(kProbeHeights[0]); ++i) {
    float gz = 0.0f;
    // GET_GROUND_Z_FOR_3D_COORD(x, y, z, groundZ*, ignoreWater, p5): all 6 args; BOOL return.
    if (invoke_return<int>(g_n.get_ground_z_for_3d_coord, g_tp_x, g_tp_y, kProbeHeights[i],
                           (uint64_t)(uintptr_t)&gz, 0, 0)) {
      ground = gz;
      found = 1;
      break;
    }
  }

  const float off = g_tp_is_vehicle ? GTAV_TP_GROUND_OFFSET_VEHICLE : GTAV_TP_GROUND_OFFSET_PED;
  float final_z = g_tp_z;
  if (found) {
    const float on_ground = ground + off;
    // FLOOR never lowers the requested Z (keeps curated rooftops); SNAP places exactly on it.
    final_z = (g_tp_clamp == GTAV_TP_CLAMP_SNAP || on_ground > g_tp_z) ? on_ground : g_tp_z;
  } else if (g_n.get_water_height) {
    // No ground at this XY -> open water. Land on the surface (GET_WATER_HEIGHT(x,y,z,height*)).
    float wz = 0.0f;
    if (invoke_return<int>(g_n.get_water_height, g_tp_x, g_tp_y, g_tp_z + 1000.0f,
                           (uint64_t)(uintptr_t)&wz)) {
      final_z = wz + off;
    }
  }

  // Re-place at the resolved height. The no-offset setter takes one fewer trailing arg; mirror
  // every other call site's branch on which variant resolved.
  if (g_n.set_entity_coords_no_offset) {
    invoke_void(set_coords, g_tp_entity, g_tp_x, g_tp_y, final_z, 0, 0, 1);
  } else {
    invoke_void(set_coords, g_tp_entity, g_tp_x, g_tp_y, final_z, 0, 0, 0, 0);
  }
  // Seat a vehicle squarely on its wheels at the resolved height.
  if (g_tp_is_vehicle && g_n.set_vehicle_on_ground_properly) {
    invoke_void(g_n.set_vehicle_on_ground_properly, g_tp_entity);
  }
}

static void tick_player_toggles(void) {
  if (g_freeze_time_enabled && g_n.network_override_clock_time) {
    static uint32_t s_freeze_time_tick;
    if ((s_freeze_time_tick++ % 30u) == 0u) {
      apply_selected_time();
    }
  }

  // Advance an in-progress streaming-aware teleport: keep requesting collision at the
  // destination and release the freeze once the area has streamed in (so the entity
  // settles onto real ground) or the timeout elapses (so a failed stream still frees
  // the player instead of stranding them frozen).
  if (g_tp_active && g_n.request_collision_at_coord && g_n.has_collision_loaded_around_entity &&
      g_n.freeze_entity_position) {
    invoke_void(g_n.request_collision_at_coord, g_tp_x, g_tp_y, g_tp_z);
    int loaded = invoke_return<int>(g_n.has_collision_loaded_around_entity, g_tp_entity);
    if (loaded || --g_tp_ticks_left <= 0) {
      // Resolve the destination onto real ground before releasing the freeze, so waypoint /
      // objective jumps stop landing in the sky or below the terrain. Run on both the streamed
      // and the timeout release: settle_teleport_onto_ground() self-guards on an unstreamed cell
      // (GET_GROUND_Z returns false -> entity left at its requested Z, the old behaviour).
      if (g_tp_clamp != GTAV_TP_CLAMP_NONE) settle_teleport_onto_ground();
      invoke_void(g_n.freeze_entity_position, g_tp_entity, 0);
      g_tp_active = 0;
    }
  }

  if (g_state.god_mode_enabled && g_n.set_player_invincible) {
    // Use the validated handle accessors, like every sibling toggle: PLAYER_ID can be -1 and
    // PLAYER_PED_ID can be 0 during loads/cutscenes, and passing those raw to the invincibility
    // natives walks a bad CPlayerInfo/CPed index the same way the spawn path guards against.
    int player = valid_player_id();
    int ped = valid_player_ped();
    if (player >= 0) invoke_void(g_n.set_player_invincible, player, 1);
    if (g_n.set_entity_invincible && ped) invoke_void(g_n.set_entity_invincible, ped, 1);
  }
  // Rainbow neon: sweep the current vehicle's underglow hue every tick while enabled.
  rainbow_neon_tick();
  // Rainbow paint: sweep the current vehicle's custom body colour every tick while enabled.
  rainbow_paint_tick();
  // Weapon damage multiplier: re-assert it (the engine resets it on weapon switch).
  apply_weapon_damage_tick();
  // Noclip is driven on the game thread (gtav_features_game_thread_tick via the frame
  // hook), not here -- its camera/move natives fault off the game thread.
  // Auto-Heal: top the player's health + armor back up every tick.
  if (g_auto_heal_enabled && g_n.set_entity_health && g_n.player_ped_id) {
    int ped = native_player_ped_id();
    if (ped) {
      int health = 200;
      if (g_n.get_ped_max_health) {
        int max = invoke_return<int>(g_n.get_ped_max_health, ped);
        if (max > 0) health = max;
      }
      invoke_void(g_n.set_entity_health, ped, health);
      if (g_n.set_ped_armour) invoke_void(g_n.set_ped_armour, ped, 100);
    }
  }
  // Wanted Lock: hold the wanted level at the selected star count every tick.
  if (g_wanted_lock_enabled && g_n.set_player_wanted_level && g_n.set_player_wanted_level_now) {
    int player = valid_player_id();
    int level = (int)(g_wanted_index % 6u);
    if (player >= 0) {
      invoke_void(g_n.set_player_wanted_level, player, level, 0);
      invoke_void(g_n.set_player_wanted_level_now, player, 0);
    }
  }
  // Movement multipliers reset each frame, so reassert while toggled on.
  if (g_state.super_run_enabled && g_n.set_run_sprint_multiplier) {
    int player = valid_player_id();
    if (player >= 0) invoke_void(g_n.set_run_sprint_multiplier, player, 1.49f);
  }
  if (g_state.fast_swim_enabled && g_n.set_swim_multiplier) {
    int player = valid_player_id();
    if (player >= 0) invoke_void(g_n.set_swim_multiplier, player, 1.49f);
  }
  // SET_SUPER_JUMP_THIS_FRAME only lasts one frame, so reassert while toggled on.
  if (g_super_jump_enabled && g_n.set_super_jump_this_frame) {
    int player = valid_player_id();
    if (player >= 0) invoke_void(g_n.set_super_jump_this_frame, player);
  }
  // Fast Movement: SET_PED_MOVE_RATE_OVERRIDE resets each frame, so reassert while
  // toggled on (speeds up all on-foot movement, not just sprint). The multiplier is the
  // Move Speed cycler's selection (default 1.4x) so the toggle's strength is tunable.
  if (g_fast_move_enabled && g_n.set_ped_move_rate_override && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) invoke_void(g_n.set_ped_move_rate_override, ped, current_move_rate());
  }
  // No Ragdoll: keep ragdoll disabled (re-resolve the ped so it survives respawns).
  if (g_no_ragdoll_enabled && g_n.set_ped_can_ragdoll && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) invoke_void(g_n.set_ped_can_ragdoll, ped, 0);
  }
  // Seatbelt: keep the never-knock-off state (bikes) AND the cleared
  // CanFlyThroughWindscreen flag (cars) asserted -- the engine restores the config flag,
  // so it must be re-cleared each frame (re-resolve ped so it survives respawns).
  if (g_seatbelt_enabled && g_n.set_ped_can_be_knocked_off_vehicle && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) {
      invoke_void(g_n.set_ped_can_be_knocked_off_vehicle, ped, 1);
      if (g_n.set_ped_config_flag)
        invoke_void(g_n.set_ped_config_flag, ped, GTAV_PED_CONFIG_FLAG_FLY_THROUGH_WINDSCREEN, 0);
    }
  }
  // Super Proofs: reassert all damage proofs (re-resolve ped so it survives respawns).
  if (g_super_proofs_enabled && g_n.set_entity_proofs && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) invoke_void(g_n.set_entity_proofs, ped, 1, 1, 1, 1, 1, 1, 1, 1);
  }
  // No Critical Hits: keep critical hits disabled.
  if (g_no_crit_enabled && g_n.set_ped_suffers_critical_hits && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) invoke_void(g_n.set_ped_suffers_critical_hits, ped, 0);
  }
  // Infinite Stamina: refill stamina each tick so the player never tires.
  if (g_inf_stamina_enabled && g_n.reset_player_stamina && g_n.player_id) {
    int player = valid_player_id();
    if (player >= 0) invoke_void(g_n.reset_player_stamina, player);
  }
  // Infinite Special Ability: keep the special-ability meter topped up.
  if (g_inf_special_enabled && g_n.special_ability_fill_meter && g_n.player_id) {
    int player = valid_player_id();
    if (player >= 0) invoke_void(g_n.special_ability_fill_meter, player, 1);
  }
  // Never Wanted: keep zeroing the wanted level so stars never stick.
  if (g_never_wanted_enabled && g_n.set_player_wanted_level && g_n.set_player_wanted_level_now) {
    int player = valid_player_id();
    if (player >= 0) {
      invoke_void(g_n.set_player_wanted_level, player, 0, 0);
      invoke_void(g_n.set_player_wanted_level_now, player, 0);
    }
  }
  // Infinite Parachutes: keep a parachute granted so the player never runs out. GIVE_WEAPON_TO_PED
  // walks the ped weapon manager (game-thread only), so it lives here, not on the worker. Throttled
  // to ~every 32 frames -- re-granting every frame churns the weapon manager and is unnecessary
  // (the gadget persists). bForceInHand=0 so it never force-switches the player to the chute.
  if (g_inf_parachute_enabled && g_n.give_weapon_to_ped && g_n.player_ped_id) {
    static uint32_t s_para_div;
    if ((s_para_div++ & 31u) == 0u) {
      int ped = valid_player_ped();
      if (ped) invoke_void(g_n.give_weapon_to_ped, ped, GTAV_GADGET_PARACHUTE_HASH, 1, 0, 0);
    }
  }
  // Thin Population: hold the ped + vehicle population budgets at minimum. The engine re-raises
  // them, so re-assert every tick while enabled. Global int setters, no entity walk; the engine
  // restores normal density on its own once we stop forcing 0 (so no restore on disable).
  if (g_thin_population_enabled && g_n.set_ped_population_budget &&
      g_n.set_vehicle_population_budget) {
    invoke_void(g_n.set_ped_population_budget, 0);
    invoke_void(g_n.set_vehicle_population_budget, 0);
  }
  // Ignored By Everyone: re-assert the police/peds/gangs ignore flags while enabled (sticky
  // setters, but re-asserting survives respawns). The handler restores defaults on disable.
  if (g_ignored_by_all_enabled && g_n.set_police_ignore_player && g_n.set_everyone_ignore_player) {
    int player = valid_player_id();
    if (player >= 0) {
      invoke_void(g_n.set_police_ignore_player, player, 1);
      invoke_void(g_n.set_everyone_ignore_player, player, 1);
      if (g_n.set_player_can_be_hassled_by_gangs)
        invoke_void(g_n.set_player_can_be_hassled_by_gangs, player, 0);
    }
  }
  // No-Reload / Infinite Clip: hold the infinite-clip flag on the player ped so the magazine never
  // empties (re-resolve the ped so it survives respawns / weapon switches).
  if (g_no_reload_enabled && g_n.set_ped_infinite_ammo_clip && g_n.player_ped_id) {
    int ped = valid_player_ped();
    if (ped) invoke_void(g_n.set_ped_infinite_ammo_clip, ped, 1);
  }
  bodyguard_game_thread_tick();
}

extern "C" void gtav_features_snapshot(GtavFeatureState* out) {
  if (!out) return;
  memcpy(out, &g_state, sizeof(*out));
}

extern "C" uint64_t gtav_features_toggle_mask(void) {
  uint64_t mask = 0;
#define GTAV_TOGGLE(name, state, reactivate, default_on) \
  if (state) mask |= GTAV_FEATURE_TOGGLE_##name;
#include "gtavmenu/feature_toggles.def"
  return mask;
}

extern "C" const char* gtav_feature_action_name(uint32_t action) {
  // Per-slot mod pickers (contiguous range) map to distinct snake_case names.
  if (action >= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST &&
      action <= GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_LAST) {
    return kVehicleModSlotActionNames[action - GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST];
  }
  switch (action) {
#define GTAV_ACTION_NAME(suffix, name)    \
  case GTAV_NATIVE_SHELL_ACTION_##suffix: \
    return name;
#include "gtavmenu/feature_actions.def"
    default:
      return "none";
  }
}

extern "C" const char* gtav_feature_result_name(uint32_t result) {
  switch (result) {
    case GTAV_FEATURE_RESULT_OK:
      return "ok";
    case GTAV_FEATURE_RESULT_UNAVAILABLE:
      return "unavailable";
    case GTAV_FEATURE_RESULT_FAILED:
      return "failed";
    case GTAV_FEATURE_RESULT_NONE:
    default:
      return "none";
  }
}
