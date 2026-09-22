// Saved vehicle (garage): capture the current vehicle's build and respawn it later, built.
//
// Source fragment of features.cpp, included AFTER lsc.inc (so the GTAV_VEH_MOD_* slot range and
// apply_spawn_vehicle_defaults are in scope) and AFTER spawn_skin.inc's spawn job is declared.
//
// Save is worker-direct (model-hash + per-slot GET_VEHICLE_MOD are plain getters). Respawn is
// game-thread gated: SPAWN_SAVED_VEHICLE routes through the existing streaming spawn job, and the
// saved mod loadout is re-applied in the post-create hook (apply_spawn_vehicle_defaults ->
// apply_saved_vehicle_build) using the game-thread invoke_*_ctx variants.

static_assert(GTAV_VEH_MOD_LAST_INDEXED + 1 == (int)GTAV_SAVED_VEHICLE_MOD_SLOTS,
              "GTAV_SAVED_VEHICLE_MOD_SLOTS must cover indexed mod slots 0..GTAV_VEH_MOD_LAST_INDEXED");

// Persisted builds (profile v16): GTAV_SAVED_VEHICLE_SLOTS garage slots. model == 0 means an empty
// slot; each mod slot stores the installed index as an int (GET_VEHICLE_MOD), with -1 == stock.
// g_saved_vehicle_slot selects which slot Save/Spawn act on (worker-side, not persisted).
static uint32_t g_saved_vehicle_model[GTAV_SAVED_VEHICLE_SLOTS];
static uint32_t g_saved_vehicle_mods[GTAV_SAVED_VEHICLE_SLOTS][GTAV_SAVED_VEHICLE_MOD_SLOTS];
static uint32_t g_saved_vehicle_slot = 0;
// Set when a SPAWN_SAVED_VEHICLE job is in flight; consumed by the post-create hook below. Latches
// which slot's mods to apply (g_saved_vehicle_slot can change before the game-thread hook drains).
static int g_pending_saved_build = 0;
static uint32_t g_pending_saved_slot = 0;

// True when the currently-selected garage slot holds a saved vehicle (drives the overwrite confirm).
static int saved_vehicle_slot_occupied(void) {
  return g_saved_vehicle_model[g_saved_vehicle_slot % GTAV_SAVED_VEHICLE_SLOTS] != 0u;
}

// Re-apply the saved mod loadout to a freshly created vehicle, on the game-thread spawn-job
// context. Returns 1 if a pending saved build was consumed (skips the maxed/invincible defaults).
static int apply_saved_vehicle_build(int worker, int vehicle) {
  if (!g_pending_saved_build) return 0;
  g_pending_saved_build = 0;
  if (!vehicle || !g_n.set_vehicle_mod || !g_n.set_vehicle_mod_kit) return 1;
  const uint32_t* mods = g_saved_vehicle_mods[g_pending_saved_slot % GTAV_SAVED_VEHICLE_SLOTS];
  invoke_void_ctx(worker, g_n.set_vehicle_mod_kit, vehicle, 0);
  for (int t = 0; t <= GTAV_VEH_MOD_LAST_INDEXED; ++t) {
    invoke_void_ctx(worker, g_n.set_vehicle_mod, vehicle, t, (int)mods[t], 0);
  }
  return 1;
}

// Capture the current vehicle's model + indexed mods into the selected slot, then persist the
// profile. Worker-direct.
static uint32_t save_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SAVE_VEHICLE;
  if (!g_n.get_entity_model || !g_n.get_vehicle_mod || !g_n.set_vehicle_mod_kit) {
    return unavailable(action, "missing vehicle save natives");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const uint32_t slot = g_saved_vehicle_slot % GTAV_SAVED_VEHICLE_SLOTS;
  invoke_void(g_n.set_vehicle_mod_kit, v, 0);  // ensure a mod kit so GET_VEHICLE_MOD reads true values
  g_saved_vehicle_model[slot] = invoke_return<uint32_t>(g_n.get_entity_model, v);
  for (int t = 0; t <= GTAV_VEH_MOD_LAST_INDEXED; ++t) {
    g_saved_vehicle_mods[slot][t] = (uint32_t)invoke_return<int>(g_n.get_vehicle_mod, v, t);
  }
  if (gtav_features_profile_save_default() != 0) return failed(action, "saved but not persisted");
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "saved to garage slot %u", (unsigned)(slot + 1u));
  return ok(action, msg);
}

// Game-thread entry (run from run_gated_action): flag the pending build for the selected slot and
// hand off to the streaming spawn job with that slot's model hash. The post-create hook applies the
// saved mods.
static uint32_t spawn_saved_vehicle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_SAVED_VEHICLE;
  const uint32_t slot = g_saved_vehicle_slot % GTAV_SAVED_VEHICLE_SLOTS;
  if (!g_saved_vehicle_model[slot]) return unavailable(action, "slot is empty");
  g_pending_saved_build = 1;
  g_pending_saved_slot = slot;
  gtav_features_run_spawn_hash_job(GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE,
                                   g_saved_vehicle_model[slot], nullptr);
  return g_state.last_result;
}

// Garage-slot selector: pure worker-side selection cycler (Save/Spawn read g_saved_vehicle_slot).
static uint32_t cycle_saved_vehicle_slot(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_SAVED_VEHICLE_SLOT;
  stage_step(&g_saved_vehicle_slot, GTAV_SAVED_VEHICLE_SLOTS, param);
  const uint32_t slot = g_saved_vehicle_slot % GTAV_SAVED_VEHICLE_SLOTS;
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "garage slot %u%s", (unsigned)(slot + 1u),
           g_saved_vehicle_model[slot] ? " [set]" : " [empty]");
  return ok(action, msg);
}

// Persistent "< value >" render for the Garage Slot row (copied immediately by push_list_values).
static char g_saved_vehicle_slot_label[24];
static const char* saved_vehicle_slot_value_label(void) {
  const uint32_t slot = g_saved_vehicle_slot % GTAV_SAVED_VEHICLE_SLOTS;
  snprintf(g_saved_vehicle_slot_label, sizeof(g_saved_vehicle_slot_label), "Slot %u%s",
           (unsigned)(slot + 1u), g_saved_vehicle_model[slot] ? " [set]" : " [empty]");
  return g_saved_vehicle_slot_label;
}
