#pragma once

#include <stdint.h>

#include "gtavmenu/keybinds.h"
#include "gtavmenu/profile_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default path as seen by the sandboxed worker. The loader mounts the persistent host-side state
 * directory here before injection, so a setup survives menu and game restarts. */
#define GTAV_FEATURE_PROFILE_DEFAULT_PATH GTAV_PROFILE_STORAGE_PATH
/* v3 appended companion + cruise cyclers; v4 appended the companion aggression cycler; v5
 * appended the companion weapon cycler; v6 appended the companion accuracy cycler. The loader
 * memsets unknown fields to 0 before parsing, so older files load with the new fields defaulting
 * to index 0; features.cpp clamps them to each feature's documented defaults where needed
 * (aggression index 0 == DEFENSIVE, weapon index 0 == CARBINE; accuracy is version-gated to its
 * HIGH default for pre-v6 files). v7 appended the browser favorites/recents hash lists; the
 * memset-0 loader means pre-v7 files load with every list empty (no migration needed). v8
 * appended the saved wardrobe outfit (per-slot drawable + texture indices); v9 appended one
 * saved vehicle (model hash + indexed mod loadout); v10 appended the global Quick pins (an
 * (action, param) pair per pinned row, 0 == empty -- same memset-0, no-migration story). v11
 * appended the Worker Hz + Toast Time menu-cycler selection indices, which previously lived as
 * function-statics in menu.c and silently reverted on every re-inject; memset-0 makes a pre-v11
 * file load with index 0 (60 Hz) and the cyclers/apply clamp the toast index to its default. v12
 * appended the editable Custom-theme channels (accent/selection/panel RGB); memset-0 loads them
 * as 0, but features.cpp version-gates the import to seed the default palette for pre-v12 files.
 * v13 appended the Reduce Motion flag + the Nav Delay / Nav Speed cycler indices; pre-v13 files
 * version-gate to the historical defaults (full motion + Normal delay/speed, index 1). v14 appended
 * the optional-touchpad-input flag (memset-0 -> off, matching the default-off safety policy). v15
 * grew the editable custom-theme channels 9 -> 30 (all 10 colour groups); a pre-v15 file only had
 * the 9 chrome channels, so the import applies just those and keeps the default-palette seed for
 * the new groups (it would otherwise load them as black). v16 (a) appended the menu Panel Width
 * index (pre-v16 version-gates to Normal = 1, not the memset-0 Narrow); and (b) grew the saved
 * vehicle/outfit stores from one slot to GTAV_SAVED_VEHICLE_SLOTS / GTAV_SAVED_OUTFIT_SLOTS indexed
 * slots: the old single saved vehicle migrates into garage slot 0 (the bare saved_vehicle_model key
 * and the 17 saved_vehicle_mod0..16 keys load into slot 0), and the old single outfit is left in
 * the live wardrobe editor (wardrobe_drawable/texture) for the user to re-save into an outfit slot.
 */
#define GTAV_FEATURE_PROFILE_VERSION 16u

/* v16: saved-vehicle (garage) and saved-outfit slot counts. Save/Spawn/Apply act on the currently
 * selected slot (a worker-side index, not persisted -- it resets to 0 on reload). */
#define GTAV_SAVED_VEHICLE_SLOTS 10u
#define GTAV_SAVED_OUTFIT_SLOTS 10u

/* v12/v15: the in-menu Theme Editor's editable channels. v12 stored 9 (accent/selection/panel RGB);
 * v15 grew it to 30 (all 10 colour groups, 3 RGB each). Must match GTAV_CUSTOM_THEME_CHANNELS in
 * native_bridge.h (asserted in features.cpp). The pre-v15 count is kept for the version-gated
 * import. */
#define GTAV_PROFILE_CUSTOM_THEME_CHANNELS 30u
#define GTAV_PROFILE_CUSTOM_THEME_CHANNELS_V12 9u

/* v10: global "Quick" favorites -- up to GTAV_QUICK_PINS_MAX user-pinned rows, each stored as an
 * (action, param) pair (action 0 == empty slot). Unlike the v7 per-browser favorites (which key
 * on a model hash), a Quick pin identifies ANY menu row generically by its dispatch action plus
 * the row's param, so a toggle, cycler, teleport preset, or spawn row can all be pinned. The
 * native bridge owns the live arrays; features.cpp bridges them in/out like the v7 favorites. */
#define GTAV_QUICK_PINS_MAX 16u

/* v9: one saved vehicle build -- its model joaat hash plus the installed index for each of the
 * GTAV_SAVED_VEHICLE_MOD_SLOTS indexed mod slots (SET_VEHICLE_MOD slots 0..16; a slot stores -1
 * as 0xFFFFFFFF for "stock"). model == 0 means no vehicle is saved. */
#define GTAV_SAVED_VEHICLE_MOD_SLOTS 17u

/* v8: one saved wardrobe outfit -- the drawable + texture index the editor has set for each of
 * the GTAV_WARDROBE_SLOTS component/prop slots. Loaded into the editor's state on import but NOT
 * auto-applied (the user presses Apply Outfit); 17 = 12 components (0..11) + 5 props. */
#define GTAV_WARDROBE_SLOTS 17u

/* v7: per-browser favorites (user-pinned) and recents (auto, most-recent-first) stored as
 * fixed-capacity parallel arrays of model joaat hashes (0 == empty slot), mirroring the
 * keybind-array persistence idiom. The native bridge owns the live lists; features.cpp
 * bridges them in/out of the profile like theme/region. Capacities are single-sourced here
 * so native_bridge.cpp sizes its arrays from the same macros. */
