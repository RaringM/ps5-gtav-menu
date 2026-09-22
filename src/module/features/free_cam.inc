// Free / cinematic camera.
//
// Source fragment of features.cpp, textually included AFTER noclip.inc / object_move.inc /
// gun_toolkit.inc inside its anonymous namespace -- reuses noclip's libm-free trig (noclip_sinf/
// cosf), control reads (noclip_ctrl / noclip_axis), the GTAV_NOCLIP_* control ids + GTAV_NOCLIP_
// DEG2RAD, object_move's camera-origin helper (object_move_cam_coord), fly_hook_ready(), and the
// Vec3 / g_n / invoke helpers. Not a standalone TU.
//
// A DETACHED scripted camera, DISTINCT from noclip (which freezes + teleports the PED): here the
// player ped stays put and a script camera is created at the gameplay-camera pose, then flown by the
// sticks for screenshots / machinima. The per-frame driver runs on the GAME thread from
// gtav_features_game_thread_tick() -- the cam create/activate/render natives touch render-script
// state and fault on the scePad worker thread, exactly like noclip. Entering closes the menu;
// Circle exits and restores the gameplay camera. The mode flag + the speed cycler live OUTSIDE the
// frame-hook gate (so gtav_features_value_label + the cycler compile in a no-hook build); only the
// driver + the enter action are gated.

static const struct FreeCamSpeedRow {
  char label[8];  // shown in the cycler "< ... >" and as the action toast
  float mps;      // metres/sec at full stick (before the Sprint boost)
} kFreeCamSpeeds[] = {{"Slow", 8.0f}, {"Normal", 20.0f}, {"Fast", 50.0f}, {"Hyper", 120.0f}};

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Look sensitivity: degrees of yaw/pitch per full look-stick deflection per 60 Hz frame.
static const float GTAV_FREECAM_LOOK_DPS = 3.2f;
static const float GTAV_FREECAM_SPRINT_MULT = 3.0f;  // hold Sprint (L3/Shift) to fly faster
static const float GTAV_FREECAM_PITCH_LIMIT = 89.0f;
static const int GTAV_FREECAM_INPUT_LOOK_LR = 1;  // INPUT_LOOK_LR (right stick X)
static const int GTAV_FREECAM_INPUT_LOOK_UD = 2;  // INPUT_LOOK_UD (right stick Y)
static const int GTAV_FREECAM_INPUT_CANCEL = 202;  // INPUT_FRONTEND_CANCEL (Circle) -> exit

static int g_free_cam_handle = 0;       // active scripted cam (0 = none)
static int g_free_cam_started = 0;      // cam created + rendering this session
static int g_free_cam_frozen_entity = 0;  // entity (ped or vehicle) pinned in place this session
static uint32_t g_free_cam_last_frame = 0;
static float g_free_cam_x = 0.0f, g_free_cam_y = 0.0f, g_free_cam_z = 0.0f;  // current cam position
static float g_free_cam_yaw = 0.0f, g_free_cam_pitch = 0.0f;                 // degrees (accumulated)

