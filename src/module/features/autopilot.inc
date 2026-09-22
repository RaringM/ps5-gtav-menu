// Vehicle autopilot (self-driving).
//
// Source fragment of features.cpp, textually included inside its anonymous namespace
// (shares g_n / the invoke helpers / Vec3 / current_vehicle_strict / list_step). Not a
// standalone TU.
//
// The menu rows (Mode / Aggression / Drive Speed cyclers + Stop) only mutate the
// file-local state below on the scePad worker thread. The natives that actually drive the
// car -- TASK_VEHICLE_DRIVE_WANDER / TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE /
// SET_DRIVE_TASK_* / CLEAR_PED_TASKS -- issue a task against the player ped's task tree,
// which FAULTS off the game/script thread (the same class of crash noclip and the
// ped-control retasks hit). So all of them run exclusively from
// autopilot_game_thread_tick(), on the game thread via the frame hook, and the Mode cycler
// refuses to engage until gtav_frame_hook_is_active().

// Shared state -- read by the worker-side cyclers, gtav_features_value_label(), AND the
// game-thread tick, so it lives OUTSIDE the frame-hook gate. mode/dirty are volatile
// because the worker writes them and the game thread reads them.
enum { GTAV_AP_MODE_OFF = 0, GTAV_AP_MODE_WANDER = 1, GTAV_AP_MODE_WAYPOINT = 2 };
static volatile int g_autopilot_mode = GTAV_AP_MODE_OFF;
static uint32_t g_autopilot_aggression_index = 1u;  // default Normal
static uint32_t g_autopilot_speed_index = 1u;       // default Normal
static volatile int g_autopilot_dirty = 0;          // re-apply style/speed on next tick

static const struct ApModeRow {
  char label[14];  // shown in the cycler "< ... >" and as the action toast
} kApModes[] = {{"Off"}, {"Wander"}, {"To Waypoint"}};

// Aggression -> RAGE driving-style bitfield. Bits (named from the decompiled enum):
//   1 StopForVehicles | 2 StopForPeds | 4 SwerveAroundAllVehicles |
//   8 SteerAroundStationaryVehicles | 16 SteerAroundPeds | 32 SteerAroundObjects |
//   128 StopAtTrafficLights | 256 UseBlinkers | 262144 UseShortcutLinks |
//   524288 ChangeLanesAroundObstructions ...
// These are the widely-used composite presets; the bit semantics are exact but the
// aggression "feel" (and the speeds below) are starting values to tune in-game.
static const struct ApAggression {
  char label[16];
  int style;
} kAp[] = {
    {"CAUTIOUS", 786603},        // 0xC00EB obey all: stop for cars/peds/lights, avoid, blinkers
    {"NORMAL", 786468},          // 0xC0064 avoid traffic + shortcut/lane change, less rigid
    {"AGGRESSIVE", 786484},      // 0xC0034 no stop/lights, swerves cars(4)/peds(16)/objects(32)
                                 //         + shortcut(0x40000)/lane(0x80000): overtakes hard
    {"IGNORE TRAFFIC", 52},      // 0x34 swerve cars/peds/objects only -> beelines the target,
                                 //      ignoring all road laws but still avoiding collisions
    {"RECKLESS", 4},             // 0x4 SwerveAroundAllVehicles ONLY: no stop bits, no ped/object
                                 //     steer -> runs lights and plows peds, the most aggressive
                                 //     preset (the old 0x6 set StopForPeds(2), making it tamer
                                 //     than IGNORE TRAFFIC -- a non-monotonic aggression ladder)
};

// Drive speed -> cruise speed. TASK_VEHICLE_* speeds are in metres/sec.
static const struct ApSpeed {
  char label[8];
  float mps;
} kApSpd[] = {
    {"SLOW", 12.0f},    // ~43 km/h
    {"NORMAL", 22.0f},  // ~80 km/h
    {"FAST", 35.0f},    // ~126 km/h
    {"MAX", 60.0f},     // ~216 km/h
};

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Tick-only bookkeeping (game thread). Under the frame-hook gate like noclip's, so a
// non-hook build doesn't carry unused statics.
static uint32_t s_ap_last_frame = 0;
static int s_ap_engaged = 0;       // a drive task is currently issued
static int s_ap_last_vehicle = 0;  // vehicle handle the task was issued for
static int s_ap_last_mode = GTAV_AP_MODE_OFF;
static uint32_t s_ap_reissue_tick = 0;
static float s_ap_target_x = 0.0f;  // latched waypoint, for the arrival check
static float s_ap_target_y = 0.0f;
static int s_ap_have_target = 0;

