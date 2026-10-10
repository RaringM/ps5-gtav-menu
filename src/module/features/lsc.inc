// Los Santos Customs (paint, tint, neon, tyre smoke, mods, cosmetics)
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.

static const PaintColor kPaintColors[] = {
    {"Black", 0},   {"White", 134}, {"Silver", 4}, {"Red", 27},  {"Orange", 38},
    {"Yellow", 88}, {"Green", 139}, {"Blue", 64},  {"Gold", 37}, {"Hot Pink", 135},
};

// GTA V vehicle mod-type ids LSC drives. The performance set is installed to its top
// tier; turbo is a toggle mod (TOGGLE_VEHICLE_MOD), not an indexed one.
enum {
  // Indexed cosmetic slots (installed to their top tier by Install Cosmetics).
  GTAV_VEH_MOD_SPOILER = 0,
  GTAV_VEH_MOD_FRONT_BUMPER = 1,
  GTAV_VEH_MOD_REAR_BUMPER = 2,
  GTAV_VEH_MOD_SIDE_SKIRT = 3,
  GTAV_VEH_MOD_EXHAUST = 4,
  GTAV_VEH_MOD_FRAME = 5,
  GTAV_VEH_MOD_GRILLE = 6,
  GTAV_VEH_MOD_HOOD = 7,
  GTAV_VEH_MOD_FENDER = 8,
  GTAV_VEH_MOD_RIGHT_FENDER = 9,
  GTAV_VEH_MOD_ROOF = 10,
  GTAV_VEH_MOD_HORN = 14,  // indexed horn-sound slot
  // Indexed performance slots.
  GTAV_VEH_MOD_ENGINE = 11,
  GTAV_VEH_MOD_BRAKES = 12,
  GTAV_VEH_MOD_TRANSMISSION = 13,
  GTAV_VEH_MOD_SUSPENSION = 15,
  GTAV_VEH_MOD_ARMOR = 16,
  GTAV_VEH_MOD_TURBO = 18,         // toggle mod (TOGGLE_VEHICLE_MOD), not indexed
  GTAV_VEH_MOD_TYRE_SMOKE = 20,    // toggle mod; colour set by SET_VEHICLE_TYRE_SMOKE_COLOR
  GTAV_VEH_MOD_XENON = 22,         // toggle mod (TOGGLE_VEHICLE_MOD): xenon headlights
  GTAV_VEH_MOD_WHEELS = 23,        // indexed wheel design slot
  GTAV_VEH_MOD_LIVERY = 48,        // VMT_LIVERY_MOD: kit liveries (Plates & Livery > Livery)
  GTAV_VEH_MOD_LAST_INDEXED = 16,  // SET_VEHICLE_MOD indexed slots span 0..16
};

// Per-slot mod pickers: one menu row per category, each cycled to any installable tier.
// The tier ceiling is read live from GET_NUM_VEHICLE_MODS, so the range tracks the current
// vehicle (a Sultan and an Adder expose different counts). Order MUST match the
// GTAV_NATIVE_SHELL_ACTION_MOD_* enum block -- range dispatch keys off the offset.
struct VehicleModSlot {
  char label[14];  // inline (no relocation; see feature_catalog.h)
  int slot;        // GET_NUM_VEHICLE_MODS / SET_VEHICLE_MOD slot id
};
static const VehicleModSlot kVehicleModSlots[] = {
    {"Spoiler", GTAV_VEH_MOD_SPOILER},
    {"Front Bumper", GTAV_VEH_MOD_FRONT_BUMPER},
    {"Rear Bumper", GTAV_VEH_MOD_REAR_BUMPER},
    {"Side Skirt", GTAV_VEH_MOD_SIDE_SKIRT},
    {"Exhaust", GTAV_VEH_MOD_EXHAUST},
    {"Frame", GTAV_VEH_MOD_FRAME},
    {"Grille", GTAV_VEH_MOD_GRILLE},
    {"Hood", GTAV_VEH_MOD_HOOD},
    {"Fenders", GTAV_VEH_MOD_FENDER},
    {"Roof", GTAV_VEH_MOD_ROOF},
    {"Engine", GTAV_VEH_MOD_ENGINE},
    {"Brakes", GTAV_VEH_MOD_BRAKES},
    {"Transmission", GTAV_VEH_MOD_TRANSMISSION},
    {"Suspension", GTAV_VEH_MOD_SUSPENSION},
    {"Armor", GTAV_VEH_MOD_ARMOR},
    {"Wheels", GTAV_VEH_MOD_WHEELS},
    {"Horn", GTAV_VEH_MOD_HORN},
};
#define GTAV_NUM_MOD_SLOTS ((uint32_t)(sizeof(kVehicleModSlots) / sizeof(kVehicleModSlots[0])))

// Stable snake_case action names for the mod-slot range (telemetry/logging). Generated
// from the shared action-name list so this order stays locked to kVehicleModSlots and the
// name switch in features.cpp / native_bridge.cpp (see include/gtavmenu/feature_actions.def).
static const char* const kVehicleModSlotActionNames[] = {
#define GTAV_MOD_SLOT_NAME(name) name,
#include "gtavmenu/feature_actions.def"
};

// Staged tier per mod slot, indexed by (action - MOD_SLOT_FIRST). -1 = Stock. Vehicle-
// specific and intentionally NOT persisted to profile.cfg (a "tier 3 spoiler" is a
// different part on every car), unlike the global paint/tint preset indices.
static int32_t g_mod_stage[GTAV_NUM_MOD_SLOTS];
static int g_mod_stage_ready = 0;
// Vehicle handle g_mod_stage was last synced to from the live car. When the player changes
// vehicle (or a one-shot installs mods) this is invalidated so the pickers re-read the truth.
static int g_mod_stage_vehicle = 0;

// RGB presets shared by the neon-underglow and tyre-smoke cyclers. Index 0 is
// "Off" (its rgb is unused; the cycler disables the effect instead of colouring).
struct ColorPreset {
  char label[10];  // inline (no relocation; see feature_catalog.h)
  uint8_t r, g, b;
};
static const ColorPreset kColorPresets[] = {
    {"Off", 0, 0, 0},        {"White", 255, 255, 255}, {"Red", 255, 0, 0},    {"Blue", 0, 80, 255},
    {"Pink", 255, 0, 160},   {"Purple", 140, 0, 255},  {"Green", 0, 255, 60}, {"Cyan", 0, 220, 255},
    {"Orange", 255, 110, 0}, {"Yellow", 255, 230, 0},
};

// Window-tint levels (SET_VEHICLE_WINDOW_TINT id). Curated subset of the 0..6 range.
struct WindowTint {
  char label[12];  // inline (no relocation; see feature_catalog.h)
  int id;
};
static const WindowTint kWindowTints[] = {
    {"None", 0},       {"Limo", 5},        {"Pure Black", 1},
    {"Dark Smoke", 2}, {"Light Smoke", 3}, {"Green", 6},
};