// Tear the scripted cam down and hand control back to the gameplay camera.
static void free_cam_teardown(void) {
  if (g_free_cam_handle) {
    if (g_n.set_cam_active) invoke_void(g_n.set_cam_active, g_free_cam_handle, 0);
    if (g_n.destroy_cam) invoke_void(g_n.destroy_cam, g_free_cam_handle, 0);
    g_free_cam_handle = 0;
  }
  // RENDER_SCRIPT_CAMS(render, ease, easeTime, p3, p4): stop rendering script cams (back to
  // gameplay) with a short ease.
  if (g_n.render_script_cams) invoke_void(g_n.render_script_cams, 0, 1, 500, 1, 0);
  if (g_n.clear_focus) invoke_void(g_n.clear_focus);
  // Release the entity we pinned on enter so physics resumes. Single exit point for both the
  // !g_free_cam_active branch and the in-mode Circle exit, both game-thread, so the unfreeze
  // always lands in valid script context.
  if (g_free_cam_frozen_entity) {
    if (g_n.freeze_entity_position)
      invoke_void(g_n.freeze_entity_position, g_free_cam_frozen_entity, 0);
    if (g_n.set_entity_velocity)
      invoke_void(g_n.set_entity_velocity, g_free_cam_frozen_entity, 0.0f, 0.0f, 0.0f);
    g_free_cam_frozen_entity = 0;
  }
  g_free_cam_started = 0;
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick(). Creates the
// cam on the first tick after enter, then flies it along its own look basis each frame.
static void free_cam_game_thread_tick(void) {
  if (!g_free_cam_active) {
    if (g_free_cam_started) free_cam_teardown();
    return;
  }
  if (!g_n.create_cam_with_params || !g_n.set_cam_active || !g_n.render_script_cams ||
      !g_n.set_cam_coord || !g_n.set_cam_rot) {
    g_free_cam_active = 0;  // natives unavailable -> bail out of the mode cleanly
    g_free_cam_vis_request = 2;
    return;
  }

  // Collapse the many hook fires within one rendered frame to a single step.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_free_cam_last_frame) return;
    g_free_cam_last_frame = frame;
  }

  // Freeze the player while the free camera flies: entering free cam hides the menu, so the pad
  // suppression mask no longer gates gameplay buttons -- without this the sticks would walk/drive
  // the ped and the triggers would fire. Suppress every locomotion / combat / vehicle control each
  // frame; the cam still reads MOVE/LOOK because noclip_axis prefers GET_DISABLED_CONTROL_NORMAL.
  // INPUT_FRONTEND_CANCEL (202 / Circle) is intentionally left enabled so the exit check above is
  // never starved. Mirrors the noclip / object_move suppression pattern.
  if (g_n.disable_control_action) {
    static const int kSuppress[] = {
        21,  22,  36,           // SPRINT / JUMP / DUCK
        30,  31,  32,  33,  34, 35,  // MOVE_LR/UD + digital MOVE_UP/DOWN/LEFT/RIGHT
        1,   2,                 // LOOK_LR / LOOK_UD (cam reads them via the disabled getter)
        24,  25,  257,          // ATTACK / AIM / ATTACK2 (no firing while flying the cam)
        23,  37,  44,  45,      // ENTER / SELECT_WEAPON / COVER / RELOAD
        140, 141, 142, 143,     // melee light/heavy/alt/block
        59,  60,  71,  72,  75, 76,  // VEH_MOVE_LR/UD, ACCELERATE, BRAKE, EXIT, HANDBRAKE
    };
    for (unsigned i = 0; i < sizeof(kSuppress) / sizeof(kSuppress[0]); ++i)
      invoke_void(g_n.disable_control_action, 0, kSuppress[i], 1);
  }

  // Pin the player entity (the ped, or the whole vehicle when driving) so it cannot coast / fall /
  // slide on with its entry momentum while the camera flies free. The control-disable list above
  // only blocks NEW input; residual physics still carries a moving car or an airborne ped -- the
  // reported "free cam still lets the player move". Mirror noclip's freeze, but DON'T teleport or
  // drop collision/gravity (we only hold position, not relocate the entity), and re-pin on an
  // entity swap (player got in/out of a vehicle). Released in free_cam_teardown on exit.
  if (g_n.freeze_entity_position) {
    int fc_ent = current_player_entity();
    if (fc_ent) {
      if (g_free_cam_frozen_entity && g_free_cam_frozen_entity != fc_ent)
        invoke_void(g_n.freeze_entity_position, g_free_cam_frozen_entity, 0);
      if (g_free_cam_frozen_entity != fc_ent) {
        invoke_void(g_n.freeze_entity_position, fc_ent, 1);
        if (g_n.set_entity_velocity) invoke_void(g_n.set_entity_velocity, fc_ent, 0.0f, 0.0f, 0.0f);
        g_free_cam_frozen_entity = fc_ent;
      }
    }
  }

  // First tick: seed the pose from the gameplay camera and create + activate the script cam.
  if (!g_free_cam_started) {
    Vec3 c = object_move_cam_coord();  // GET_GAMEPLAY_CAM_COORD (or {0,0,0} if unresolved)
    Vec3 r = noclip_cam_rot();         // pitch=x, roll=y, yaw=z (degrees)
    g_free_cam_x = c.x;
    g_free_cam_y = c.y;
    g_free_cam_z = c.z;
    g_free_cam_pitch = r.x;
    g_free_cam_yaw = r.z;
    // CREATE_CAM_WITH_PARAMS(camName, x,y,z, rx,ry,rz, fov, active, rotOrder).
    g_free_cam_handle = invoke_return<int>(g_n.create_cam_with_params, "DEFAULT_SCRIPTED_CAMERA",
                                           g_free_cam_x, g_free_cam_y, g_free_cam_z, g_free_cam_pitch,
                                           0.0f, g_free_cam_yaw, 50.0f, 1, 2);
    invoke_void(g_n.set_cam_active, g_free_cam_handle, 1);
    invoke_void(g_n.render_script_cams, 1, 0, 0, 1, 0);
    g_free_cam_started = 1;
  }

  // Exit on Circle (read with the disabled variant so it works while the menu suppresses controls).
  if (noclip_ctrl(GTAV_FREECAM_INPUT_CANCEL)) {
    g_free_cam_active = 0;
    free_cam_teardown();
    g_free_cam_vis_request = 2;  // worker reopens the menu
    return;
  }

  // Look: accumulate yaw/pitch from the right stick (LOOK axes). Positive look-up should pitch up.
  const float look_lr = noclip_axis(GTAV_FREECAM_INPUT_LOOK_LR);
  const float look_ud = noclip_axis(GTAV_FREECAM_INPUT_LOOK_UD);
  g_free_cam_yaw -= look_lr * GTAV_FREECAM_LOOK_DPS;
  g_free_cam_pitch -= look_ud * GTAV_FREECAM_LOOK_DPS;
  if (g_free_cam_pitch > GTAV_FREECAM_PITCH_LIMIT) g_free_cam_pitch = GTAV_FREECAM_PITCH_LIMIT;
  if (g_free_cam_pitch < -GTAV_FREECAM_PITCH_LIMIT) g_free_cam_pitch = -GTAV_FREECAM_PITCH_LIMIT;

  // Movement basis from the accumulated orientation (GTA heading: yaw 0 = +Y).
  const float yaw = g_free_cam_yaw * GTAV_NOCLIP_DEG2RAD;
  const float pitch = g_free_cam_pitch * GTAV_NOCLIP_DEG2RAD;
  const float cp = noclip_cosf(pitch);
  const float fwd_x = -noclip_sinf(yaw) * cp;
  const float fwd_y = noclip_cosf(yaw) * cp;
  const float fwd_z = noclip_sinf(pitch);
  const float right_x = noclip_cosf(yaw);
  const float right_y = noclip_sinf(yaw);

  const uint32_t count = (uint32_t)(sizeof(kFreeCamSpeeds) / sizeof(kFreeCamSpeeds[0]));
  float speed = kFreeCamSpeeds[g_free_cam_speed_index % count].mps;
  if (noclip_ctrl(GTAV_NOCLIP_INPUT_SPRINT)) speed *= GTAV_FREECAM_SPRINT_MULT;
  const float step = speed * 0.016667f;  // per-60Hz-frame metres

  float fwd_in = -noclip_axis(GTAV_NOCLIP_INPUT_MOVE_UD);
  float side_in = noclip_axis(GTAV_NOCLIP_INPUT_MOVE_LR);
  float up_in = (float)(noclip_ctrl(GTAV_NOCLIP_INPUT_JUMP) - noclip_ctrl(GTAV_NOCLIP_INPUT_DUCK));
  float len2 = fwd_in * fwd_in + side_in * side_in + up_in * up_in;
  if (len2 > 1.0f) {
    const float inv = 1.0f / sqrtf(len2);
    fwd_in *= inv;
    side_in *= inv;
    up_in *= inv;
  }
  g_free_cam_x += (fwd_x * fwd_in + right_x * side_in) * step;
  g_free_cam_y += (fwd_y * fwd_in + right_y * side_in) * step;
  g_free_cam_z += (fwd_z * fwd_in + up_in) * step;

  // Apply the pose. Stream the world in around the cam so a far fly-out still has collision/props.
  invoke_void(g_n.set_cam_coord, g_free_cam_handle, g_free_cam_x, g_free_cam_y, g_free_cam_z);
  invoke_void(g_n.set_cam_rot, g_free_cam_handle, g_free_cam_pitch, 0.0f, g_free_cam_yaw, 2);
  if (g_n.set_focus_pos_and_vel)
    invoke_void(g_n.set_focus_pos_and_vel, g_free_cam_x, g_free_cam_y, g_free_cam_z, 0.0f, 0.0f,
                0.0f);
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Worker-side enter action: arm the mode + ask the worker to hide the menu. The driver creates the
// cam next frame. Refused until the frame hook is live (the cam natives run on that tick), mirroring
// enter_object_move_mode() / toggle_noclip().
static uint32_t enter_free_cam(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_FREE_CAM;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.create_cam_with_params || !g_n.set_cam_active || !g_n.render_script_cams ||
      !g_n.set_cam_coord || !g_n.set_cam_rot) {
    return unavailable(action, "missing camera natives");
  }
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  g_noclip_enabled = 0;             // mutual exclusion with noclip / object move
  g_object_move_mode_enabled = 0;
  g_free_cam_active = 1;
  g_free_cam_vis_request = 1;  // worker hides the menu; Circle (in-mode) reopens it
  return ok(action, "free camera (Circle to exit)");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

// Worker-side speed cycler (stage/apply; the driver reads the live value). No hook gate -- it only
// tunes a number that the driver consumes, and is harmless to set before entering the mode.
static uint32_t cycle_free_cam_speed(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_FREE_CAM_SPEED;
  const uint32_t count = (uint32_t)(sizeof(kFreeCamSpeeds) / sizeof(kFreeCamSpeeds[0]));
  if (param == 1u || param == 2u) {
    g_free_cam_speed_index = list_step(g_free_cam_speed_index, count, param == 2u ? -1 : 1);
  }
  return ok(action, kFreeCamSpeeds[g_free_cam_speed_index % count].label);
}

// Worker-safe: non-zero while the free camera is flying (the menu is hidden then). The render layer
// reads it to drive the native instructional-button bar + the free-cam text HUD. Plain volatile read
// of the game-thread-owned master flag -- no native call, so it is safe from any thread. The flag is
// declared outside the frame-hook gate, so this compiles (and reads 0) in a no-hook build too.
extern "C" int gtav_features_free_cam_active(void) { return g_free_cam_active ? 1 : 0; }