// Re-assert a drive task every ~120 stepped frames (~2 s @60 fps): the AI finishes a
// wander leg or gets stuck, and re-issuing recovers it. Issuing EVERY frame would
// stutter / cancel the path, so we throttle.
static const uint32_t GTAV_AP_REISSUE_FRAMES = 120u;
// Waypoint stop radius (metres). Also the LONGRANGE stopRange. Compared squared in the
// arrival check so no sqrtf is needed (the payload doesn't link -lm).
static const float GTAV_AP_ARRIVE_RANGE = 25.0f;

// Gentle stop: CLEAR_PED_TASKS lets the car coast to a halt (vs _IMMEDIATELY's hard drop).
static void autopilot_clear(int ped) {
  if (ped && g_n.clear_ped_tasks) invoke_void(g_n.clear_ped_tasks, ped);
}

// Read the current map waypoint (blip sprite 8) x/y. Returns 1 if a waypoint is set.
// Same read as teleport_to_waypoint(); blip getters are safe on either thread.
static int autopilot_read_waypoint(float* out_x, float* out_y) {
  if (!g_n.get_first_blip_info_id || !g_n.get_blip_coords) return 0;
  int blip = invoke_return<int>(g_n.get_first_blip_info_id, 8);
  if (blip == 0 ||
      (g_n.get_blip_info_id_type && invoke_return<int>(g_n.get_blip_info_id_type, blip) == 0)) {
    return 0;
  }
  Vec3 wp = invoke_vector_blip(g_n.get_blip_coords, blip);
  *out_x = wp.x;
  *out_y = wp.y;
  return 1;
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick().
static void autopilot_game_thread_tick(void) {
  const int mode = g_autopilot_mode;

  // Disengaged: drop any task we issued, once, here on the game thread so the clear lands
  // in the same context as the task.
  if (mode == GTAV_AP_MODE_OFF) {
    if (s_ap_engaged) {
      autopilot_clear(native_player_ped_id());
      s_ap_engaged = 0;
      s_ap_last_vehicle = 0;
      s_ap_have_target = 0;
    }
    return;
  }
  if (!g_n.player_ped_id || !g_n.get_vehicle_ped_is_in) return;

  // Collapse the many hook fires within one rendered frame to a single step.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == s_ap_last_frame) return;
    s_ap_last_frame = frame;
  }

  int ped = native_player_ped_id();
  if (!ped) return;
  int veh = current_vehicle_strict();
  if (!veh) {  // on foot: nothing to drive. Re-engages automatically on re-entry.
    s_ap_engaged = 0;
    s_ap_last_vehicle = 0;
    return;
  }
  // Only drive when the player is the driver (seat -1); a passenger can't steer.
  if (g_n.get_ped_in_vehicle_seat &&
      invoke_return<int>(g_n.get_ped_in_vehicle_seat, veh, -1, 0) != ped) {
    return;
  }

  const ApAggression& agg = kAp[g_autopilot_aggression_index % (sizeof(kAp) / sizeof(kAp[0]))];
  const float speed = kApSpd[g_autopilot_speed_index % (sizeof(kApSpd) / sizeof(kApSpd[0]))].mps;

  // Waypoint arrival: squared 2D distance vs stop radius (no sqrtf). Disengage on arrival.
  if (mode == GTAV_AP_MODE_WAYPOINT && s_ap_engaged && s_ap_have_target && g_n.get_entity_coords) {
    Vec3 cur = invoke_vector(g_n.get_entity_coords, veh, 1);
    const float dx = cur.x - s_ap_target_x;
    const float dy = cur.y - s_ap_target_y;
    if (dx * dx + dy * dy <= GTAV_AP_ARRIVE_RANGE * GTAV_AP_ARRIVE_RANGE) {
      autopilot_clear(ped);
      g_autopilot_mode = GTAV_AP_MODE_OFF;  // menu Mode row reads back "Off"
      s_ap_engaged = 0;
      s_ap_last_vehicle = 0;
      s_ap_have_target = 0;
      return;
    }
  }

  // (Re)issue the whole task on engage, vehicle/mode change, or the throttled re-assert.
  int reissue = 0;
  if (!s_ap_engaged || veh != s_ap_last_vehicle || mode != s_ap_last_mode) reissue = 1;
  if (++s_ap_reissue_tick >= GTAV_AP_REISSUE_FRAMES) {
    s_ap_reissue_tick = 0;
    reissue = 1;
  }

  if (reissue) {
    if (mode == GTAV_AP_MODE_WANDER) {
      if (!g_n.task_vehicle_drive_wander) return;
      invoke_void(g_n.task_vehicle_drive_wander, ped, veh, speed, agg.style);
      s_ap_have_target = 0;
    } else {  // GTAV_AP_MODE_WAYPOINT
      float wx = 0.0f, wy = 0.0f;
      if (!autopilot_read_waypoint(&wx, &wy)) return;  // no waypoint yet: armed but idle
      // A waypoint blip only carries x/y. LONGRANGE pathfinds on roads, so seed z with the
      // vehicle's current height -- close enough for the road graph to take over.
      float wz = 0.0f;
      if (g_n.get_entity_coords) wz = invoke_vector(g_n.get_entity_coords, veh, 1).z;
      if (!g_n.task_vehicle_drive_to_coord_longrange) return;
      // TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(ped, vehicle, x, y, z, speed, driveMode, stopRange)
      invoke_void(g_n.task_vehicle_drive_to_coord_longrange, ped, veh, wx, wy, wz, speed, agg.style,
                  GTAV_AP_ARRIVE_RANGE);
      s_ap_target_x = wx;
      s_ap_target_y = wy;
      s_ap_have_target = 1;
    }
    s_ap_engaged = 1;
    s_ap_last_vehicle = veh;
    s_ap_last_mode = mode;
    g_autopilot_dirty = 0;
  } else if (g_autopilot_dirty) {
    // Style/speed changed while engaged: adjust the live task without re-pathing (smoother,
    // per the natives.h note on SET_DRIVE_TASK_CRUISE_SPEED).
    if (g_n.set_drive_task_driving_style)
      invoke_void(g_n.set_drive_task_driving_style, ped, agg.style);
    if (g_n.set_drive_task_cruise_speed) invoke_void(g_n.set_drive_task_cruise_speed, ped, speed);
    g_autopilot_dirty = 0;
  }
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Is the game-thread driver actually able to run? Only when built with the frame hook AND
// the hook is live. frame_hook.h is included by features.cpp only under the same gate, so
// the call must stay inside the #if (toggle_noclip does the same).
static int autopilot_hook_ready(void) {
#if GTAV_MENU_ENABLE_FRAME_HOOK
  return gtav_frame_hook_is_active();
#else
  return 0;
#endif
}

// Worker-side cyclers/action. They only advance the index/mode + raise the dirty flag; the
// game-thread tick reads the live values. Engaging a drive mode is refused until the frame
// hook is live (the drive natives run on that tick), mirroring toggle_noclip().
static uint32_t cycle_autopilot_mode(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_MODE;
  const uint32_t count = (uint32_t)(sizeof(kApModes) / sizeof(kApModes[0]));
  if (param == 1u || param == 2u) {
    g_autopilot_mode = (int)list_step((uint32_t)g_autopilot_mode, count, param == 2u ? -1 : 1);
    g_autopilot_dirty = 1;
  }
  const uint32_t mode = (uint32_t)g_autopilot_mode % count;
  if (mode != (uint32_t)GTAV_AP_MODE_OFF && !autopilot_hook_ready()) {
    return unavailable(action, "needs main-thread hook");
  }
  return ok(action, kApModes[mode].label);
}

static uint32_t cycle_autopilot_aggression(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_AGGRESSION;
  const uint32_t count = (uint32_t)(sizeof(kAp) / sizeof(kAp[0]));
  if (param == 1u || param == 2u) {
    g_autopilot_aggression_index =
        list_step(g_autopilot_aggression_index, count, param == 2u ? -1 : 1);
    g_autopilot_dirty = 1;
  }
  return ok(action, kAp[g_autopilot_aggression_index % count].label);
}

static uint32_t cycle_autopilot_speed(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_AUTOPILOT_SPEED;
  const uint32_t count = (uint32_t)(sizeof(kApSpd) / sizeof(kApSpd[0]));
  if (param == 1u || param == 2u) {
    g_autopilot_speed_index = list_step(g_autopilot_speed_index, count, param == 2u ? -1 : 1);
    g_autopilot_dirty = 1;
  }
  return ok(action, kApSpd[g_autopilot_speed_index % count].label);
}

static uint32_t stop_autopilot() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_STOP_AUTOPILOT;
  g_autopilot_mode = GTAV_AP_MODE_OFF;
  g_autopilot_dirty = 1;
  return ok(action, "autopilot off");
}
