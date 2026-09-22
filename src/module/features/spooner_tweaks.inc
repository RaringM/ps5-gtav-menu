// Spooner entity tweaks (World > Spawned Entities): act on the LAST spawned object.
//
// Source fragment of features.cpp, textually included AFTER spawned_entities.inc (the
// g_spawned_object_* roster + roster_handle_alive) and spawn_entity.inc (run_entity_spawn_job)
// inside its anonymous namespace -- shares g_n / g_state / the invoke helpers. Not a standalone TU.
//
// The spooner can already spawn + interactively move/save/load props; this adds the Menyoo-style
// entity edits: Duplicate (re-spawn the same model), Attach to the player (held-prop), Detach, and a
// transparency cycler. Duplicate/Attach/Detach touch entity managers -> game-thread (run_gated_action);
// the alpha cycler is a modify-existing field store (worker-safe, like SET_ENTITY_VISIBLE).

// Transparency presets for CYCLE_ENTITY_ALPHA (255 = solid; lower = more see-through).
static const struct EntityAlphaRow {
  char label[8];
  int alpha;
} kEntityAlphaLevels[] = {{"Solid", 255}, {"75%", 200}, {"50%", 128}, {"25%", 50}, {"Ghost", 20}};
static uint32_t g_entity_alpha_index = 0;

// The most-recently spawned, still-alive object handle (and its model hash via out_model). Walks the
// object roster newest-first; roster_handle_alive drops stale handles. 0 = nothing spawned.
static int last_tweak_object(int* out_model) {
  for (int i = g_spawned_object_count - 1; i >= 0; --i) {
    if (roster_handle_alive(g_spawned_object_roster[i])) {
      if (out_model) *out_model = g_spawned_object_model[i];
      return g_spawned_object_roster[i];
    }
  }
  if (out_model) *out_model = 0;
  return 0;
}

// Duplicate: spawn another copy of the last object's model at the configured placement (front of the
// player/camera), reusing the object spawn job. Game-thread (run_entity_spawn_job CREATE_OBJECTs +
// tracks the copy into the same roster).
static uint32_t duplicate_last_entity() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_DUPLICATE_LAST_ENTITY;
  int model = 0;
  if (!last_tweak_object(&model) || !model) return unavailable(action, "no spawned object");
  run_entity_spawn_job(GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT, (uint32_t)model, /*is_ped=*/0);
  return g_state.last_result;
}

// Attach the last object to the player ped so it floats in front like a held prop. Game-thread
// (ATTACH_ENTITY_TO_ENTITY walks the attachment manager).
static uint32_t attach_last_entity() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_ATTACH_LAST_ENTITY;
  if (!g_n.attach_entity_to_entity || !g_n.player_ped_id) {
    return unavailable(action, "missing attach native");
  }
  int ent = last_tweak_object(nullptr);
  if (!ent) return unavailable(action, "no spawned object");
  int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  // ATTACH_ENTITY_TO_ENTITY(e1, e2, bone, x,y,z, rx,ry,rz, p9, softPin, collision, isPed, vtx,
  // fixedRot). Offset 0.6 m forward + 0.3 m up; collision off so it does not shove the player.
  invoke_void(g_n.attach_entity_to_entity, ent, ped, 0, 0.0f, 0.6f, 0.3f, 0.0f, 0.0f, 0.0f, 0, 0, 0,
              0, 2, 1);
  return ok(action, "attached to player");
}

static uint32_t detach_last_entity() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_DETACH_LAST_ENTITY;
  if (!g_n.detach_entity) return unavailable(action, "missing detach native");
  int ent = last_tweak_object(nullptr);
  if (!ent) return unavailable(action, "no spawned object");
  // DETACH_ENTITY(entity, dynamic, collision): re-enable physics + collision on release.
  invoke_void(g_n.detach_entity, ent, 1, 1);
  return ok(action, "detached");
}

// Transparency cycler. SET_ENTITY_ALPHA / RESET_ENTITY_ALPHA are modify-existing field stores
// (worker-safe), so this applies immediately on each Left/Right step (like cycle_wind), no apply
// gate. RESET at Solid clears the override (SET_ENTITY_ALPHA can't restore full opacity cleanly).
static uint32_t cycle_entity_alpha(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_ENTITY_ALPHA;
  const uint32_t count = (uint32_t)(sizeof(kEntityAlphaLevels) / sizeof(kEntityAlphaLevels[0]));
  if (!g_n.set_entity_alpha) return unavailable(action, "missing alpha native");
  if (param == 1u || param == 2u) {
    g_entity_alpha_index = list_step(g_entity_alpha_index, count, param == 2u ? -1 : 1);
  }
  int ent = last_tweak_object(nullptr);
  if (!ent) return unavailable(action, "no spawned object");
  const EntityAlphaRow& a = kEntityAlphaLevels[g_entity_alpha_index % count];
  if (a.alpha >= 255 && g_n.reset_entity_alpha) {
    invoke_void(g_n.reset_entity_alpha, ent);
  } else {
    invoke_void(g_n.set_entity_alpha, ent, a.alpha, 0);  // SET_ENTITY_ALPHA(entity, alpha, skin=0)
  }
  return ok(action, a.label);
}
