// Weapons > Attachments: add/remove components (suppressor, scope, grip, extended clip, flashlight)
// on the player's currently-equipped weapon.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace AFTER
// weapon_tint.inc (reuses stage_step, native_player_ped_id, ok/failed/unavailable, g_n + the invoke
// helpers, GTAV_FEATURE_MESSAGE_LEN). Not a standalone TU.
//
// The five per-slot rows are worker-side STAGING cyclers (Off/On) -- like the LSC mod-tier rows --
// because GIVE_WEAPON_COMPONENT_TO_PED / REMOVE_WEAPON_COMPONENT_FROM_PED walk the ped weapon
// manager and must run on the GAME thread (same lane as GIVE_WEAPON_TO_PED). "Apply Attachments"
// commits the staged set to the current weapon on the game-thread gated lane; "Remove All
// Attachments" strips them. The component table maps a weapon to its canonical component per slot
// (0 = the weapon doesn't accept that slot). Component hashes are joaat() of the standard GTA V
// component names, verified against published values; an unsupported (weapon, component) pair is a
// harmless no-op in the native, so a curated table degrades gracefully. MK2 weapons use a separate
// livery/camo system and are intentionally omitted (their rows simply find no table entry).

enum {
  GTAV_WATTACH_SUPP = 0,
  GTAV_WATTACH_SCOPE,
  GTAV_WATTACH_GRIP,
  GTAV_WATTACH_CLIP,
  GTAV_WATTACH_FLASH,
  GTAV_WATTACH_SLOT_COUNT,
};

static const struct WAttachRow {
  uint32_t weapon;
  uint32_t comp[GTAV_WATTACH_SLOT_COUNT];  // supp, scope, grip, clip, flash (0 = unsupported)
} kWAttach[] = {
    {0x1b06d571u, {0x65ea7ebbu, 0u, 0u, 0xed265a1cu, 0x359b7aaeu}},          // PISTOL
    {0x5ef9fec4u, {0xc304849au, 0u, 0u, 0xd67b4f2du, 0x359b7aaeu}},          // COMBATPISTOL
    {0x22d8fe39u, {0xc304849au, 0u, 0u, 0x249a17d5u, 0x359b7aaeu}},          // APPISTOL
    {0x99aeeb3bu, {0xa73d4664u, 0u, 0u, 0xd9d3ac92u, 0x359b7aaeu}},          // PISTOL50
    {0xd205520eu, {0xc304849au, 0u, 0u, 0x64f9c62bu, 0x359b7aaeu}},          // HEAVYPISTOL
    {0xbfd21232u, {0x65ea7ebbu, 0u, 0u, 0x7b0033b3u, 0u}},                   // SNSPISTOL
    {0x083839c4u, {0xc304849au, 0u, 0u, 0x33ba12e8u, 0u}},                   // VINTAGEPISTOL
    {0x13532244u, {0xa73d4664u, 0x9d2fbf29u, 0u, 0x10e6ba2bu, 0x359b7aaeu}}, // MICROSMG
    {0x2be6766bu, {0xc304849au, 0x3cc6ba57u, 0u, 0x350966fbu, 0x7bc4cddcu}}, // SMG
    {0xefe7e2dfu, {0xa73d4664u, 0x9d2fbf29u, 0u, 0xbb46e417u, 0x7bc4cddcu}}, // ASSAULTSMG
    {0x0a3d4d34u, {0u, 0x9d2fbf29u, 0x0c164f53u, 0x334a5203u, 0x7bc4cddcu}}, // COMBATPDW
    {0x61012683u, {0u, 0u, 0u, 0xeac8c270u, 0u}},                           // GUSENBERG
    {0xbfefff6du, {0xa73d4664u, 0x9d2fbf29u, 0x0c164f53u, 0xb1214f9bu, 0x7bc4cddcu}}, // ASSAULTRIFLE
    {0x83bf0278u, {0x837445aau, 0xa0d89c42u, 0x0c164f53u, 0x91109691u, 0x7bc4cddcu}}, // CARBINERIFLE
    {0xc0a3098du, {0xa73d4664u, 0xa0d89c42u, 0x0c164f53u, 0x7c8bd10eu, 0x7bc4cddcu}}, // SPECIALCARBINE
    {0xaf113f99u, {0x837445aau, 0xaa2c45b4u, 0u, 0x8ec1c979u, 0x7bc4cddcu}}, // ADVANCEDRIFLE
    {0x7f229f94u, {0x837445aau, 0xaa2c45b4u, 0x0c164f53u, 0xb3688b0fu, 0x7bc4cddcu}}, // BULLPUPRIFLE
    {0x624fe830u, {0u, 0u, 0u, 0x59ff9bf8u, 0u}},                           // COMPACTRIFLE
    {0x05fc3c11u, {0xa73d4664u, 0xd2443ddcu, 0u, 0u, 0u}},                   // SNIPERRIFLE
    {0x0c472fe2u, {0u, 0xd2443ddcu, 0u, 0u, 0u}},                           // HEAVYSNIPER
    {0xc734385au, {0x837445aau, 0xa0d89c42u, 0x0c164f53u, 0xccfd2ac5u, 0x7bc4cddcu}}, // MARKSMANRIFLE
    {0x1d073a89u, {0xe608b35eu, 0u, 0u, 0u, 0x7bc4cddcu}},                   // PUMPSHOTGUN
    {0xe284c527u, {0u, 0u, 0u, 0x86bd7f72u, 0x7bc4cddcu}},                   // ASSAULTSHOTGUN
    {0x9d61e50fu, {0x837445aau, 0u, 0u, 0u, 0x7bc4cddcu}},                   // BULLPUPSHOTGUN
    {0x3aabbbaau, {0u, 0u, 0x0c164f53u, 0x971cf6fdu, 0x7bc4cddcu}},          // HEAVYSHOTGUN
    {0x7fd62962u, {0u, 0xa0d89c42u, 0x0c164f53u, 0xd6c59cd6u, 0u}},          // COMBATMG
    {0x9d07f764u, {0u, 0x3c00afedu, 0u, 0x82158b47u, 0u}},                   // MG
};

// Per-slot staged Off/On, committed to the equipped weapon by Apply Attachments. Runtime-only (the
// equipped weapon + its components are themselves not persisted), so no profile field.
static uint32_t g_wattach_on[GTAV_WATTACH_SLOT_COUNT];

static const struct WAttachRow* wattach_row_for(uint32_t weapon) {
  for (uint32_t i = 0; i < sizeof(kWAttach) / sizeof(kWAttach[0]); ++i) {
    if (kWAttach[i].weapon == weapon) return &kWAttach[i];
  }
  return nullptr;
}

// Worker-side staging cycler for one slot: Left/Right flip Off<->On; the value is committed to the
// weapon by Apply Attachments (the give/remove natives are game-thread only). Quiet on Left/Right
// (a staging cycler), so it is NOT in action_applies_on_change.
static uint32_t cycle_weapon_attachment(uint32_t action, int slot, uint32_t param) {
  stage_step(&g_wattach_on[slot], 2u, param);
  return ok(action, g_wattach_on[slot] ? "On" : "Off");
}

// Read the player's currently-equipped weapon hash (out-param idiom, same as cycle_weapon_tint).
// Returns 0 if none/unavailable.
static uint32_t wattach_current_weapon(int* out_ped) {
  if (!g_n.get_current_ped_weapon || !g_n.player_ped_id) return 0u;
  int ped = native_player_ped_id();
  if (out_ped) *out_ped = ped;
  if (ped <= 0) return 0u;
  uint32_t weapon = 0u;
  int has = invoke_return<int>(g_n.get_current_ped_weapon, ped, &weapon, 1);
  return (has && weapon) ? weapon : 0u;
}

// Game-thread: commit the staged Off/On set to the equipped weapon (give the On slots, remove the
// Off ones). Runs from run_gated_action -- the component natives walk the ped weapon manager.
static uint32_t apply_weapon_attachments(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_APPLY_ATTACHMENTS;
  if (!g_n.give_weapon_component_to_ped || !g_n.remove_weapon_component_from_ped ||
      !g_n.get_current_ped_weapon || !g_n.player_ped_id) {
    return unavailable(action, "missing attachment natives");
  }
  int ped = 0;
  const uint32_t weapon = wattach_current_weapon(&ped);
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  if (!weapon) return failed(action, "no weapon equipped");
  const struct WAttachRow* row = wattach_row_for(weapon);
  if (!row) return unavailable(action, "no attachments for this weapon");
  int on = 0;
  for (int s = 0; s < GTAV_WATTACH_SLOT_COUNT; ++s) {
    const uint32_t comp = row->comp[s];
    if (!comp) continue;  // this weapon does not accept this slot
    if (g_wattach_on[s]) {
      invoke_void(g_n.give_weapon_component_to_ped, ped, weapon, comp);
      ++on;
    } else {
      invoke_void(g_n.remove_weapon_component_from_ped, ped, weapon, comp);
    }
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "attachments applied (%d on)", on);
  return ok(action, msg);
}

// Game-thread: strip every supported component from the equipped weapon and clear the staged set.
static uint32_t remove_weapon_attachments(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS;
  if (!g_n.remove_weapon_component_from_ped || !g_n.get_current_ped_weapon || !g_n.player_ped_id) {
    return unavailable(action, "missing attachment natives");
  }
  int ped = 0;
  const uint32_t weapon = wattach_current_weapon(&ped);
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  if (!weapon) return failed(action, "no weapon equipped");
  const struct WAttachRow* row = wattach_row_for(weapon);
  if (!row) return unavailable(action, "no attachments for this weapon");
  for (int s = 0; s < GTAV_WATTACH_SLOT_COUNT; ++s) {
    if (row->comp[s]) invoke_void(g_n.remove_weapon_component_from_ped, ped, weapon, row->comp[s]);
    g_wattach_on[s] = 0u;
  }
  return ok(action, "attachments removed");
}