#define GTAV_FAVORITE_VEHICLES_MAX 40u
#define GTAV_FAVORITE_PEDS_MAX 40u
#define GTAV_RECENT_VEHICLES_MAX 10u
#define GTAV_RECENT_PEDS_MAX 10u

/* Serializable menu state. Kept deliberately flat (no pointers) so the INI IO in
 * feature_profile.c is a trivial key=value round-trip and the ABI is stable. The
 * toggle_mask field reuses the GTAV_FEATURE_TOGGLE_* bits from features.h. */
typedef struct GtavFeatureProfile {
  uint32_t version;
  uint64_t toggle_mask;
  uint32_t weather_index;
  uint32_t time_index;
  uint32_t wanted_index;
  uint32_t nitro_power_index;
  uint32_t timescale_index;
  uint32_t gravity_index;
  uint32_t clock_hour;
  uint32_t move_rate_index;
  /* v2: menu customisation */
  uint32_t theme_index;
  uint32_t region_index;
  /* v2: keybinds, stored as flat parallel arrays so the INI stays plain key=value. */
  uint32_t keybind_mask[GTAV_KEYBIND_SLOTS];
  uint32_t keybind_action[GTAV_KEYBIND_SLOTS];
  /* v3: Menyoo-informed companion + cruise list choices. */
  uint32_t bodyguard_health_index;
  uint32_t bodyguard_armor_index;
  uint32_t bodyguard_formation_index;
  uint32_t cruise_speed_index;
  /* v4: companion threat response (0 DEFENSIVE / 1 AGGRESSIVE / 2 PASSIVE). */
  uint32_t bodyguard_aggression_index;
  /* v5: companion weapon loadout (index into kBodyguardWeapons; 0 == CARBINE). */
  uint32_t bodyguard_weapon_index;
  /* v6: companion combat accuracy (index into kBodyguardAccuracy; default HIGH). */
  uint32_t bodyguard_accuracy_index;
  /* v7: vehicle/ped browser favorites + recents, as model-hash lists (0 == empty slot). */
  uint32_t favorite_vehicles[GTAV_FAVORITE_VEHICLES_MAX];
  uint32_t favorite_peds[GTAV_FAVORITE_PEDS_MAX];
  uint32_t recent_vehicles[GTAV_RECENT_VEHICLES_MAX];
  uint32_t recent_peds[GTAV_RECENT_PEDS_MAX];
  /* v8: the LIVE wardrobe editor working set (per-slot drawable + texture index). v16 keeps this as
   * the editor buffer; the saved outfits live in the separate saved_outfit_* slot arrays below. */
  uint32_t wardrobe_drawable[GTAV_WARDROBE_SLOTS];
  uint32_t wardrobe_texture[GTAV_WARDROBE_SLOTS];
  /* v9 (slotted in v16): saved vehicle builds, one model hash + indexed mod loadout per garage
   * slot. Pre-v16 single saved vehicle migrates into slot 0. model == 0 means an empty slot. */
  uint32_t saved_vehicle_model[GTAV_SAVED_VEHICLE_SLOTS];
  uint32_t saved_vehicle_mods[GTAV_SAVED_VEHICLE_SLOTS][GTAV_SAVED_VEHICLE_MOD_SLOTS];
  /* v16: saved outfit slots (distinct from the live wardrobe editor above). Each slot is a full
   * per-component drawable + texture loadout; Apply Outfit copies the selected slot into the editor
   * and onto the ped. An all-zero slot is "empty". */
  uint32_t saved_outfit_drawable[GTAV_SAVED_OUTFIT_SLOTS][GTAV_WARDROBE_SLOTS];
  uint32_t saved_outfit_texture[GTAV_SAVED_OUTFIT_SLOTS][GTAV_WARDROBE_SLOTS];
  /* v10: global Quick pins -- parallel (action, param) arrays (action 0 == empty slot). */
  uint32_t quick_action[GTAV_QUICK_PINS_MAX];
  uint32_t quick_param[GTAV_QUICK_PINS_MAX];
  /* v11: Worker Hz + Toast Time menu-cycler selection indices (clamped to the choice-table size
   * at apply time in menu.c). 0 == the historical defaults (60 Hz; toast default is index 2). */
  uint32_t worker_hz_index;
  uint32_t toast_time_index;
  /* v12: Theme Editor custom-theme channels (accent/selection/panel RGB, 0-255 each). */
  uint32_t custom_theme[GTAV_PROFILE_CUSTOM_THEME_CHANNELS];
  /* v13: feel/accessibility tuners. reduce_motion: 0 = full animation, 1 = reduced (snap). The Nav
   * Delay / Nav Speed indices select the D-pad auto-repeat cadence (applied by menu.c). Pre-v13
   * files (memset-0) version-gate to the historical defaults: full motion, Normal delay/speed. */
  uint32_t reduce_motion;
  uint32_t nav_delay_index;
  uint32_t nav_speed_index;
  /* v14: optional touchpad gesture input (0 = off, the default until calibrated on hardware). */
  uint32_t touchpad_enabled;
  /* v16: menu panel width index (Narrow/Normal/Wide). Pre-v16 version-gates to Normal (1). */
  uint32_t panel_width_index;
} GtavFeatureProfile;

/* Write/read a profile as an INI file under the game's root (via the rootdir guard).
 * Return 0 on success, non-zero on any IO error (caller keeps in-memory state). */
int gtav_feature_profile_save(const char* path, const GtavFeatureProfile* profile);
int gtav_feature_profile_load(const char* path, GtavFeatureProfile* profile);

#ifdef __cplusplus
}
#endif
