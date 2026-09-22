// Noclip / free-move.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace
// (shares g_n / the invoke helpers / Vec3). Not a standalone TU.
//
// A per-frame GAME-THREAD effect: while enabled, the active player entity (the vehicle when
// driving, else the ped) is FROZEN so physics can't move it, and re-placed from a latched
// free-move position updated by camera-relative movement controls while the menu is closed.
// The driver runs from gtav_features_game_thread_tick() (the frame-hook
// lane), NOT the scePad worker thread: GET_GAMEPLAY_CAM_ROT and the entity move/velocity
// natives read engine state that is only valid in script context and FAULT when called off
// the game thread. (Earlier builds ran this on the worker and the console crashed as soon
// as movement actually fired.) FREEZE_ENTITY_POSITION + SET_ENTITY_COORDS_NO_OFFSET are
// modify-existing setters; the consumed controls are suppressed with DISABLE_CONTROL_ACTION
// so the ped/vehicle doesn't fight the teleport, and the held stick is read with
// IS_DISABLED_CONTROL_PRESSED (a control disabled this frame reports not-pressed via the
// plain IS_CONTROL_PRESSED).

#if GTAV_MENU_ENABLE_FRAME_HOOK
// GTA V control ids (PLAYER group 0). Movement prefers analog left-stick axes, with the
// digital direction reads retained as a keyboard / native-table fallback.
enum {
  GTAV_NOCLIP_INPUT_SPRINT = 21,
  GTAV_NOCLIP_INPUT_JUMP = 22,
  GTAV_NOCLIP_INPUT_DUCK = 36,
  GTAV_NOCLIP_INPUT_MOVE_LR = 30,     // analog left/right axis
  GTAV_NOCLIP_INPUT_MOVE_UD = 31,     // analog forward/back axis
  GTAV_NOCLIP_INPUT_MOVE_UP = 32,     // forward (push stick up)
  GTAV_NOCLIP_INPUT_MOVE_DOWN = 33,   // back
  GTAV_NOCLIP_INPUT_MOVE_LEFT = 34,   // strafe left
  GTAV_NOCLIP_INPUT_MOVE_RIGHT = 35,  // strafe right
  GTAV_NOCLIP_INPUT_ASCEND = 206,     // R1 -> climb. Works menu-open too (unlike Jump=Cross, which
  GTAV_NOCLIP_INPUT_DESCEND = 205,    // L1 -> descend. is the menu SELECT button / hidden from game)
};

static const float GTAV_NOCLIP_BASE_MPS = 18.0f;    // full-stick metres/sec
static const float GTAV_NOCLIP_SPRINT_MULT = 4.0f;  // hold Sprint to fly faster
static const float GTAV_NOCLIP_DEADZONE = 0.16f;    // left-stick drift guard
static const float GTAV_NOCLIP_PITCH_DEADBAND_DEG = 8.0f;
static const float GTAV_NOCLIP_DEG2RAD = 0.017453292519943295f;
static const float GTAV_NOCLIP_PI = 3.14159265358979323846f;

// The PLAYER_PED_ID frame hook fires once per script call to that native -- many times per
// rendered frame. We move one step per *frame*, so remember the last GET_FRAME_COUNT we
// stepped on and skip duplicate fires; without this the entity advances a step per fire and
// flies off uncontrollably.
static uint32_t g_noclip_last_frame = 0;
// GET_GAME_TIMER (ms) at the last stepped frame, so the per-frame step can be scaled by the
// real frame delta and fly speed stays consistent whether the game runs at 30 or 60 fps.
static uint32_t g_noclip_last_ms = 0;
// Authoritative free-move position. Do not repeatedly derive the next step from
// GET_ENTITY_COORDS: even while frozen, collision/physics can report a slightly settled Z
// after SET_ENTITY_COORDS_NO_OFFSET, and preserving that value every step becomes the slow
// downward drift the user sees. Sample engine coords only when acquiring/changing entity.
static Vec3 g_noclip_pos;
static int g_noclip_have_pos = 0;

// Minimal sine/cosine (no libm dependency -- the payload and host tests don't link -lm;
// only builtin-lowered sqrtf is safe to use). Bhaskara-style parabola + one refinement
// pass, accurate to ~0.001 -- ample for a movement-direction vector. Input in radians.
static float noclip_sinf(float x) {
  const float two_pi = 2.0f * GTAV_NOCLIP_PI;
  while (x > GTAV_NOCLIP_PI) x -= two_pi;
  while (x < -GTAV_NOCLIP_PI) x += two_pi;
  const float b = 4.0f / GTAV_NOCLIP_PI;
  const float c = -4.0f / (GTAV_NOCLIP_PI * GTAV_NOCLIP_PI);
  float y = b * x + c * x * (x < 0.0f ? -x : x);
  y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;  // refinement (P = 0.225)
  return y;
}
static float noclip_cosf(float x) { return noclip_sinf(x + 0.5f * GTAV_NOCLIP_PI); }

