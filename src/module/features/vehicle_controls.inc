// Vehicle Controls: doors / windows / headlights / locks / convertible roof.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace --
// shares g_n / g_state and the helpers defined in features.cpp. Not a standalone TU.
//
// Every action here is a ONE-SHOT, modify-existing field store on the player's current
// vehicle: the same worker-safe class as the LSC colour/mod setters and SET_VEHICLE_ENGINE_ON.
// They are NOT the heavy CVehicle::Fix rebuild (SET_VEHICLE_FIXED), which must run on the game
// thread, and they are NOT per-tick re-assertions (the lane that crashes off-thread) -- the user
// presses a button once. Worker-direct; needs hardware verification like the rest of the wave.

// Door selection for OPEN/CLOSE_VEHICLE_DOOR. Index 0 = "All Doors"; 1..6 map to GTA V door ids
// 0..5 (front L/R, rear L/R, hood, trunk).
static const char kVehicleDoorNames[][12] = {
    "All Doors", "Front Left", "Front Right", "Rear Left", "Rear Right", "Hood", "Trunk",
};
static uint32_t g_vehicle_door_index = 0;

// SET_VEHICLE_LIGHTS state: 0 = engine default/auto, 2 = force on, 1 = force off.
static const char kHeadlightNames[][12] = {"Default", "Force On", "Force Off"};
static const int kHeadlightStates[] = {0, 2, 1};
static uint32_t g_headlight_index = 0;

// SET_VEHICLE_DOORS_LOCKED status: 1 = unlocked, 2 = locked (players + AI).
static const char kVehicleLockNames[][12] = {"Unlocked", "Locked"};
static const int kVehicleLockStates[] = {1, 2};
static uint32_t g_vehicle_lock_index = 0;

// Pure selection cycler: Left/Right pick which door; it never applies a game effect (Open/Close
// Door read g_vehicle_door_index). Always shows the current selection.
static uint32_t cycle_vehicle_door(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_DOOR;
  const uint32_t count = (uint32_t)(sizeof(kVehicleDoorNames) / sizeof(kVehicleDoorNames[0]));
  stage_step(&g_vehicle_door_index, count, param);
  // Index directly (not LABEL_OF, which is defined in features.cpp after the .inc includes).
  return ok(action, kVehicleDoorNames[g_vehicle_door_index % count]);
}

static uint32_t open_close_vehicle_door(uint32_t action, int open) {
  if (open ? !g_n.set_vehicle_door_open : !g_n.set_vehicle_door_shut) {
    return unavailable(action, "missing vehicle door native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  const uint32_t count = (uint32_t)(sizeof(kVehicleDoorNames) / sizeof(kVehicleDoorNames[0]));
  uint32_t sel = g_vehicle_door_index % count;
  int lo = (sel == 0) ? 0 : (int)(sel - 1u);
  int hi = (sel == 0) ? 5 : (int)(sel - 1u);
  for (int d = lo; d <= hi; ++d) {
    // SET_VEHICLE_DOOR_OPEN(v, doorId, loose=0, openInstantly=0);
    // SET_VEHICLE_DOOR_SHUT(v, doorId, closeInstantly=0).
    if (open)
      invoke_void(g_n.set_vehicle_door_open, v, d, 0, 0);
    else
      invoke_void(g_n.set_vehicle_door_shut, v, d, 0);
  }
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "%s %s", open ? "opened" : "closed", kVehicleDoorNames[sel]);
  return ok(action, msg);
}

static uint32_t roll_vehicle_windows(uint32_t action, int down) {
  if (down ? !g_n.roll_down_window : !g_n.roll_up_window) {
    return unavailable(action, "missing vehicle window native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  // Window indices 0..3 = front L/R, rear L/R.
  for (int w = 0; w < 4; ++w) {
    if (down)
      invoke_void(g_n.roll_down_window, v, w);
    else
      invoke_void(g_n.roll_up_window, v, w);
  }
  return ok(action, down ? "windows rolled down" : "windows rolled up");
}

static uint32_t cycle_vehicle_headlights(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_HEADLIGHTS;
  const uint32_t count = (uint32_t)(sizeof(kHeadlightNames) / sizeof(kHeadlightNames[0]));
  if (stage_step(&g_headlight_index, count, param)) {
    return ok(action, kHeadlightNames[g_headlight_index]);
  }
  if (!g_n.set_vehicle_lights) return unavailable(action, "missing vehicle lights native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_lights, v, kHeadlightStates[g_headlight_index % count]);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "headlights: %s", kHeadlightNames[g_headlight_index % count]);
  return ok(action, msg);
}

static uint32_t cycle_vehicle_lock(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_VEHICLE_LOCK;
  const uint32_t count = (uint32_t)(sizeof(kVehicleLockNames) / sizeof(kVehicleLockNames[0]));
  if (stage_step(&g_vehicle_lock_index, count, param)) {
    return ok(action, kVehicleLockNames[g_vehicle_lock_index]);
  }
  if (!g_n.set_vehicle_doors_locked) return unavailable(action, "missing vehicle lock native");
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  invoke_void(g_n.set_vehicle_doors_locked, v, kVehicleLockStates[g_vehicle_lock_index % count]);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "doors %s", kVehicleLockNames[g_vehicle_lock_index % count]);
  return ok(action, msg);
}

static uint32_t convertible_roof(uint32_t action, int raise) {
  if (raise ? !g_n.raise_convertible_roof : !g_n.lower_convertible_roof) {
    return unavailable(action, "missing convertible roof native");
  }
  int v = current_vehicle();
  if (!v) return failed(action, "not in a vehicle");
  // IS_VEHICLE_A_CONVERTIBLE(v, p1=0): skip the no-op (and report it) on hardtops.
  if (g_n.is_vehicle_a_convertible && !invoke_return<int>(g_n.is_vehicle_a_convertible, v, 0)) {
    return unavailable(action, "not a convertible");
  }
  if (raise)
    invoke_void(g_n.raise_convertible_roof, v, 0);
  else
    invoke_void(g_n.lower_convertible_roof, v, 0);
  return ok(action, raise ? "roof raised" : "roof lowered");
}
