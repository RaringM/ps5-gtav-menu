// Fly modes.
//
// Source fragment of features.cpp, textually included AFTER noclip.inc inside its anonymous
// namespace -- reuses noclip's libm-free trig (noclip_sinf/cosf), control reads (noclip_axis/
// noclip_ctrl), gameplay-camera rotation (noclip_cam_rot), pitch deadband, the GTAV_NOCLIP_*
// control ids/consts, and the Vec3 / g_n / invoke helpers. Not a standalone TU.
//
// Velocity-driven flight, DISTINCT from noclip's freeze + teleport-step: gravity is killed and the
// entity's velocity is driven each frame from the camera look vector + throttle, so momentum and
// collision still apply (you fly but still bump walls). Player mode flies the ped on foot; Vehicle
// mode flies whatever the player is driving (any car/bike becomes a flying car). The per-frame
// driver runs on the GAME thread from gtav_features_game_thread_tick() -- the velocity/gravity/
// camera natives fault on the scePad worker thread, exactly like noclip. The mode enum/labels and
// the worker-side cyclers live OUTSIDE the frame-hook gate (so gtav_features_value_label + the
// cyclers compile in a no-hook build); only the per-frame driver is gated.

// Shared state -- read by the worker-side cyclers, gtav_features_value_label(), AND the game-thread
// driver, so it lives OUTSIDE the frame-hook gate. The mode globals are declared in features.cpp
// (g_fly_mode / g_vehicle_fly_mode), volatile because the worker writes and the game thread reads.
enum { GTAV_FLY_OFF = 0, GTAV_FLY_ON = 1, GTAV_FLY_FAST = 2 };

static const struct FlyModeRow {
  char label[8];  // shown in the cycler "< ... >" and as the action toast
} kFlyModes[] = {{"Off"}, {"On"}, {"Fast"}};

// True only when the per-frame driver can actually run (the frame hook is live). Mirrors
// autopilot_hook_ready(); always 0 in a build without the frame hook so the cyclers refuse.
static int fly_hook_ready(void) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  return gtav_frame_hook_is_active() ? 1 : 0;
#else
  return 0;
#endif
}

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Per-frame fly speeds (metres/sec at full input). Starting values to tune in-game, like the
// noclip / autopilot constants.
static const float GTAV_FLY_PLAYER_MPS = 16.0f;
static const float GTAV_FLY_VEHICLE_MPS = 45.0f;
static const float GTAV_FLY_FAST_MULT = 3.0f;

static uint32_t g_fly_last_frame = 0;
static int g_fly_entity = 0;  // entity we last killed gravity on, restored on stop/switch

static void fly_release(int entity) {
  if (entity && g_n.set_entity_has_gravity) invoke_void(g_n.set_entity_has_gravity, entity, 1);
}