// Read a held movement control. The driver DISABLE_CONTROL_ACTIONs the movement controls
// each frame (so the ped/vehicle doesn't fight the teleport), and a control disabled this
// frame reports not-pressed via IS_CONTROL_PRESSED -- so read with IS_DISABLED_CONTROL_PRESSED
// (which reports the raw state regardless of the disable), falling back to the plain read
// only if the disabled variant is unavailable.
static int noclip_ctrl(int control) {
  const uint64_t fn =
      g_n.is_disabled_control_pressed ? g_n.is_disabled_control_pressed : g_n.is_control_pressed;
  if (!fn) return 0;
  return invoke_return<int>(fn, 0, control) ? 1 : 0;
}

static float noclip_absf(float x) { return x < 0.0f ? -x : x; }

static float noclip_apply_deadzone(float v) {
  const float a = noclip_absf(v);
  if (a <= GTAV_NOCLIP_DEADZONE) return 0.0f;
  float out = (a - GTAV_NOCLIP_DEADZONE) / (1.0f - GTAV_NOCLIP_DEADZONE);
  if (out > 1.0f) out = 1.0f;
  return v < 0.0f ? -out : out;
}

static float noclip_pitch_with_deadband(float pitch_deg) {
  const float a = noclip_absf(pitch_deg);
  if (a <= GTAV_NOCLIP_PITCH_DEADBAND_DEG) return 0.0f;
  return pitch_deg < 0.0f ? -(a - GTAV_NOCLIP_PITCH_DEADBAND_DEG)
                          : (a - GTAV_NOCLIP_PITCH_DEADBAND_DEG);
}

static float noclip_axis(int control) {
  const uint64_t fn =
      g_n.get_disabled_control_normal ? g_n.get_disabled_control_normal : g_n.get_control_normal;
  if (!fn) return 0.0f;
  return noclip_apply_deadzone(invoke_return<float>(fn, 0, control));
}

// Absolute gameplay-camera rotation (pitch=x, roll=y, yaw=z, degrees) via
// GET_GAMEPLAY_CAM_ROT(rotationOrder). Returned as a scrVector in returns[0..2], read the
// same way invoke_vector reads GET_ENTITY_COORDS.
static Vec3 noclip_cam_rot() {
  Vec3 zero = {0.0f, 0.0f, 0.0f};
  if (!g_n.get_gameplay_cam_rot) return zero;  // unresolved slot: never a null jump
  gtavmenu::NativeCallContext ctx;
  ctx.reset();
  ctx.push(2);  // rotationOrder 2 (ZXY) -- the Rockstar default
  gtavmenu::note_native_call(g_n.get_gameplay_cam_rot);  // crash-breadcrumb parity with the helpers
  ((void (*)(gtavmenu::NativeArg*))(uintptr_t)g_n.get_gameplay_cam_rot)(&ctx.native_arg);
  Vec3 v;
  v.x = *reinterpret_cast<const float*>(&ctx.returns[0]);
  v.y = *reinterpret_cast<const float*>(&ctx.returns[1]);
  v.z = *reinterpret_cast<const float*>(&ctx.returns[2]);
  return v;
}

// Release a frozen entity so physics resumes.
static void noclip_release(int entity) {
  if (!entity) return;
  if (g_n.set_entity_collision) invoke_void(g_n.set_entity_collision, entity, 1, 1);
  if (g_n.set_entity_has_gravity) invoke_void(g_n.set_entity_has_gravity, entity, 1);
  if (g_n.freeze_entity_position) invoke_void(g_n.freeze_entity_position, entity, 0);
  if (g_n.set_entity_velocity) invoke_void(g_n.set_entity_velocity, entity, 0.0f, 0.0f, 0.0f);
}

static void noclip_place(int entity) {
  const uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  if (!entity || !set_coords || !g_noclip_have_pos) return;
  if (g_n.set_entity_coords_no_offset) {
    invoke_void(set_coords, entity, g_noclip_pos.x, g_noclip_pos.y, g_noclip_pos.z, 0, 0, 1);
  } else {
    invoke_void(set_coords, entity, g_noclip_pos.x, g_noclip_pos.y, g_noclip_pos.z, 0, 0, 0, 0);
  }
  if (g_n.set_entity_velocity) invoke_void(g_n.set_entity_velocity, entity, 0.0f, 0.0f, 0.0f);
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick().
// Freezes the active entity and teleports it a small step along the camera look vector once
// per rendered frame.
static void noclip_game_thread_tick(void) {
  if (!g_noclip_enabled) {
    // Toggled off (or never on): release whatever we froze. Done here, on the game thread,
    // so the unfreeze lands in the same context as the freeze.
    if (g_noclip_entity) {
      noclip_release(g_noclip_entity);
      g_noclip_entity = 0;
    }
    g_noclip_last_ms = 0;  // drop the frame-delta baseline so a re-enable starts clean
    g_noclip_have_pos = 0;
    return;
  }
  if (!g_n.freeze_entity_position || !g_n.get_entity_coords) return;
  const uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  if (!set_coords) return;

  // Collapse the many hook fires within one rendered frame to a single step.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_noclip_last_frame) return;  // already stepped this frame; stay frozen
    g_noclip_last_frame = frame;
  }

  int entity = current_player_entity();
  if (!entity) return;
  // Re-freeze on entity change (player got in/out of a vehicle): release the old handle.
  if (g_noclip_entity && g_noclip_entity != entity) noclip_release(g_noclip_entity);
  if (g_noclip_entity != entity) {
    invoke_void(g_n.freeze_entity_position, entity, 1);
    if (g_n.set_entity_collision) invoke_void(g_n.set_entity_collision, entity, 0, 0);
    if (g_n.set_entity_has_gravity) invoke_void(g_n.set_entity_has_gravity, entity, 0);
    if (g_n.set_entity_velocity) invoke_void(g_n.set_entity_velocity, entity, 0.0f, 0.0f, 0.0f);
    g_noclip_pos = invoke_vector(g_n.get_entity_coords, entity, 1);
    g_noclip_have_pos = 1;
    g_noclip_entity = entity;
  }
  if (!g_noclip_have_pos) {
    g_noclip_pos = invoke_vector(g_n.get_entity_coords, entity, 1);
    g_noclip_have_pos = 1;
  }

  // Suppress the controls we consume so the ped/vehicle doesn't fight the teleport. Done whether or
  // not the menu is open: free-move is drivable with the menu up (the menu navigates with the D-pad
  // and face buttons only, never the movement stick), so the stick must keep steering the fly.
  if (g_n.disable_control_action) {
    static const int kSuppress[] = {GTAV_NOCLIP_INPUT_SPRINT,     GTAV_NOCLIP_INPUT_MOVE_LR,
                                    GTAV_NOCLIP_INPUT_MOVE_UD,    GTAV_NOCLIP_INPUT_MOVE_UP,
                                    GTAV_NOCLIP_INPUT_MOVE_DOWN,  GTAV_NOCLIP_INPUT_MOVE_LEFT,
                                    GTAV_NOCLIP_INPUT_MOVE_RIGHT, GTAV_NOCLIP_INPUT_JUMP,
                                    GTAV_NOCLIP_INPUT_DUCK,       GTAV_NOCLIP_INPUT_ASCEND,
                                    GTAV_NOCLIP_INPUT_DESCEND};
    for (unsigned i = 0; i < sizeof(kSuppress) / sizeof(kSuppress[0]); ++i) {
      invoke_void(g_n.disable_control_action, 0, kSuppress[i], 1);
    }
  }

  // Scale the step by the real frame delta (ms) so fly speed is frame-rate independent.
  // Sampled every stepped frame (we're past the frame-count gate) and clamped so a hitch
  // can't fling the entity across the map.
  float step_scale = 1.0f;
  if (g_n.get_game_timer) {
    const uint32_t now_ms = invoke_return<uint32_t>(g_n.get_game_timer);
    if (g_noclip_last_ms != 0u) {
      uint32_t dt = now_ms - g_noclip_last_ms;
      if (dt > 100u) dt = 100u;             // cap hitches at ~6 frames @60Hz
      step_scale = (float)dt / 16.667f;     // 1.0 at 60 fps, ~2.0 at 30 fps
    }
    g_noclip_last_ms = now_ms;
  }

  float fwd_in = -noclip_axis(GTAV_NOCLIP_INPUT_MOVE_UD);
  float side_in = noclip_axis(GTAV_NOCLIP_INPUT_MOVE_LR);
  if (fwd_in == 0.0f) {
    fwd_in =
        (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_UP) - noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_DOWN));
  }
  if (side_in == 0.0f) {
    side_in = (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_RIGHT) -
                      noclip_ctrl(GTAV_NOCLIP_INPUT_MOVE_LEFT));
  }
  // Vertical (up/down). R1/L1 (ASCEND/DESCEND) drive it in BOTH states so full 6-DOF fly works with
  // the menu open -- they are suppressed above and read via the disabled getter, and unlike Jump
  // (Cross) they aren't the menu SELECT button. Jump/Duck stay as an additional menu-CLOSED binding
  // (the established feel); Cross is hidden from the game while the menu is open, so it is skipped
  // then. Clamp the combined axis so pressing R1 + Jump together can't exceed full deflection.
  float up_in = (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_ASCEND) - noclip_ctrl(GTAV_NOCLIP_INPUT_DESCEND));
  if (!g_menu_open) {
    up_in += (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_JUMP) - noclip_ctrl(GTAV_NOCLIP_INPUT_DUCK));
  }
  if (up_in > 1.0f) up_in = 1.0f;
  if (up_in < -1.0f) up_in = -1.0f;

  float step = GTAV_NOCLIP_BASE_MPS * (step_scale * 0.016667f);
  if (noclip_ctrl(GTAV_NOCLIP_INPUT_SPRINT)) step *= GTAV_NOCLIP_SPRINT_MULT;

  // Camera-relative movement. A small pitch deadband preserves the "fly level while just
  // looking around" feel and prevents the old slow descent from the gameplay camera's usual
  // slight downward tilt, but deliberate look-up/look-down still drives vertical movement.
  float yaw = 0.0f;
  float pitch = 0.0f;
  if (g_n.get_gameplay_cam_rot) {
    const Vec3 rot = noclip_cam_rot();
    yaw = rot.z * GTAV_NOCLIP_DEG2RAD;
    pitch = noclip_pitch_with_deadband(rot.x) * GTAV_NOCLIP_DEG2RAD;
  } else if (g_n.get_gameplay_cam_relative_heading && g_n.get_entity_heading) {
    const float h = invoke_return<float>(g_n.get_entity_heading, entity) +
                    invoke_return<float>(g_n.get_gameplay_cam_relative_heading, 0);
    yaw = h * GTAV_NOCLIP_DEG2RAD;
  }

  // GTA heading convention: yaw 0 = +Y (north), increasing counter-clockwise.
  const float cp = noclip_cosf(pitch);
  const float sp = noclip_sinf(pitch);
  const float fwd_x = -noclip_sinf(yaw) * cp;
  const float fwd_y = noclip_cosf(yaw) * cp;
  const float fwd_z = sp;
  const float right_x = noclip_cosf(yaw);
  const float right_y = noclip_sinf(yaw);

  float len2 = fwd_in * fwd_in + side_in * side_in + up_in * up_in;
  if (len2 > 1.0f) {
    const float inv_len = 1.0f / sqrtf(len2);
    fwd_in *= inv_len;
    side_in *= inv_len;
    up_in *= inv_len;
  }
  if (fwd_in != 0.0f || side_in != 0.0f || up_in != 0.0f) {
    g_noclip_pos.x += (fwd_x * fwd_in + right_x * side_in) * step;
    g_noclip_pos.y += (fwd_y * fwd_in + right_y * side_in) * step;
    g_noclip_pos.z += (fwd_z * fwd_in + up_in) * step;
  }
  // Keep the latched position asserted every frame, including idle/menu-open frames. This is
  // what prevents the engine from accumulating a tiny downward settle into visible drift.
  noclip_place(entity);
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Toggle. The per-frame driver runs on the game thread via the frame hook, so noclip is
// refused until the hook is live (and is unavailable in a build without the frame hook) --
// the camera/move natives fault if called from the worker thread. The driver releases the
// frozen entity when it next sees the flag cleared, on the game thread.
static uint32_t toggle_noclip() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_NOCLIP;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.freeze_entity_position || !g_n.get_entity_coords || !g_n.is_control_pressed ||
      !(g_n.set_entity_coords_no_offset || g_n.set_entity_coords)) {
    return unavailable(action, "missing noclip natives");
  }
  if (!gtav_frame_hook_is_active()) {
    return unavailable(action, "needs main-thread hook");
  }
  g_noclip_enabled = !g_noclip_enabled;
  return ok(action, g_noclip_enabled ? "noclip on" : "noclip off");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}