static uint32_t ls_max_performance() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LS_MAX_PERFORMANCE;
  if (!g_n.set_vehicle_mod || !g_n.get_num_vehicle_mods || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle mod natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  // SET_VEHICLE_MOD_KIT(v, 0) must run before any mod slot can be written.
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  static const int kPerfMods[] = {GTAV_VEH_MOD_ENGINE, GTAV_VEH_MOD_BRAKES,
                                  GTAV_VEH_MOD_TRANSMISSION, GTAV_VEH_MOD_SUSPENSION,
                                  GTAV_VEH_MOD_ARMOR};
  for (uint32_t i = 0; i < sizeof(kPerfMods) / sizeof(kPerfMods[0]); ++i) {
    // GET_NUM_VEHICLE_MODS returns the slot count; the top tier is count-1 (-1 = stock).
    int count = invoke_return<int>(g_n.get_num_vehicle_mods, v, kPerfMods[i]);
    if (count > 0) invoke_void(g_n.set_vehicle_mod, v, kPerfMods[i], count - 1, 0);
  }
  if (g_n.toggle_vehicle_mod) invoke_void(g_n.toggle_vehicle_mod, v, GTAV_VEH_MOD_TURBO, 1);
  g_mod_stage_vehicle = 0;  // force the per-slot pickers to re-read the now-modded car
  return ok(action, "max performance installed");
}

// Spawner defaults: apply Spawn Maxed (full performance recipe, mirroring ls_max_performance) and
// Spawn Invincible to a freshly created vehicle. Called from spawn_vehicle_model_hash on the SAME
// spawn-job context, so it uses invoke_*_ctx(worker, ...) -- NOT the worker-direct invoke_void the
// interactive LSC helpers use, because the spawn drain runs on the game thread (worker =
// allow_block = 0 there). A no-op when both toggles are off.
// Defined in saved_vehicles.inc (included after this file): re-applies a pending saved-vehicle mod
// loadout after CREATE_VEHICLE. Returns 1 if it consumed the pending build (so the maxed/invincible
// spawner defaults below are skipped -- the saved build wins).
static int apply_saved_vehicle_build(int worker, int vehicle);

// Every indexed kit slot the engine has: SET_VEHICLE_MOD types 0..49 except the toggle mods 17..22
// (nitrous, turbo, subwoofer, tyre smoke, hydraulics, xenon). The LS Customs "Part Type" / "Part"
// rows walk the slots the current car has parts for, so every part of a car's mod kit is reachable,
// also where no per-slot row exists (right fender, plate holder, interior, engine bay, livery mods).
// `visual`: a kit body part Spawn Upgraded fits (not the performance tiers, horn or wheel designs).
struct VehicleKitSlot {
  char label[14];  // inline (no relocation; see feature_catalog.h)
  uint8_t slot;    // GET_NUM_VEHICLE_MODS / SET_VEHICLE_MOD slot id
  uint8_t visual;
};
static const VehicleKitSlot kVehicleKitSlots[] = {
    {"Spoiler", 0, 1},        {"Front Bumper", 1, 1}, {"Rear Bumper", 2, 1},   {"Side Skirt", 3, 1},
    {"Exhaust", 4, 1},        {"Frame", 5, 1},        {"Grille", 6, 1},        {"Hood", 7, 1},
    {"Fender", 8, 1},         {"Right Fender", 9, 1}, {"Roof", 10, 1},         {"Engine", 11, 0},
    {"Brakes", 12, 0},        {"Transmission", 13, 0}, {"Horn", 14, 0},        {"Suspension", 15, 0},
    {"Armor", 16, 0},         {"Wheels", 23, 0},      {"Rear Wheels", 24, 0},  {"Plate Holder", 25, 1},
    {"Vanity Plate", 26, 1},  {"Trim Design", 27, 1}, {"Ornaments", 28, 1},    {"Dashboard", 29, 1},
    {"Dial Design", 30, 1},   {"Door Speaker", 31, 1}, {"Seats", 32, 1},       {"Steering", 33, 1},
    {"Shifter", 34, 1},       {"Plaque", 35, 1},      {"Speakers", 36, 1},     {"Trunk", 37, 1},
    {"Hydraulics", 38, 1},    {"Engine Block", 39, 1}, {"Air Filter", 40, 1},  {"Struts", 41, 1},
    {"Arch Cover", 42, 1},    {"Aerials", 43, 1},     {"Trim", 44, 1},         {"Tank", 45, 1},
    {"Windows", 46, 1},       {"Right Door", 47, 1},  {"Livery", 48, 1},       {"Lightbar", 49, 1},
};
#define GTAV_NUM_KIT_SLOTS ((uint32_t)(sizeof(kVehicleKitSlots) / sizeof(kVehicleKitSlots[0])))

// Spawn Upgraded's body parts: the top part of every visual kit slot the new car's kit has, then a
// pack-notes line naming the slots fitted (`slot:parts`), so a hardware run can check the kit from
// the host. Same spawn-job context as apply_spawn_vehicle_defaults (invoke_*_ctx).
static void install_kit_visuals_ctx(int worker, int vehicle) {
  char slots[112];
  size_t used = 0;
  int fitted = 0;
  slots[0] = '\0';
  for (uint32_t i = 0; i < GTAV_NUM_KIT_SLOTS; ++i) {
    if (!kVehicleKitSlots[i].visual) continue;
    const int slot = kVehicleKitSlots[i].slot;
    const int count = invoke_return_ctx<int>(worker, g_n.get_num_vehicle_mods, vehicle, slot);
    if (count <= 0) continue;
    invoke_void_ctx(worker, g_n.set_vehicle_mod, vehicle, slot, count - 1, 0);
    ++fitted;
    if (used < sizeof(slots)) {
      const int n = snprintf(slots + used, sizeof(slots) - used, "%s%d:%d", used ? "," : "", slot,
                             count);
      used = n > 0 ? used + (size_t)n : used;
    }
  }
#if GTAV_MENU_ENABLE_CUSTOM_PACKS  // the worker klog (pack-notes) is linked with the pack lane
  const uint32_t model =
      g_n.get_entity_model ? invoke_return_ctx<uint32_t>(worker, g_n.get_entity_model, vehicle) : 0u;
  char line[192];
  snprintf(line, sizeof(line), "GTAVMenu pack spawn upgraded model=0x%08x parts=%d slots=%s",
           (unsigned)model, fitted, fitted ? slots : "none");
  gtav_worker_klog(line);
#else
  (void)fitted;
#endif
}