// Camera-relative forward + right unit vectors (yaw 0 = +Y, GTA heading convention), reusing
// noclip's libm-free trig + pitch deadband. Out: fwd[3], right[3] (right has no Z component).
static void fly_basis(float* fwd, float* right) {
  float yaw = 0.0f;
  float pitch = 0.0f;
  if (g_n.get_gameplay_cam_rot) {
    const Vec3 rot = noclip_cam_rot();
    yaw = rot.z * GTAV_NOCLIP_DEG2RAD;
    pitch = noclip_pitch_with_deadband(rot.x) * GTAV_NOCLIP_DEG2RAD;
  }
  const float cp = noclip_cosf(pitch);
  fwd[0] = -noclip_sinf(yaw) * cp;
  fwd[1] = noclip_cosf(yaw) * cp;
  fwd[2] = noclip_sinf(pitch);
  right[0] = noclip_cosf(yaw);
  right[1] = noclip_sinf(yaw);
  right[2] = 0.0f;
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick(). Flies the
// ped (on foot) or the current vehicle by setting its velocity along the camera basis each frame;
// gravity is killed so it floats when idle.
static void fly_game_thread_tick(void) {
  const int pmode = g_fly_mode;
  const int vmode = g_vehicle_fly_mode;
  if (pmode == GTAV_FLY_OFF && vmode == GTAV_FLY_OFF) {
    if (g_fly_entity) {
      fly_release(g_fly_entity);
      g_fly_entity = 0;
    }
    return;
  }
  if (!g_n.set_entity_velocity || !g_n.player_ped_id) return;

  // Collapse the many hook fires within one rendered frame to a single velocity step.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_fly_last_frame) return;
    g_fly_last_frame = frame;
  }

  const int ped = native_player_ped_id();
  if (!ped) return;
  int veh = 0;
  if (g_n.is_ped_in_any_vehicle && g_n.get_vehicle_ped_is_in &&
      invoke_return<int>(g_n.is_ped_in_any_vehicle, ped, 0)) {
    veh = invoke_return<int>(g_n.get_vehicle_ped_is_in, ped, 0);
  }

  // Pick the active entity + speed: in a vehicle -> vehicle-fly drives the car; on foot ->
  // player-fly drives the ped. The two never overlap (the player is one or the other).
  int entity = 0;
  float speed = 0.0f;
  if (veh && vmode != GTAV_FLY_OFF) {
    entity = veh;
    speed = GTAV_FLY_VEHICLE_MPS;
    if (vmode == GTAV_FLY_FAST) speed *= GTAV_FLY_FAST_MULT;
  } else if (!veh && pmode != GTAV_FLY_OFF) {
    entity = ped;
    speed = GTAV_FLY_PLAYER_MPS;
    if (pmode == GTAV_FLY_FAST) speed *= GTAV_FLY_FAST_MULT;
  } else {
    // Active mode doesn't match the current on-foot/in-vehicle state -> release + idle.
    if (g_fly_entity) {
      fly_release(g_fly_entity);
      g_fly_entity = 0;
    }
    return;
  }

  // Kill gravity on (re)acquire / entity change; restore the previous one. Re-assert every frame so
  // a settle or ped<->vehicle transition can't drop the entity.
  if (g_fly_entity && g_fly_entity != entity) fly_release(g_fly_entity);
  if (g_n.set_entity_has_gravity) invoke_void(g_n.set_entity_has_gravity, entity, 0);
  g_fly_entity = entity;

  // Read camera-relative throttle. The horizontal stick drives flight whether or not the menu is
  // open (the menu navigates with the D-pad + face buttons only, never the movement stick), so you
  // can fly while browsing; hold nothing to hover in place. Vertical (up/down) rides R1/L1
  // (ASCEND/DESCEND) in BOTH states -- unlike Jump=Cross (the menu SELECT, hidden from the game when
  // open) -- so full 6-DOF flight works menu-open; Jump/Duck stay as an extra menu-closed binding.
  // The consumed controls are always suppressed so the ped/vehicle doesn't fight the velocity.
  float fwd_in = -noclip_axis(GTAV_NOCLIP_INPUT_MOVE_UD);
  float side_in = noclip_axis(GTAV_NOCLIP_INPUT_MOVE_LR);
  float up_in = (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_ASCEND) - noclip_ctrl(GTAV_NOCLIP_INPUT_DESCEND));
  if (fwd_in == 0.0f) {
    fwd_in =
        (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_UP) - noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_DOWN));
  }
  if (side_in == 0.0f) {
    side_in = (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_RIGHT) -
                      noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_LEFT));
  }
  if (!g_menu_open) {
    up_in += (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_JUMP) - noclip_ctrl(GTAV_NOCLIP_INPUT_DUCK));
  }
  if (up_in > 1.0f) up_in = 1.0f;
  if (up_in < -1.0f) up_in = -1.0f;
  if (g_n.disable_control_action) {
    static const int kSuppress[] = {GTAV_NOCLIP_INPUT_MOVE_LR,    GTAV_NOCLIP_INPUT_MOVE_UD,
                                    GTAV_NOCLIP_INPUT_MOVE_UP,     GTAV_NOCLIP_INPUT_MOVE_DOWN,
                                    GTAV_NOCLIP_INPUT_MOVE_LEFT,   GTAV_NOCLIP_INPUT_MOVE_RIGHT,
                                    GTAV_NOCLIP_INPUT_JUMP,        GTAV_NOCLIP_INPUT_DUCK,
                                    GTAV_NOCLIP_INPUT_ASCEND,      GTAV_NOCLIP_INPUT_DESCEND};
    for (unsigned i = 0; i < sizeof(kSuppress) / sizeof(kSuppress[0]); ++i) {
      invoke_void(g_n.disable_control_action, 0, kSuppress[i], 1);
    }
  }

  float fwd[3];
  float right[3];
  fly_basis(fwd, right);

  // Idle -> zero velocity (gravity-off keeps it floating). Otherwise drive velocity straight along
  // the camera basis; normalise the input so diagonals aren't faster.
  float vx = 0.0f;
  float vy = 0.0f;
  float vz = 0.0f;
  const float len2 = fwd_in * fwd_in + side_in * side_in + up_in * up_in;
  if (len2 > 0.0001f) {
    const float inv = (len2 > 1.0f) ? 1.0f / sqrtf(len2) : 1.0f;
    fwd_in *= inv;
    side_in *= inv;
    up_in *= inv;
    vx = (fwd[0] * fwd_in + right[0] * side_in) * speed;
    vy = (fwd[1] * fwd_in + right[1] * side_in) * speed;
    vz = (fwd[2] * fwd_in + up_in) * speed;
  }
  invoke_void(g_n.set_entity_velocity, entity, vx, vy, vz);
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Worker-side cyclers, pick then apply (mode_pick_step): Left/Right stage a mode, Cross makes it the
// live value the game-thread driver reads. A non-Off mode is refused until the frame hook is live
// (the velocity/camera natives run on that tick), mirroring toggle_noclip() /
// cycle_autopilot_mode().
static ModePick g_fly_pick;
static ModePick g_vehicle_fly_pick;
static uint32_t cycle_fly_mode(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_FLY_MODE;
  const uint32_t count = (uint32_t)(sizeof(kFlyModes) / sizeof(kFlyModes[0]));
  if (mode_pick_step(&g_fly_pick, &g_fly_mode, count, param))
    return ok(action, kFlyModes[g_fly_pick.pick % count].label);
  const uint32_t mode = (uint32_t)g_fly_mode % count;
  if (mode != (uint32_t)GTAV_FLY_OFF && !fly_hook_ready()) {
    return unavailable(action, "needs main-thread hook");
  }
  return ok(action, kFlyModes[mode].label);
}

static uint32_t cycle_vehicle_fly(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_FLY;
  const uint32_t count = (uint32_t)(sizeof(kFlyModes) / sizeof(kFlyModes[0]));
  if (mode_pick_step(&g_vehicle_fly_pick, &g_vehicle_fly_mode, count, param))
    return ok(action, kFlyModes[g_vehicle_fly_pick.pick % count].label);
  const uint32_t mode = (uint32_t)g_vehicle_fly_mode % count;
  if (mode != (uint32_t)GTAV_FLY_OFF && !fly_hook_ready()) {
    return unavailable(action, "needs main-thread hook");
  }
  return ok(action, kFlyModes[mode].label);
}
