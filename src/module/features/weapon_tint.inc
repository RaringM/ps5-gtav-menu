// Weapons > Weapon Tint: recolour the player's equipped weapon.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace, AFTER
// world.inc (for stage_step). SET_PED_WEAPON_TINT_INDEX is a modify-existing weapon-field setter
// -- worker-safe class (like the LSC setters), NOT the give/remove weapon-manager lane that has to
// run on the game thread. Flagged for hardware verification like the rest of the wave.

// 8 standard weapon tints. MK2 weapons have their own (livery) palette and ignore out-of-range
// indices, so this stays at the base set.
static const char kWeaponTintNames[][12] = {
    "Normal", "Green", "Gold", "Pink", "Army", "LSPD", "Orange", "Platinum",
};
static uint32_t g_weapon_tint_index = 0;

// Stage/apply cycler: Left/Right pick a tint (no effect), Cross applies it to the equipped weapon.
static uint32_t cycle_weapon_tint(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WEAPON_TINT;
  const uint32_t count = (uint32_t)(sizeof(kWeaponTintNames) / sizeof(kWeaponTintNames[0]));
  if (stage_step(&g_weapon_tint_index, count, param)) {
    return ok(action, kWeaponTintNames[g_weapon_tint_index]);
  }
  if (!g_n.set_ped_weapon_tint_index || !g_n.get_current_ped_weapon || !g_n.player_ped_id) {
    return unavailable(action, "missing weapon tint natives");
  }
  int ped = native_player_ped_id();
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  uint32_t weapon = 0;
  // GET_CURRENT_PED_WEAPON(ped, Hash* out, doDeadCheck) -> BOOL; `weapon` is filled via the
  // out-param pointer (same idiom as GET_MAX_AMMO's int* out-param).
  int has = invoke_return<int>(g_n.get_current_ped_weapon, ped, &weapon, 1);
  if (!has || !weapon) return failed(action, "no weapon equipped");
  invoke_void(g_n.set_ped_weapon_tint_index, ped, weapon, (int)(g_weapon_tint_index % count));
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "weapon tint: %s", kWeaponTintNames[g_weapon_tint_index % count]);
  return ok(action, msg);
}
