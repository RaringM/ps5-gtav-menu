// World spectacle (Wind / Camera Shake / Fireworks).
//
// Source fragment of features.cpp, textually included inside its anonymous namespace (shares g_n /
// the invoke helpers / Vec3 / list_step / ok/unavailable/failed). Not a standalone TU.
//
// Three world-effect toys under World > Spectacle:
//   - Wind        : worker-safe global wind-speed setter (same class as Weather/Time).
//   - Camera Shake: a game-thread re-asserted SHAKE_GAMEPLAY_CAM (volatile mode state), run from
//                   spectacle_game_thread_tick() so the shake is sustained ("earthquake").
//   - Fireworks   : a game-thread PTFX streaming job (request the named asset, then start the
//                   non-looped fx at the player) -- the request/start are streaming + fx-spawn
//                   natives (allocation class, like REQUEST_MODEL), so it runs game-thread via the
//                   frame-hook drain with non-blocking self-requeue while the asset streams in.

// Wind speed presets (m/s). Calm = stop the wind. Worker-safe; the index is runtime-only (not
// persisted -- wind is a transient world effect, reset to Calm on load by disable_all).
static const struct WindRow {
  char label[8];
  float speed;
} kWindLevels[] = {{"Calm", 0.0f}, {"Breezy", 4.0f}, {"Windy", 8.0f}, {"Gale", 12.0f}};
// NOTE: SET_WIND_SPEED is documented to clamp to 0.0-12.0, so Gale tops out at 12 (the old 15.0 was
// silently clamped). Wind in GTA V is intrinsically subtle (it sways foliage/cloth + drifts smoke,
// but never pushes the player/vehicles), so with nothing wind-reactive in view it can be hard to
// notice -- this is the engine's behaviour, not a bug.

// Camera-shake presets. Off = no shake; the rest re-assert SHAKE_GAMEPLAY_CAM each frame at the
// given intensity. g_cam_shake_index is declared in features.cpp (volatile -- the cycler runs on
// the worker, the driver on the game thread).
static const struct CamShakeRow {
  char label[8];
  float intensity;
} kCamShakeLevels[] = {{"Off", 0.0f}, {"Small", 0.4f}, {"Medium", 0.9f}, {"Large", 1.8f}};

// Wind: worker-safe. Step the index (Left/Right) then apply the wind speed + a fixed direction
// immediately (a global field write, same class as cycle_weather). Cross (param 0) re-applies the
// current level.
static uint32_t cycle_wind(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_WIND;
  const uint32_t count = (uint32_t)(sizeof(kWindLevels) / sizeof(kWindLevels[0]));
  if (!g_n.set_wind_speed) return unavailable(action, "missing wind native");
  if (param == 1u || param == 2u) {
    g_wind_index = list_step(g_wind_index, count, param == 2u ? -1 : 1);
  }
  const uint32_t i = g_wind_index % count;
  invoke_void(g_n.set_wind_speed, kWindLevels[i].speed);
  if (g_n.set_wind_direction) invoke_void(g_n.set_wind_direction, 0.0f);
  return ok(action, kWindLevels[i].label);
}