static void apply_spawn_vehicle_defaults(int worker, int vehicle) {
  if (!vehicle) return;
  if (apply_saved_vehicle_build(worker, vehicle)) return;
  // Spawn Upgraded = Spawn Maxed's performance recipe + every visual part of the car's kit.
  const int upgraded = g_spawn_upgraded_enabled;
  if ((g_spawn_maxed_enabled || upgraded) && g_n.set_vehicle_mod && g_n.get_num_vehicle_mods &&
      g_n.set_vehicle_mod_kit) {
    invoke_void_ctx(worker, g_n.set_vehicle_mod_kit, vehicle, 0);
    static const int kPerfMods[] = {GTAV_VEH_MOD_ENGINE, GTAV_VEH_MOD_BRAKES,
                                    GTAV_VEH_MOD_TRANSMISSION, GTAV_VEH_MOD_SUSPENSION,
                                    GTAV_VEH_MOD_ARMOR};
    for (uint32_t i = 0; i < sizeof(kPerfMods) / sizeof(kPerfMods[0]); ++i) {
      int count = invoke_return_ctx<int>(worker, g_n.get_num_vehicle_mods, vehicle, kPerfMods[i]);
      if (count > 0)
        invoke_void_ctx(worker, g_n.set_vehicle_mod, vehicle, kPerfMods[i], count - 1, 0);
    }
    if (g_n.toggle_vehicle_mod)
      invoke_void_ctx(worker, g_n.toggle_vehicle_mod, vehicle, GTAV_VEH_MOD_TURBO, 1);
    if (upgraded) install_kit_visuals_ctx(worker, vehicle);
  }
  if (g_spawn_invincible_enabled && g_n.set_entity_invincible) {
    invoke_void_ctx(worker, g_n.set_entity_invincible, vehicle, 1);
  }
}

static uint32_t ls_stock_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LS_STOCK_VEHICLE;
  if (!g_n.set_vehicle_mod || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle mod natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  // Revert every indexed mod slot to stock (-1).
  for (int t = 0; t <= GTAV_VEH_MOD_LAST_INDEXED; ++t) {
    invoke_void(g_n.set_vehicle_mod, v, t, -1, 0);
  }
  if (g_n.toggle_vehicle_mod) {
    invoke_void(g_n.toggle_vehicle_mod, v, GTAV_VEH_MOD_TURBO, 0);
    invoke_void(g_n.toggle_vehicle_mod, v, GTAV_VEH_MOD_TYRE_SMOKE, 0);
  }
  if (g_n.set_vehicle_colours) invoke_void(g_n.set_vehicle_colours, v, 0, 0);
  if (g_n.set_vehicle_window_tint) invoke_void(g_n.set_vehicle_window_tint, v, 0);
  if (g_n.set_vehicle_neon_enabled) {
    for (int i = 0; i < 4; ++i) invoke_void(g_n.set_vehicle_neon_enabled, v, i, 0);
  }
  // Reset the staged cycler indices so the menu shows stock values after a reset.
  g_window_tint_index = 0;
  g_neon_index = 0;
  g_tyre_smoke_index = 0;
  // Stock means every per-slot mod picker reads back as "Stock" (-1), not tier 0. We just
  // forced the true state to stock, so mark the stage synced to this car (no re-read needed).
  for (uint32_t i = 0; i < GTAV_NUM_MOD_SLOTS; ++i) g_mod_stage[i] = -1;
  g_mod_stage_ready = 1;
  g_mod_stage_vehicle = v;
  g_xenon_enabled = 0;
  // Note: bulletproof tyres (SET_VEHICLE_TYRES_CAN_BURST) is left as-is by design.
  return ok(action, "vehicle reset to stock");
}

// Apply the staged primary+secondary paint to the current vehicle. Returns the vehicle
// handle (0 if none) so callers can report "not in a vehicle".
static int apply_vehicle_paint() {
  if (!g_n.set_vehicle_colours) return 0;
  int v = current_vehicle();
  if (!v) return 0;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  invoke_void(g_n.set_vehicle_colours, v, kPaintColors[g_paint_primary_index % count].id,
              kPaintColors[g_paint_secondary_index % count].id);
  return v;
}

static uint32_t cycle_paint_primary(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_PRIMARY;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  if (stage_step(&g_paint_primary_index, count, param))
    return ok(action, kPaintColors[g_paint_primary_index].label);
  if (!g_n.set_vehicle_colours) return unavailable(action, "missing paint native");
  if (!apply_vehicle_paint()) return failed(action, "not in a vehicle");
  return ok(action, "primary paint applied");
}

static uint32_t cycle_paint_secondary(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_PAINT_SECONDARY;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  if (stage_step(&g_paint_secondary_index, count, param))
    return ok(action, kPaintColors[g_paint_secondary_index].label);
  if (!g_n.set_vehicle_colours) return unavailable(action, "missing paint native");
  if (!apply_vehicle_paint()) return failed(action, "not in a vehicle");
  return ok(action, "secondary paint applied");
}

// Window tint list cycler. Left/Right stage a level; Cross applies it. Mirrors the
// paint cyclers (staged index, apply on the non-arrow press).
static uint32_t cycle_window_tint(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WINDOW_TINT;
  const uint32_t count = (uint32_t)(sizeof(kWindowTints) / sizeof(kWindowTints[0]));
  if (stage_step(&g_window_tint_index, count, param))
    return ok(action, kWindowTints[g_window_tint_index].label);
  if (!g_n.set_vehicle_window_tint) return unavailable(action, "missing tint native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_window_tint, v, kWindowTints[g_window_tint_index % count].id);
  return ok(action, "window tint applied");
}

// Neon underglow colour cycler. Index 0 ("Off") disables all four neon sides; any
// other preset enables them and sets the RGB colour.
static uint32_t cycle_neon(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_NEON;
  const uint32_t count = (uint32_t)(sizeof(kColorPresets) / sizeof(kColorPresets[0]));
  if (stage_step(&g_neon_index, count, param)) return ok(action, kColorPresets[g_neon_index].label);
  if (!g_n.set_vehicle_neon_enabled) return unavailable(action, "missing neon native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const ColorPreset& c = kColorPresets[g_neon_index % count];
  const int on = (g_neon_index % count) != 0u;
  // Neon indices: 0 left, 1 right, 2 front, 3 back.
  for (int i = 0; i < 4; ++i) invoke_void(g_n.set_vehicle_neon_enabled, v, i, on);
  if (on && g_n.set_vehicle_neon_colour) invoke_void(g_n.set_vehicle_neon_colour, v, c.r, c.g, c.b);
  return ok(action, on ? "neon enabled" : "neon disabled");
}

// Coloured tyre smoke cycler. "Off" disables the tyre-smoke mod; any colour enables
// it (TOGGLE_VEHICLE_MOD slot 20) and tints it. The mod kit must be unlocked first.
static uint32_t cycle_tyre_smoke(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_TYRE_SMOKE;
  const uint32_t count = (uint32_t)(sizeof(kColorPresets) / sizeof(kColorPresets[0]));
  if (stage_step(&g_tyre_smoke_index, count, param))
    return ok(action, kColorPresets[g_tyre_smoke_index].label);
  if (!g_n.toggle_vehicle_mod || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing tyre smoke natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const ColorPreset& c = kColorPresets[g_tyre_smoke_index % count];
  const int on = (g_tyre_smoke_index % count) != 0u;
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  if (on && g_n.set_vehicle_tyre_smoke_color) {
    invoke_void(g_n.set_vehicle_tyre_smoke_color, v, c.r, c.g, c.b);
  }
  invoke_void(g_n.toggle_vehicle_mod, v, GTAV_VEH_MOD_TYRE_SMOKE, on);
  return ok(action, on ? "tyre smoke enabled" : "tyre smoke disabled");
}

// Install top-tier visual mods + wheels on the current vehicle (cosmetic sibling of
// ls_max_performance). Each slot's top tier is GET_NUM_VEHICLE_MODS - 1.
static uint32_t ls_install_cosmetics() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LS_INSTALL_COSMETICS;
  if (!g_n.set_vehicle_mod || !g_n.get_num_vehicle_mods || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle mod natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  static const int kCosmeticMods[] = {
      GTAV_VEH_MOD_SPOILER,      GTAV_VEH_MOD_FRONT_BUMPER, GTAV_VEH_MOD_REAR_BUMPER,
      GTAV_VEH_MOD_SIDE_SKIRT,   GTAV_VEH_MOD_EXHAUST,      GTAV_VEH_MOD_FRAME,
      GTAV_VEH_MOD_GRILLE,       GTAV_VEH_MOD_HOOD,         GTAV_VEH_MOD_FENDER,
      GTAV_VEH_MOD_RIGHT_FENDER, GTAV_VEH_MOD_ROOF,         GTAV_VEH_MOD_WHEELS};
  for (uint32_t i = 0; i < sizeof(kCosmeticMods) / sizeof(kCosmeticMods[0]); ++i) {
    int count = invoke_return<int>(g_n.get_num_vehicle_mods, v, kCosmeticMods[i]);
    if (count > 0) invoke_void(g_n.set_vehicle_mod, v, kCosmeticMods[i], count - 1, 0);
  }
  g_mod_stage_vehicle = 0;  // force the per-slot pickers to re-read the now-modded car
  return ok(action, "cosmetics installed");
}

// Make the current vehicle's tyres non-burstable.
static uint32_t ls_bulletproof_tyres() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LS_BULLETPROOF_TYRES;
  if (!g_n.set_vehicle_tyres_can_burst) return unavailable(action, "missing tyre native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_tyres_can_burst, v, 0);
  return ok(action, "bulletproof tyres on");
}

// GTA vehicles carry up to 8 wheel slots; per the native docs you burst/fix all tyres by
// calling the per-tyre native for index 0..7 (extra indices on a 4-wheeler are no-ops).
enum { GTAV_VEHICLE_TYRE_SLOTS = 8 };

// Pop every tyre on the current vehicle. Worker-safe modify-existing setter, same class as
// the bulletproof toggle. SET_VEHICLE_TYRE_BURST(vehicle, index, onRim, damage): onRim=1 so
// the car runs on the rims (a proper blowout), damage 1000 = fully burst.
static uint32_t burst_tyres() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_BURST_TYRES;
  if (!g_n.set_vehicle_tyre_burst) return unavailable(action, "missing tyre native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  // Make the tyres burstable first (a bulletproofed car would otherwise ignore the burst).
  if (g_n.set_vehicle_tyres_can_burst) invoke_void(g_n.set_vehicle_tyres_can_burst, v, 1);
  for (int i = 0; i < GTAV_VEHICLE_TYRE_SLOTS; ++i) {
    invoke_void(g_n.set_vehicle_tyre_burst, v, i, 1, 1000.0f);
  }
  return ok(action, "tyres burst");
}

// Repair every burst tyre on the current vehicle (without fixing the rest of the car, unlike
// Fix Vehicle). Worker-safe per-tyre setter.
static uint32_t fix_tyres() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_FIX_TYRES;
  if (!g_n.set_vehicle_tyre_fixed) return unavailable(action, "missing tyre native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  for (int i = 0; i < GTAV_VEHICLE_TYRE_SLOTS; ++i) {
    invoke_void(g_n.set_vehicle_tyre_fixed, v, i);
  }
  return ok(action, "tyres repaired");
}

// Lazily set every per-slot picker to Stock (-1). Zero-init would read as tier 0, which
// misrepresents a fresh/stock vehicle, so the first access seeds -1.
static void ensure_mod_stage_init() {
  if (g_mod_stage_ready) return;
  for (uint32_t i = 0; i < GTAV_NUM_MOD_SLOTS; ++i) g_mod_stage[i] = -1;
  g_mod_stage_ready = 1;
}

// Installable tier count for a mod slot on the CURRENT vehicle (0 if no vehicle / native
// missing). The picker range is -1..count-1, so count==0 leaves only "Stock".
static int live_mod_count(int slot) {
  if (!g_n.get_num_vehicle_mods) return 0;
  int v = current_vehicle();
  if (!v) return 0;
  int c = invoke_return<int>(g_n.get_num_vehicle_mods, v, slot);
  return c > 0 ? c : 0;
}

// Seed every per-slot picker from what is ACTUALLY installed on the current vehicle via
// GET_VEHICLE_MOD, so the LSC rows reflect reality rather than a tier staged for some other
// car. Cheap in steady state: it only re-reads when the vehicle handle changes (or a one-shot
// invalidated g_mod_stage_vehicle). All plain getters/field stores -> worker-safe. Falls back
// to lazy Stock-init when GET_VEHICLE_MOD is unavailable. SET_VEHICLE_MOD_KIT must run first
// to unlock the slots for reading.
static void sync_mod_stage_to_vehicle() {
  if (!g_n.get_vehicle_mod) {
    ensure_mod_stage_init();
    return;
  }
  int v = current_vehicle();
  if (!v) return;  // no car: keep whatever we had rather than blanking the rows
  if (g_mod_stage_ready && v == g_mod_stage_vehicle) return;
  if (g_n.set_vehicle_mod_kit) invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  for (uint32_t i = 0; i < GTAV_NUM_MOD_SLOTS; ++i) {
    int lvl = invoke_return<int>(g_n.get_vehicle_mod, v, kVehicleModSlots[i].slot);
    g_mod_stage[i] = (lvl < 0) ? -1 : lvl;  // GET_VEHICLE_MOD returns -1 for Stock
  }
  g_mod_stage_vehicle = v;
  g_mod_stage_ready = 1;
}

// Display label for a staged tier: "Stock" (-1) or 1-based "Noun N/M" (M = live count). The
// noun is per-slot: the Wheels slot selects a wheel DESIGN (a cosmetic variation), not an
// upgrade tier, so it reads "Design N/M"; the performance slots read "Level N/M".
static const char* mod_level_label(int slot, int lvl, int count) {
  if (lvl < 0) return "Stock";
  // Scratch wide enough that the formatter can't truncate (-Wformat-truncation); the
  // short "Noun N/M" result is re-copied into ShellListValue.value[24] by set_list_value.
  static char buf[32];
  // The part's own name when the car's mod kit labels it (retail kits, and pack kits through the
  // pack's shop labels): GET_MOD_TEXT_LABEL gives the text key, GET_FILENAME_FOR_AUDIO_CONVERSATION
  // its text in a shared game buffer, copied at once. Both are worker-safe getters. Unlabelled
  // parts (and kits whose key has no text) keep the generic "Noun N/M".
  const int v = (g_n.get_mod_text_label && g_n.get_filename_for_audio_conversation)
                    ? current_vehicle()
                    : 0;
  const char* key = v ? invoke_return<const char*>(g_n.get_mod_text_label, v, slot, lvl) : nullptr;
  if (key && key[0] && strcmp(key, "NULL") != 0) {
    const char* text = invoke_return<const char*>(g_n.get_filename_for_audio_conversation, key);
    if (text && text[0] && strcmp(text, "NULL") != 0) {
      // Wheels: a wheel type lists 50+ designs (retail Sport alone has 50, the shop shows 25) and
      // pack wheels are appended at the end, so keep the position next to the name.
      if (slot == GTAV_VEH_MOD_WHEELS) {
        snprintf(buf, sizeof(buf), "%d/%d %s", lvl + 1, count, text);
      } else {
        snprintf(buf, sizeof(buf), "%s", text);
      }
      return buf;
    }
  }
  const char* format =
      slot == GTAV_VEH_MOD_WHEELS   ? locale_text_or("value.design_level", "Design %d/%d")
      : slot == GTAV_VEH_MOD_LIVERY ? locale_text_or("value.kit_livery_level", "Kit Livery %d/%d")
                                    : locale_text_or("value.upgrade_level", "Level %d/%d");
  snprintf(buf, sizeof(buf), format, lvl + 1, count);
  return buf;
}

// Generic per-slot mod cycler, shared by every kVehicleModSlots[] row. Stage (Left/Right)
// walks the -1..count-1 domain with wraparound; apply (Select) writes SET_VEHICLE_MOD.
// Tiers are clamped against the live vehicle so a value carried over from another car
// (with fewer tiers) can never install an out-of-range mod.
static uint32_t cycle_vehicle_mod(uint32_t idx, uint32_t param) {
  sync_mod_stage_to_vehicle();  // start from the car's actual installed tiers
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_MOD_SLOT_FIRST + idx;
  const int slot = kVehicleModSlots[idx].slot;
  if (param == 1u || param == 2u) {
    const int count = live_mod_count(slot);
    const int span = count + 1;  // domain -1..count-1 (Stock always exists) => count+1 values
    int cur = g_mod_stage[idx];
    if (cur < -1 || cur > count - 1) cur = -1;  // clamp a tier carried over from another vehicle
    int u = cur + 1;                            // map -1..count-1 -> 0..count
    u = (u + (param == 2u ? -1 : 1) + span) % span;
    g_mod_stage[idx] = u - 1;
    return ok(action, mod_level_label(slot, g_mod_stage[idx], count));
  }
  if (!g_n.set_vehicle_mod || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle mod natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  int count = g_n.get_num_vehicle_mods ? invoke_return<int>(g_n.get_num_vehicle_mods, v, slot) : 0;
  int lvl = g_mod_stage[idx];
  if (lvl > count - 1) lvl = count - 1;  // re-clamp at apply against the live vehicle
  if (lvl < -1) lvl = -1;
  invoke_void(g_n.set_vehicle_mod, v, slot, lvl, 0);
  g_mod_stage[idx] = lvl;  // keep the displayed tier in step with what was actually written
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s: %s", kVehicleModSlots[idx].label,
           lvl < 0 ? "stock" : "installed");
  return ok(action, msg);
}

// Xenon headlights toggle: TOGGLE_VEHICLE_MOD slot 22 after unlocking the mod kit. Reuses
// natives already bound; the menu shows its state via the feature-toggle bitmask.
static uint32_t toggle_xenon_lights() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_XENON;
  if (!g_n.toggle_vehicle_mod || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle mod natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  g_xenon_enabled = !g_xenon_enabled;
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  invoke_void(g_n.toggle_vehicle_mod, v, GTAV_VEH_MOD_XENON, g_xenon_enabled);
  return ok(action, g_xenon_enabled ? "xenon headlights on" : "xenon headlights off");
}

// ---------------------------------------------------------------------------
// LS Customs expansion 2: wheel type, liveries, plates, extra/custom colours,
// rainbow neon. All worker-safe modify-existing setters, same stage/apply cycler
// shape as the paint/tint rows above (param 1/2 = stage prev/next, 0 = apply).
// ---------------------------------------------------------------------------

// Wheel categories driven by SET_VEHICLE_WHEEL_TYPE. Picking a category changes
// which designs the existing Wheels mod picker (slot 23) then exposes.
struct VehicleWheelType {
  char label[12];  // inline (no relocation; see feature_catalog.h)
  int id;
};
static const VehicleWheelType kWheelTypes[] = {
    {"Sport", 0},    {"Muscle", 1},    {"Lowrider", 2},  {"SUV", 3},     {"Offroad", 4},
    {"Tuner", 5},    {"Bike", 6},      {"High End", 7},  {"Benny's", 8}, {"Benny's 2", 9},
};

// Number-plate styles (SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX id == array index).
struct PlateStyle {
  char label[14];  // inline (no relocation)
};
static const PlateStyle kPlateStyles[] = {
    {"Blue/White"}, {"Yellow/Black"}, {"Yellow/Blue"}, {"Blue/White 2"}, {"Yankton"},
};

// Curated number-plate text presets. Inline 2D char array (no pointer relocation);
// GTA plates hold up to 8 glyphs, so every preset stays <= 8 characters.
static const char kPlatePresets[][12] = {
    "GTAV", "LSCUSTOM", "MODMENU", "ROCKSTAR", "SANANDRS", "OGLOC",
};

// Vivid true-RGB palette for SET_VEHICLE_CUSTOM_PRIMARY/SECONDARY_COLOUR. Reuses the
// ColorPreset {label,r,g,b} shape; these are tones the indexed paint palette can't hit.
static const ColorPreset kCustomColors[] = {
    {"Crimson", 220, 20, 30}, {"Sunset", 255, 90, 0},  {"Gold", 255, 200, 20},
    {"Lime", 130, 255, 20},   {"Emerald", 0, 200, 90}, {"Teal", 0, 200, 200},
    {"Azure", 0, 120, 255},   {"Violet", 150, 0, 255}, {"Magenta", 255, 0, 200},
    {"Chrome", 200, 210, 220},
};

// Full-saturation hue -> RGB (S=V=1), six 60-degree segments. Drives the rainbow neon.
static void hue_to_rgb(uint32_t hue, uint8_t* r, uint8_t* g, uint8_t* b) {
  hue %= 360u;
  const uint8_t up = (uint8_t)((hue % 60u) * 255u / 60u);
  const uint8_t dn = (uint8_t)(255u - up);
  switch (hue / 60u) {
    case 0:
      *r = 255, *g = up, *b = 0;
      break;
    case 1:
      *r = dn, *g = 255, *b = 0;
      break;
    case 2:
      *r = 0, *g = 255, *b = up;
      break;
    case 3:
      *r = 0, *g = dn, *b = 255;
      break;
    case 4:
      *r = up, *g = 0, *b = 255;
      break;
    default:
      *r = 255, *g = 0, *b = dn;
      break;
  }
}

static uint32_t cycle_wheel_type(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_TYPE;
  const uint32_t count = (uint32_t)(sizeof(kWheelTypes) / sizeof(kWheelTypes[0]));
  if (stage_step(&g_wheel_type_index, count, param))
    return ok(action, kWheelTypes[g_wheel_type_index].label);
  if (!g_n.set_vehicle_wheel_type || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing wheel natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  invoke_void(g_n.set_vehicle_wheel_type, v, kWheelTypes[g_wheel_type_index % count].id);
  return ok(action, "wheel type applied");
}

static uint32_t cycle_plate_style(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_STYLE;
  const uint32_t count = (uint32_t)(sizeof(kPlateStyles) / sizeof(kPlateStyles[0]));
  if (stage_step(&g_plate_style_index, count, param))
    return ok(action, kPlateStyles[g_plate_style_index].label);
  if (!g_n.set_vehicle_number_plate_text_index) return unavailable(action, "missing plate native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_number_plate_text_index, v, (int)(g_plate_style_index % count));
  return ok(action, "plate style applied");
}

static uint32_t cycle_plate_text(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_PLATE_TEXT;
  const uint32_t count = (uint32_t)(sizeof(kPlatePresets) / sizeof(kPlatePresets[0]));
  if (stage_step(&g_plate_text_index, count, param))
    return ok(action, kPlatePresets[g_plate_text_index]);
  if (!g_n.set_vehicle_number_plate_text) return unavailable(action, "missing plate native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const char* text = kPlatePresets[g_plate_text_index % count];
  invoke_void(g_n.set_vehicle_number_plate_text, v, text);
  return ok(action, "plate text applied");
}

// Apply the staged pearlescent + wheel colours together (one SET_VEHICLE_EXTRA_COLOURS
// call carries both). Returns the vehicle handle (0 if none) like apply_vehicle_paint().
static int apply_vehicle_extra_colours() {
  if (!g_n.set_vehicle_extra_colours) return 0;
  int v = current_vehicle();
  if (!v) return 0;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  invoke_void(g_n.set_vehicle_extra_colours, v, kPaintColors[g_pearl_color_index % count].id,
              kPaintColors[g_wheel_color_index % count].id);
  return v;
}

static uint32_t cycle_pearlescent(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_PEARLESCENT;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  if (stage_step(&g_pearl_color_index, count, param))
    return ok(action, kPaintColors[g_pearl_color_index].label);
  if (!g_n.set_vehicle_extra_colours) return unavailable(action, "missing extra-colour native");
  if (!apply_vehicle_extra_colours()) return failed(action, "not in a vehicle");
  return ok(action, "pearlescent applied");
}

static uint32_t cycle_wheel_color(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WHEEL_COLOR;
  const uint32_t count = (uint32_t)(sizeof(kPaintColors) / sizeof(kPaintColors[0]));
  if (stage_step(&g_wheel_color_index, count, param))
    return ok(action, kPaintColors[g_wheel_color_index].label);
  if (!g_n.set_vehicle_extra_colours) return unavailable(action, "missing extra-colour native");
  if (!apply_vehicle_extra_colours()) return failed(action, "not in a vehicle");
  return ok(action, "wheel colour applied");
}

static uint32_t cycle_custom_primary(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_PRIMARY;
  const uint32_t count = (uint32_t)(sizeof(kCustomColors) / sizeof(kCustomColors[0]));
  if (stage_step(&g_custom_primary_index, count, param))
    return ok(action, kCustomColors[g_custom_primary_index].label);
  if (!g_n.set_vehicle_custom_primary_colour) {
    return unavailable(action, "missing custom-colour native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const ColorPreset& c = kCustomColors[g_custom_primary_index % count];
  invoke_void(g_n.set_vehicle_custom_primary_colour, v, c.r, c.g, c.b);
  return ok(action, "custom primary applied");
}

static uint32_t cycle_custom_secondary(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CUSTOM_SECONDARY;
  const uint32_t count = (uint32_t)(sizeof(kCustomColors) / sizeof(kCustomColors[0]));
  if (stage_step(&g_custom_secondary_index, count, param))
    return ok(action, kCustomColors[g_custom_secondary_index].label);
  if (!g_n.set_vehicle_custom_secondary_colour) {
    return unavailable(action, "missing custom-colour native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const ColorPreset& c = kCustomColors[g_custom_secondary_index % count];
  invoke_void(g_n.set_vehicle_custom_secondary_colour, v, c.r, c.g, c.b);
  return ok(action, "custom secondary applied");
}

// Rainbow neon: a toggle whose colour is driven per worker tick by rainbow_neon_tick().
// Toggling on enables all four neon sides; the per-tick driver does the hue sweep.
static uint32_t toggle_rainbow_neon() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_NEON;
  if (!g_n.set_vehicle_neon_colour || !g_n.set_vehicle_neon_enabled) {
    return unavailable(action, "missing neon natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  g_rainbow_neon_enabled = !g_rainbow_neon_enabled;
  if (g_rainbow_neon_enabled) {
    for (int i = 0; i < 4; ++i) invoke_void(g_n.set_vehicle_neon_enabled, v, i, 1);
  }
  return ok(action, g_rainbow_neon_enabled ? "rainbow neon on" : "rainbow neon off");
}

// Per-tick neon hue sweep, called from gtav_features_worker_tick while enabled. Uses the
// strict vehicle (no stale fallback) so it only paints the car the player is actually in.
static void rainbow_neon_tick() {
  if (!g_rainbow_neon_enabled) return;
  if (!g_n.set_vehicle_neon_colour || !g_n.set_vehicle_neon_enabled) return;
  int v = current_vehicle_strict();
  if (!v) return;
  g_rainbow_neon_phase = (g_rainbow_neon_phase + 6u) % 360u;
  uint8_t r, g, b;
  hue_to_rgb(g_rainbow_neon_phase, &r, &g, &b);
  for (int i = 0; i < 4; ++i) invoke_void(g_n.set_vehicle_neon_enabled, v, i, 1);
  invoke_void(g_n.set_vehicle_neon_colour, v, r, g, b);
}

// Rainbow body-paint: a toggle whose colour is driven per worker tick by
// rainbow_paint_tick(), same shape as rainbow neon but resprays the custom-RGB primary
// (and secondary) instead of the underglow. Worker-safe modify-existing setters.
static uint32_t toggle_rainbow_paint() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_RAINBOW_PAINT;
  if (!g_n.set_vehicle_custom_primary_colour) {
    return unavailable(action, "missing paint native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  g_rainbow_paint_enabled = !g_rainbow_paint_enabled;
  return ok(action, g_rainbow_paint_enabled ? "rainbow paint on" : "rainbow paint off");
}

// Per-tick body-paint hue sweep, called from gtav_features_worker_tick while enabled. Uses
// the strict vehicle (no stale fallback) so it only paints the car the player is in.
static void rainbow_paint_tick() {
  if (!g_rainbow_paint_enabled) return;
  if (!g_n.set_vehicle_custom_primary_colour) return;
  int v = current_vehicle_strict();
  if (!v) return;
  g_rainbow_paint_phase = (g_rainbow_paint_phase + 6u) % 360u;
  uint8_t r, g, b;
  hue_to_rgb(g_rainbow_paint_phase, &r, &g, &b);
  invoke_void(g_n.set_vehicle_custom_primary_colour, v, r, g, b);
  if (g_n.set_vehicle_custom_secondary_colour) {
    invoke_void(g_n.set_vehicle_custom_secondary_colour, v, r, g, b);
  }
}

// ---------------------------------------------------------------------------
// Every kit slot: "Part Type" (list_kinds.def SETTING) picks one of the slots the current car has
// parts for (kVehicleKitSlots, the slots without parts are skipped; "None" when the car has none),
// and "Part" (CHOICE) stages one of that slot's parts, Cross fits it. Part names come from the kit
// (GET_MOD_TEXT_LABEL, mod_level_label) like the per-slot rows. Worker-safe getters/setters.
// ---------------------------------------------------------------------------

static uint32_t g_kit_slot_index;     // kVehicleKitSlots row of the Part Type row
static int32_t g_kit_part_stage = -1;  // Part row's staged part (-1 = stock)
static int g_kit_part_vehicle;        // vehicle and slot the staged part was read from
static int g_kit_part_slot = -1;
static int g_kit_vehicle;             // vehicle whose mod kit was last unlocked for reading

// Parts of mod slot `slot` on vehicle `v` (0 = none). The kit is unlocked once per vehicle
// (SET_VEHICLE_MOD_KIT must precede the counts).
static int kit_mod_count(int v, int slot) {
  if (!v || !g_n.get_num_vehicle_mods) return 0;
  if (v != g_kit_vehicle && g_n.set_vehicle_mod_kit) {
    invoke_void(g_n.set_vehicle_mod_kit, v, 0);
    g_kit_vehicle = v;
  }
  const int count = invoke_return<int>(g_n.get_num_vehicle_mods, v, slot);
  return count > 0 ? count : 0;
}

// Parts of kit slot row `i` on vehicle `v` (0 = none).
static int kit_slot_parts(int v, uint32_t i) {
  return i < GTAV_NUM_KIT_SLOTS ? kit_mod_count(v, (int)kVehicleKitSlots[i].slot) : 0;
}

// The Part Type row on vehicle `v`: the selected slot when it has parts, else the next one that
// has (stepping `dir` = +1 / -1 from the selected one), else -1 (the car's kit has no parts).
static int kit_slot_pick(int v, int dir) {
  for (uint32_t n = 0; n < GTAV_NUM_KIT_SLOTS; ++n) {
    const uint32_t i = dir >= 0 ? (g_kit_slot_index + n) % GTAV_NUM_KIT_SLOTS
                                : (g_kit_slot_index + GTAV_NUM_KIT_SLOTS - n) % GTAV_NUM_KIT_SLOTS;
    if (kit_slot_parts(v, i) > 0) return (int)i;
  }
  return -1;
}

// The Part row's stage follows the car: re-read from GET_VEHICLE_MOD when the vehicle or slot moved.
static void kit_part_sync(int v, int slot) {
  if (v == g_kit_part_vehicle && slot == g_kit_part_slot) return;
  const int lvl = g_n.get_vehicle_mod ? invoke_return<int>(g_n.get_vehicle_mod, v, slot) : -1;
  g_kit_part_stage = lvl < 0 ? -1 : lvl;
  g_kit_part_vehicle = v;
  g_kit_part_slot = slot;
}

static const char* kit_slot_value_label() {
  const int i = kit_slot_pick(current_vehicle(), 1);
  return i < 0 ? "None" : kVehicleKitSlots[i].label;
}

static const char* kit_part_value_label() {
  const int v = current_vehicle();
  const int i = kit_slot_pick(v, 1);
  if (i < 0) return "None";
  const int slot = kVehicleKitSlots[i].slot;
  const int count = kit_slot_parts(v, (uint32_t)i);
  kit_part_sync(v, slot);
  int lvl = g_kit_part_stage;
  if (lvl > count - 1) lvl = count - 1;
  return mod_level_label(slot, lvl < -1 ? -1 : lvl, count);
}

// Part Type: Left/Right (param 1 / 2) step to the next / previous slot with parts (a SETTING row:
// Cross never reaches here).
static uint32_t cycle_kit_slot(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_KIT_SLOT;
  const int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  if (!g_n.get_num_vehicle_mods) return unavailable(action, "missing vehicle mod natives");
  const int dir = param == 2u ? -1 : 1;
  if (kit_slot_pick(v, 1) < 0) return failed(action, "this car has no kit parts");
  g_kit_slot_index = (g_kit_slot_index + GTAV_NUM_KIT_SLOTS + (uint32_t)dir) % GTAV_NUM_KIT_SLOTS;
  const int i = kit_slot_pick(v, dir);
  g_kit_slot_index = (uint32_t)i;
  return ok(action, kVehicleKitSlots[i].label);
}

// Part: Left/Right stage a part of the selected slot (-1..parts-1, wrapping); Cross fits it.
static uint32_t cycle_kit_part(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_KIT_PART;
  const int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  if (!g_n.get_num_vehicle_mods || !g_n.set_vehicle_mod || !g_n.set_vehicle_mod_kit)
    return unavailable(action, "missing vehicle mod natives");
  const int i = kit_slot_pick(v, 1);
  if (i < 0) return failed(action, "this car has no kit parts");
  g_kit_slot_index = (uint32_t)i;
  const int slot = kVehicleKitSlots[i].slot;
  const int count = kit_slot_parts(v, (uint32_t)i);
  kit_part_sync(v, slot);
  int lvl = g_kit_part_stage;
  if (lvl > count - 1) lvl = count - 1;  // the value kit_part_value_label shows
  if (lvl < -1) lvl = -1;
  if (param == 1u || param == 2u) {
    const int span = count + 1;  // -1..count-1
    g_kit_part_stage = (lvl + 1 + (param == 2u ? -1 : 1) + span) % span - 1;
    return ok(action, mod_level_label(slot, g_kit_part_stage, count));
  }
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  invoke_void(g_n.set_vehicle_mod, v, slot, lvl, 0);
  g_kit_part_stage = lvl;
  g_mod_stage_vehicle = 0;  // the per-slot rows re-read the car
  g_livery_vehicle = 0;     // and the Livery row (a slot-48 part is a kit livery)
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s: %s", kVehicleKitSlots[i].label, lvl < 0 ? "stock" : "fitted");
  return ok(action, msg);
}

// ---------------------------------------------------------------------------
// Plates & Livery > Livery (2026-10-08 session 3): the car's texture liveries (SET_VEHICLE_LIVERY,
// GET_VEHICLE_LIVERY_COUNT), then its kit liveries (VMT_LIVERY_MOD parts of mod slot 48, named by
// the kit like LS Customs > Part Type: Livery and fitted with the same SET_VEHICLE_MOD). One CHOICE
// domain -1..textures+kit-1: -1 None (removes both), then the textures, then the kit parts.
// Converted cars often have only kit liveries (S3: the operator looked for them here first).
// Worker-safe getters/setters, like the rest of LS Customs.
// ---------------------------------------------------------------------------

// Installable texture livery count for the CURRENT vehicle (0 if none / native missing).
static int live_livery_count() {
  if (!g_n.get_vehicle_livery_count) return 0;
  int v = current_vehicle();
  if (!v) return 0;
  int c = invoke_return<int>(g_n.get_vehicle_livery_count, v);
  return c > 0 ? c : 0;
}

// Label for livery pick `lvl`: "None", 1-based "Livery N/M" for a texture livery, else the kit
// part's own name (mod_level_label: "Kit Livery N/M" when the kit has no name for it).
static const char* livery_label(int lvl, int textures, int kit) {
  if (lvl < 0 || textures + kit <= 0) return "None";
  if (lvl >= textures) return mod_level_label(GTAV_VEH_MOD_LIVERY, lvl - textures, kit);
  // Scratch wide enough that the formatter can't truncate (-Wformat-truncation); the
  // short result is re-copied into ShellListValue.value[24] by set_list_value.
  static char buf[40];
  snprintf(buf, sizeof(buf), locale_text_or("value.livery_level", "Livery %d/%d"), lvl + 1,
           textures);
  return buf;
}

// The Livery row's domain on vehicle `v` (*textures, *kit) and its pick clamped to it. The pick
// follows the car: on another vehicle it starts at the fitted kit livery (GET_VEHICLE_MOD 48), else
// None (no reviewed getter reads the texture livery).
static int livery_state(int v, int* textures, int* kit) {
  *textures = live_livery_count();
  *kit = kit_mod_count(v, GTAV_VEH_MOD_LIVERY);
  if (v != g_livery_vehicle) {
    const int fitted = *kit && g_n.get_vehicle_mod
                           ? invoke_return<int>(g_n.get_vehicle_mod, v, GTAV_VEH_MOD_LIVERY)
                           : -1;
    g_livery_stage = fitted >= 0 ? *textures + fitted : -1;
    g_livery_vehicle = v;
  }
  int lvl = g_livery_stage;
  if (lvl > *textures + *kit - 1) lvl = *textures + *kit - 1;
  return lvl < -1 ? -1 : lvl;
}

static const char* livery_value_label() {
  int textures = 0, kit = 0;
  const int lvl = livery_state(current_vehicle(), &textures, &kit);
  return livery_label(lvl, textures, kit);
}

// Left/Right stage a pick (wrapping), Cross applies it: None clears the texture livery and the kit
// livery; a texture livery is SET_VEHICLE_LIVERY; a kit livery is SET_VEHICLE_MOD(48). A pack-notes
// line reads the slot back (pack lane builds: the worker klog is linked with it).
static uint32_t cycle_livery(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_LIVERY;
  const int v = current_vehicle();
  int textures = 0, kit = 0;
  const int lvl = livery_state(v, &textures, &kit);
  if (param == 1u || param == 2u) {
    const int span = textures + kit + 1;  // -1..textures+kit-1
    g_livery_stage = (lvl + 1 + (param == 2u ? -1 : 1) + span) % span - 1;
    return ok(action, livery_label(g_livery_stage, textures, kit));
  }
  if (!g_n.set_vehicle_livery || !g_n.set_vehicle_mod)
    return unavailable(action, "missing livery native");
  if (!v) return failed(action, "not in a vehicle");
  if (g_n.set_vehicle_mod_kit) invoke_void(g_n.set_vehicle_mod_kit, v, 0);
  if (lvl < textures) invoke_void(g_n.set_vehicle_livery, v, lvl);  // -1 removes it
  if (kit && lvl < 0) invoke_void(g_n.set_vehicle_mod, v, GTAV_VEH_MOD_LIVERY, -1, 0);
  if (lvl >= textures) invoke_void(g_n.set_vehicle_mod, v, GTAV_VEH_MOD_LIVERY, lvl - textures, 0);
  g_livery_stage = lvl;
  g_mod_stage_vehicle = 0;  // the per-slot rows and Part re-read the car
  g_kit_part_vehicle = 0;
#if GTAV_MENU_ENABLE_CUSTOM_PACKS  // the worker klog (pack-notes) is linked with the pack lane
  const int fitted =
      g_n.get_vehicle_mod ? invoke_return<int>(g_n.get_vehicle_mod, v, GTAV_VEH_MOD_LIVERY) : -2;
  const uint32_t model =
      g_n.get_entity_model ? invoke_return<uint32_t>(g_n.get_entity_model, v) : 0u;
  char line[128];
  snprintf(line, sizeof(line),
           "GTAVMenu pack livery model=0x%08x pick=%d textures=%d kit=%d mod48=%d",
           (unsigned)model, lvl, textures, kit, fitted);
  gtav_worker_klog(line);
#endif
  return ok(action, lvl < 0          ? "livery removed"
                    : lvl < textures ? "livery applied"
                                     : "kit livery fitted");
}

// Spawn Upgraded (Vehicles > Spawn Options): a flag apply_spawn_vehicle_defaults reads at create
// time, like Spawn Maxed; no native at toggle time.
static uint32_t toggle_spawn_upgraded() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SPAWN_UPGRADED;
  g_spawn_upgraded_enabled = !g_spawn_upgraded_enabled;
  return ok(action, g_spawn_upgraded_enabled ? "new vehicles spawn upgraded"
                                             : "spawn upgraded disabled");
}
