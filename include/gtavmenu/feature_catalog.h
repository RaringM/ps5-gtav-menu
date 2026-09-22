#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Shared, build-time data for menu rows and feature actions. Vehicle rows carry
// a precomputed joaat hash so the game-thread spawn queue can receive a compact
// model id directly, without hashing strings from the hook callback.

// Strings are stored INLINE (fixed char arrays), not as const char* pointers. A pointer
// field would need an R_X86_64_RELATIVE load-time relocation; the etaHEN fake-self PRX
// loaded by kstuff-lite does not reliably apply those, leaving the pointers as link-time
// addresses that fault on dereference (hardware 2026-06-15: crash dereferencing catalog
// labels in build_menus). Inline storage needs no relocations. The generated
// `{"label","model",hash}` initializers populate the arrays unchanged.
typedef struct GtavVehicleEntry {
  char label[40];  // short menu label (longest observed 25)
  char model[20];  // GTA V model name for GET_HASH_KEY (longest observed 14)
  uint32_t model_hash;
} GtavVehicleEntry;

typedef struct GtavWeaponEntry {
  char name[32];  // GTA V weapon name (longest observed 28, WEAPON_GRENADELAUNCHER_SMOKE); for logs
  uint32_t hash;  // precomputed joaat(name), so give_weapons() needs no GET_HASH_KEY
  int ammo;       // ammo to grant with the weapon
} GtavWeaponEntry;

// Skin changer / spawn-ped / bodyguard model list. hash is precomputed joaat(name) so
// the skin/spawn job requests and applies the model without runtime GET_HASH_KEY (matches
// the vehicle/weapon catalogs). The job validates each with IS_MODEL_VALID before use, so
// an unknown/foreign name degrades to "model not valid" rather than crashing. category is
// the 0-based index into native_bridge.cpp's ped category list (its kPedCategoryNames
// prepends a synthetic "All", so category N there is index N + 1) -- it drives the in-menu
// category filter. Generated from the Menyoo PedList dump; see
// devtools/generators/generate_ped_registry.py and data/peds/gtav-ped-names.txt.
typedef struct GtavPedModelEntry {
  char label[40];    // caption / menu label (longest observed 34)
  char name[28];     // GTA V ped model name (longest observed 25); for logs/debugging
  uint8_t category;  // ped category index (see ped_categories_generated.h)
  uint32_t hash;     // precomputed joaat(name)
} GtavPedModelEntry;

extern const GtavVehicleEntry gtav_vehicle_catalog[];
extern const uint32_t gtav_vehicle_catalog_count;

extern const GtavWeaponEntry gtav_weapon_catalog[];
extern const uint32_t gtav_weapon_catalog_count;

extern const GtavPedModelEntry gtav_ped_model_catalog[];
extern const uint32_t gtav_ped_model_catalog_count;

// Object spawner prop list. hash is precomputed joaat(name) (same joaat() as the other
// catalogs) so the spawn job requests + creates the prop without runtime GET_HASH_KEY; the
// job validates each with IS_MODEL_* before use. The catalog is generated from a curated,
// categorized prop list (devtools/generators/generate_object_registry.py); `category` drives the
// in-menu category filter (mirrors the vehicle-class filter). Strings are stored INLINE (fixed
// arrays, no pointers) for the same load-time-relocation reason as GtavVehicleEntry.
typedef struct GtavObjectEntry {
  char label[24];    // short menu label (longest in catalog 23)
  char name[32];     // GTA V prop model name (longest observed 23); for logs/debugging
  uint8_t category;  // object category index (see object_categories_generated.h)
  uint32_t hash;     // precomputed joaat(name)
} GtavObjectEntry;

extern const GtavObjectEntry gtav_object_catalog[];
extern const uint32_t gtav_object_catalog_count;

// Self > Scenarios browser list. A scenario travels to the game-thread handler as its
// *catalog index* (the menu row's .param), which then passes `name` straight to
// TASK_START_SCENARIO_IN_PLACE -- the native takes the name as a string, so there is no
// joaat hash column (unlike the vehicle/ped/object catalogs). `category` drives the in-menu
// category filter (mirrors the object-spawner filter). Strings are stored INLINE (fixed
// arrays, no pointers) for the same load-time-relocation reason as GtavVehicleEntry. Generated
// from a curated list; see devtools/generators/generate_scenario_registry.py and
// data/scenarios/gtav-scenario-names.txt.
typedef struct GtavScenarioEntry {
  char label[40];    // friendly menu label (longest in catalog 22)
  char name[48];     // GTA V scenario name for TASK_START_SCENARIO_IN_PLACE (longest observed 35)
  uint8_t category;  // scenario category index (see scenario_categories_generated.h)
} GtavScenarioEntry;

extern const GtavScenarioEntry gtav_scenario_catalog[];
extern const uint32_t gtav_scenario_catalog_count;

// Self > Emotes browser list. Like a scenario, an emote travels to the game-thread emote tick as
// its *catalog index* (the menu row's .param); the tick streams `dict` (REQUEST_ANIM_DICT) and
// plays `clip` (TASK_PLAY_ANIM) -- both passed verbatim as strings, so there is no joaat column.
// Strings are INLINE (no relocation). Generated; see devtools/generators/generate_anim_registry.py
// and data/scenarios/menyoo-PedAnimation.cpp.
typedef struct GtavAnimEntry {
  char label[40];  // friendly menu label
  char dict[48];   // anim dictionary for REQUEST_ANIM_DICT / TASK_PLAY_ANIM
  char clip[48];   // anim clip name for TASK_PLAY_ANIM
} GtavAnimEntry;

extern const GtavAnimEntry gtav_anim_catalog[];
extern const uint32_t gtav_anim_catalog_count;

// Vehicle image-preview registry (used by GTAV_MENU_ENABLE_VEHICLE_PREVIEW). Keyed by the
// vehicle model hash, it carries the streamed texture dict + the texture *inside* that dict to
// DRAW_SPRITE for the highlighted vehicle's thumbnail, plus a display caption. This is SEPARATE
// from gtav_vehicle_catalog on purpose: the spawn catalog only knows model names, and a vehicle's
// preview dict/texture are NOT reliably the model name (a model-named dict can load yet hold no
// model-named texture -> DRAW_SPRITE then renders a white quad). Only vehicles with a verified
// mapping appear here; the render path draws nothing (clean placeholder) for any model not found,
// so an unverified vehicle can never produce the white card. Strings are stored INLINE (fixed
// arrays, no pointers) for the same load-time-relocation reason as GtavVehicleEntry.
typedef struct GtavVehiclePreviewEntry {
  char model[20];  // GTA V model name (matches gtav_vehicle_catalog.model; for logs/breadcrumb)
  uint32_t model_hash;  // joaat(model) -- the lookup key (matches gtav_vehicle_catalog.model_hash)
  char dict[32];        // streamed texture dict: REQUEST_STREAMED_TEXTURE_DICT / DRAW_SPRITE arg 1
  char texture[48];     // texture name INSIDE the dict: DRAW_SPRITE arg 2 (need NOT equal the dict)
  char caption[40];     // display caption drawn under the preview card
} GtavVehiclePreviewEntry;

extern const GtavVehiclePreviewEntry gtav_vehicle_preview_catalog[];
extern const uint32_t gtav_vehicle_preview_catalog_count;

#ifdef __cplusplus
}
#endif
