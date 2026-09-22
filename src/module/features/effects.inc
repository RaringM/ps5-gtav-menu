// Screen effects: night vision, thermal, timecycle colour grades, fullscreen animpostfx.
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.
//
// Night vision / thermal are toggles re-asserted every frame from the game-thread tick
// (effects.cpp owns no worker-direct render calls; cutscenes/loads can clear the flags, so
// re-asserting keeps them stable). The timecycle + animpostfx cyclers STAGE their index
// worker-side on Left/Right (so the menu value updates instantly) and APPLY on the game
// thread on Cross, because those natives touch render-script state.

// Curated timecycle modifiers (index 0 = Off -> CLEAR_TIMECYCLE_MODIFIER). name is the GTA
// modifier id passed to the native; label is the menu text. Names are build-version-sensitive
// -- a bad one is a silent no-op, not a crash -- so this stays a short, curated set.
struct EffectEntry {
  char label[14];  // inline (no relocation; see feature_catalog.h)
  char name[28];   // GTA effect id passed to the native (longest animpostfx ~25)
};
static const EffectEntry kTimecycleModifiers[] = {
    {"Off", ""},
    {"Drugs", "drug_drive_blend01"},
    {"Trippy", "spectator5"},
    {"Red Mist", "REDMIST"},
    {"Cold", "hud_def_desat_cold"},
    {"Noir", "NG_filmnoir_BW01"},
    {"Stoned", "stoned"},
    {"Alien Trip", "stoned_aliens"},
    {"Drug Fly", "drug_flying_01"},
    {"Wobbly", "drug_wobbly"},
    {"Deadman", "Drug_deadman"},
    {"Bloom", "Bloom"},
    {"Turbo", "RaceTurboFlash"},
    {"Camera BW", "CAMERA_BW"},
    {"Cinema", "cinema"},
    {"UFO", "ufo"},
    {"Scanline", "scanline_cam"},
    {"Night", "MP_deathfail_night"},
};
static const uint32_t kTimecycleCount =
    (uint32_t)(sizeof(kTimecycleModifiers) / sizeof(kTimecycleModifiers[0]));

// Curated animpostfx effects (index 0 = Off -> ANIMPOSTFX_STOP_ALL). Looped fullscreen FX.
static const EffectEntry kAnimpostfx[] = {
    {"Off", ""},
    {"Drunk", "Dont_tazeme_bro"},
    {"Trip", "DMT_flight"},
    {"Focus", "FocusIn"},
    {"Death", "DeathFailOut"},
    {"Drug Drive", "DrugsDrivingIn"},
    {"Peyote", "PeyoteIn"},
    {"Rampage", "Rampage"},
    {"Red Mist", "REDMIST"},
    {"Sniper", "SniperOverlay"},
    {"Chop", "ChopVision"},
    {"Bullet Time", "BulletTime"},
    {"Neon", "DeadlineNeon"},
    {"Turbo", "RaceTurbo"},
};
static const uint32_t kAnimpostfxCount = (uint32_t)(sizeof(kAnimpostfx) / sizeof(kAnimpostfx[0]));

static const char* timecycle_value_label() {
  return kTimecycleModifiers[g_timecycle_index % kTimecycleCount].label;
}
static const char* animpostfx_value_label() {
  return kAnimpostfx[g_animpostfx_index % kAnimpostfxCount].label;
}

// Re-assert night vision / thermal each frame on the game thread. Tracks the last applied
// state so an ON re-asserts every frame (cutscene-proof) while an OFF is issued once.
[[maybe_unused]] static void effects_game_thread_tick(void) {
  static int s_nv_applied = -1;
  static int s_st_applied = -1;
  if (g_n.set_nightvision) {
    if (g_night_vision_enabled) {
      invoke_void(g_n.set_nightvision, 1);
      s_nv_applied = 1;
    } else if (s_nv_applied != 0) {
      invoke_void(g_n.set_nightvision, 0);
      s_nv_applied = 0;
    }
  }
  if (g_n.set_seethrough) {
    if (g_seethrough_enabled) {
      invoke_void(g_n.set_seethrough, 1);
      s_st_applied = 1;
    } else if (s_st_applied != 0) {
      invoke_void(g_n.set_seethrough, 0);
      s_st_applied = 0;
    }
  }
}

static uint32_t toggle_night_vision() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NIGHT_VISION;
  if (!g_n.set_nightvision) return unavailable(action, "missing night vision native");
  g_night_vision_enabled = !g_night_vision_enabled;
  return ok(action, g_night_vision_enabled ? "night vision on" : "night vision off");
}

static uint32_t toggle_seethrough() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_SEETHROUGH;
  if (!g_n.set_seethrough) return unavailable(action, "missing thermal native");
  g_seethrough_enabled = !g_seethrough_enabled;
  return ok(action, g_seethrough_enabled ? "thermal vision on" : "thermal vision off");
}

// Apply the currently-staged timecycle / animpostfx. Game-thread only (run_gated_action).
static uint32_t apply_timecycle() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE;
  const uint32_t i = g_timecycle_index % kTimecycleCount;
  if (i == 0u) {
    if (g_n.clear_timecycle_modifier) invoke_void(g_n.clear_timecycle_modifier);
    return ok(action, "screen effect cleared");
  }
  if (g_n.set_timecycle_modifier) invoke_void(g_n.set_timecycle_modifier, kTimecycleModifiers[i].name);
  if (g_n.set_timecycle_modifier_strength) {
    invoke_void(g_n.set_timecycle_modifier_strength, 1.0f);
  }
  return ok(action, "screen effect applied");
}

static uint32_t apply_animpostfx() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX;
  const uint32_t i = g_animpostfx_index % kAnimpostfxCount;
  if (i == 0u) {
    if (g_n.animpostfx_stop_all) invoke_void(g_n.animpostfx_stop_all);
    return ok(action, "fx cleared");
  }
  if (g_n.animpostfx_stop_all) invoke_void(g_n.animpostfx_stop_all);
  if (g_n.animpostfx_play) invoke_void(g_n.animpostfx_play, kAnimpostfx[i].name, 0, 1);
  return ok(action, "fx applied");
}

// Cyclers: param 1/2 stage the index worker-side (instant menu update, no native); param 0
// (Cross) applies on the game thread via the frame-hook job queue.
static uint32_t cycle_effect(uint32_t action, uint32_t* index, uint32_t count, uint32_t param) {
  if (param == 1u) {
    *index = (*index + 1u) % count;
    return ok(action, "staged");
  }
  if (param == 2u) {
    *index = (*index + count - 1u) % count;
    return ok(action, "staged");
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (gtav_frame_hook_is_active()) {
    if (gtav_frame_hook_enqueue(action, 0u)) return ok(action, "queued for main thread");
    return failed(action, "main-thread job queue full");
  }
#endif
  return unavailable(action, "needs main-thread hook");
}

static uint32_t cycle_timecycle(uint32_t param) {
  if (!g_n.set_timecycle_modifier || !g_n.clear_timecycle_modifier) {
    return unavailable(GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE, "missing timecycle natives");
  }
  return cycle_effect(GTAV_NATIVE_SHELL_ACTION_CYCLE_TIMECYCLE, &g_timecycle_index, kTimecycleCount,
                      param);
}

static uint32_t cycle_animpostfx(uint32_t param) {
  if (!g_n.animpostfx_play || !g_n.animpostfx_stop_all) {
    return unavailable(GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX, "missing animpostfx natives");
  }
  return cycle_effect(GTAV_NATIVE_SHELL_ACTION_CYCLE_ANIMPOSTFX, &g_animpostfx_index,
                      kAnimpostfxCount, param);
}