// Camera shake: stage the mode; the game-thread tick applies it. A non-Off level is refused until
// the frame hook is live (SHAKE_GAMEPLAY_CAM is a render-script effect), mirroring cycle_fly_mode().
static uint32_t cycle_cam_shake(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_CAM_SHAKE;
  const uint32_t count = (uint32_t)(sizeof(kCamShakeLevels) / sizeof(kCamShakeLevels[0]));
  if (param == 1u || param == 2u) {
    g_cam_shake_index = (int)list_step((uint32_t)g_cam_shake_index, count, param == 2u ? -1 : 1);
  }
  const uint32_t i = (uint32_t)g_cam_shake_index % count;
  if (i != 0u && !fly_hook_ready()) return unavailable(action, "needs main-thread hook");
  return ok(action, kCamShakeLevels[i].label);
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Fireworks PTFX asset + one of its burst effects. Tunable in-game (the asset/effect names and the
// launch height/scale are starting values).
static const char kFireworksAsset[] = "scr_indep_fireworks";
static const char kFireworksEffect[] = "scr_indep_firework_starburst";
static const float GTAV_FIREWORKS_HEIGHT = 35.0f;  // metres above the player (high enough to read)
static const float GTAV_FIREWORKS_SCALE = 3.0f;
// Cycle a few bright colours per launch so consecutive bursts vary (and so the burst is never the
// near-black default tint that made it invisible). RGB 0..1.
static const struct {
  float r, g, b;
} kFireworkColours[] = {{1.0f, 0.2f, 0.2f}, {0.2f, 0.5f, 1.0f}, {0.3f, 1.0f, 0.3f}, {1.0f, 0.85f, 0.2f}};
static uint32_t g_firework_colour_index = 0;

// Game-thread driver: re-trigger the selected camera shake periodically so it is sustained as a
// repeating rumble. Cheap when Off (one volatile read + early return).
//
// SHAKE_GAMEPLAY_CAM("SMALL_EXPLOSION_SHAKE", ...) starts a ONE-SHOT, time-enveloped shake that ramps
// up then decays over ~1 s. Re-invoking it every single frame restarted that envelope at t=0 every
// frame, pinning the camera at the near-zero start of the ramp -> no perceptible shake (the reported
// bug). Instead fire it once every GTAV_CAM_SHAKE_PERIOD frames, so each impulse plays its full
// envelope before the next re-trigger -- a steady rumble. (A constant-amplitude hold would need the
// unbound SET_GAMEPLAY_CAM_SHAKE_AMPLITUDE native; the frame-divider needs none.)
static const uint32_t GTAV_CAM_SHAKE_PERIOD = 36u;  // ~0.6 s at 60 fps between impulses
static void spectacle_game_thread_tick(void) {
  const uint32_t count = (uint32_t)(sizeof(kCamShakeLevels) / sizeof(kCamShakeLevels[0]));
  const uint32_t i = (uint32_t)g_cam_shake_index % count;
  if (i == 0u || !g_n.shake_gameplay_cam) return;
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if ((frame % GTAV_CAM_SHAKE_PERIOD) != 0u) return;  // let the prior impulse decay first
  }
  invoke_void(g_n.shake_gameplay_cam, "SMALL_EXPLOSION_SHAKE", kCamShakeLevels[i].intensity);
}

// Game-thread fireworks job: stream the PTFX asset, then start the non-looped burst above the
// player. Non-blocking -- while the asset loads it re-enqueues itself (deadline-bounded), the same
// pattern the spawn jobs use for model streaming.
static uint32_t spawn_fireworks() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS;
  static time_t deadline = 0;
  if (!g_n.request_named_ptfx_asset || !g_n.has_named_ptfx_asset_loaded ||
      !g_n.use_particle_fx_asset || !g_n.start_particle_fx_non_looped_at_coord ||
      !g_n.get_entity_coords || !g_n.player_ped_id) {
    deadline = 0;
    return unavailable(action, "missing fireworks natives");
  }
  const int ped = native_player_ped_id();
  if (!ped) {
    deadline = 0;
    return failed(action, "player ped unavailable");
  }
  invoke_void(g_n.request_named_ptfx_asset, kFireworksAsset);
  if (!invoke_return<int>(g_n.has_named_ptfx_asset_loaded, kFireworksAsset)) {
    if (deadline == 0) deadline = time(NULL) + GTAV_MODEL_STREAM_WAIT_SECONDS;
    if (time(NULL) < deadline && gtav_frame_hook_enqueue(action, 0u)) {
      return ok(action, "fireworks loading");
    }
    deadline = 0;
    return failed(action, "fireworks asset not loaded");
  }
  deadline = 0;
  const Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.use_particle_fx_asset, kFireworksAsset);
  // Tint the burst BEFORE starting it -- without this the sparks default to ~black and are
  // invisible against the sky (the reported "audible but invisible" firework). Cycle the colour so
  // repeated launches vary. SET_PARTICLE_FX_NON_LOOPED_COLOUR(r, g, b) in 0..1.
  if (g_n.set_particle_fx_non_looped_colour) {
    const uint32_t ci =
        g_firework_colour_index % (uint32_t)(sizeof(kFireworkColours) / sizeof(kFireworkColours[0]));
    invoke_void(g_n.set_particle_fx_non_looped_colour, kFireworkColours[ci].r, kFireworkColours[ci].g,
                kFireworkColours[ci].b);
    g_firework_colour_index++;
  }
  invoke_return<int>(g_n.start_particle_fx_non_looped_at_coord, kFireworksEffect, c.x, c.y,
                     c.z + GTAV_FIREWORKS_HEIGHT, 0.0f, 0.0f, 0.0f, GTAV_FIREWORKS_SCALE, 0, 0, 0);
  return ok(action, "fireworks");
}
#else
// spectacle_game_thread_tick is called only from the frame-hook-gated gtav_features_game_thread_tick
// (like noclip/fly/gun), so it has no no-hook definition. spawn_fireworks is reached from the
// always-compiled run_gated_action, so it needs a no-hook stub.
static uint32_t spawn_fireworks() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_SPAWN_FIREWORKS, "needs main-thread hook");
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK
