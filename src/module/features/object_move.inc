// Interactive object placement / move mode.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace
// (shares g_n / the invoke helpers / Vec3, and reuses noclip.inc's camera + input helpers --
// noclip.inc is included immediately before this). Not a standalone TU.
//
// A per-frame GAME-THREAD effect that runs while the menu is CLOSED (the inverse of noclip's
// stand-down): the most-recently-spawned object (g_last_spawned_object) is FROZEN and then
// repositioned/reoriented from a latched pose driven by the controller, until the player
// commits (Cross) or reverts (Circle). Like noclip, the entity-move + camera natives fault on
// the scePad worker thread, so the driver lives here on the frame-hook game thread; the menu
// open/close is done on the worker (a request flag the worker consumes -- see
// gtav_features_take_menu_visibility_request) because the driver must not touch the
// worker-only logging path.
//
// Control scheme (PLAYER group 0, read via the disabled-control variants so the controls the
// driver consumes still report their raw state):
//   Left stick   (MOVE_LR 30 / MOVE_UD 31)   translate on the camera-relative ground plane
//   Right stick  (LOOK_LR 1 / LOOK_UD 2)      MOVE THE CAMERA -- NOT consumed, so you can orbit the
//                                             gameplay camera and look around the prop while placing
//   L1 / R1      (205 / 206)                  rotate the object (yaw left / right)
//   L2 / R2      (AIM 25 / ATTACK 24)         lower / raise the object (Z)
//   Square (203) tap                          one-shot snap-to-ground; D-pad Down toggles a
//                                             continuous ground-lock (auto-clamp every frame)
//   Sprint (21) held                          coarse (fast) step
//   Triangle (204) / L3 (209)                 duplicate / delete the prop; D-pad L/R select prop
//   Cross  (FRONTEND_ACCEPT 201)              commit: pin the object where placed
//   Circle (FRONTEND_CANCEL 202)              cancel: revert to the entry pose, or -- on auto-edit-
//                                             on-spawn -- delete the just-spawned prop (undo spawn)

#if GTAV_MENU_ENABLE_FRAME_HOOK

// GTA V control ids used by the move driver (PLAYER group 0). Movement/look reuse the same
// analog axes noclip drives; Cross/Circle are the frontend accept/cancel buttons.
enum {
  GTAV_OBJMOVE_INPUT_MOVE_LR = 30,  // left stick X  (strafe)
  GTAV_OBJMOVE_INPUT_MOVE_UD = 31,  // left stick Y  (forward/back)
  GTAV_OBJMOVE_INPUT_LOOK_LR = 1,   // right stick X (yaw)
  GTAV_OBJMOVE_INPUT_LOOK_UD = 2,   // right stick Y (height)
  GTAV_OBJMOVE_INPUT_SPRINT = 21,   // coarse modifier
  GTAV_OBJMOVE_INPUT_DUCK = 36,     // hold -> remap right stick to pitch/roll (3-axis only)
  GTAV_OBJMOVE_INPUT_ACCEPT = 201,  // Cross  -> commit
  GTAV_OBJMOVE_INPUT_CANCEL = 202,  // Circle -> revert
  // In-mode buttons (FRONTEND block, read group 0 like ACCEPT/CANCEL). Final mapping is
  // hardware-tuned (like the rest of this driver); these are the consistent defaults.
  GTAV_OBJMOVE_INPUT_SNAP_GROUND = 203,  // Square   -> drop to ground at current XY
  GTAV_OBJMOVE_INPUT_DUPLICATE = 204,    // Triangle -> clone the prop at the current pose
  GTAV_OBJMOVE_INPUT_DIST_DEC = 205,     // L1 -> aim distance nearer / free-move object yaw left
  GTAV_OBJMOVE_INPUT_DIST_INC = 206,     // R1 -> aim distance farther / free-move object yaw right
  GTAV_OBJMOVE_INPUT_HEIGHT_UP = 24,     // R2 (ATTACK) analog -> raise the prop in free-move
  GTAV_OBJMOVE_INPUT_HEIGHT_DOWN = 25,   // L2 (AIM) analog -> lower the prop in free-move
  GTAV_OBJMOVE_INPUT_DELETE = 209,       // L3 -> delete the prop
  GTAV_OBJMOVE_INPUT_SEL_PREV = 211,     // D-pad Left  -> previous placed object
  GTAV_OBJMOVE_INPUT_SEL_NEXT = 212,     // D-pad Right -> next placed object
  GTAV_OBJMOVE_INPUT_AIM_TOGGLE = 213,   // D-pad Up   -> toggle aim-follow sub-mode
  GTAV_OBJMOVE_INPUT_SNAP_TOGGLE = 214,  // D-pad Down -> toggle surface raycast snap
};

static const float GTAV_OBJMOVE_BASE_MPS = 2.5f;     // full-stick metres/sec (fine)
static const float GTAV_OBJMOVE_COARSE_MULT = 6.0f;  // hold Sprint to move/rotate faster
static const float GTAV_OBJMOVE_YAW_DPS = 90.0f;     // full-stick degrees/sec (fine)
static const float GTAV_OBJMOVE_DIST_MIN = 0.5f;     // aim-follow distance clamp (metres)
static const float GTAV_OBJMOVE_DIST_MAX = 100.0f;
static const float GTAV_OBJMOVE_PROBE_LEN = 1000.0f;  // surface-snap ray length (metres)

// Latched authoritative pose (written + asserted every frame so a frozen object's physics
// can't drift it). Sampled from the object on acquire; the entry copy is restored on Circle.
static Vec3 g_object_move_pos;
static float g_object_move_heading;
static float g_object_move_pitch = 0.0f;  // Phase 2 (3-axis); stays 0 with heading-only natives
static float g_object_move_roll = 0.0f;
static Vec3 g_object_move_entry_pos;
static float g_object_move_entry_heading;
static float g_object_move_entry_pitch = 0.0f;
static float g_object_move_entry_roll = 0.0f;
static int g_object_move_coarse = 0;  // last coarse-modifier state (for the HUD)
// Per-frame dedup + frame-delta baseline, same idea as noclip's latches.
static uint32_t g_object_move_last_frame = 0;
static uint32_t g_object_move_last_ms = 0;
// Edge latches for the commit/cancel buttons (no *_JUST_PRESSED native is resolved, so the
// driver detects the rising edge itself).
static int g_object_move_prev_accept = 0;
static int g_object_move_prev_cancel = 0;
// Edge latches for the in-mode action buttons (snap / duplicate / delete / selection / sub-mode
// toggles). Read + latched together each frame so an early return (target switch) can't strand a
// latch and double-fire the next frame.
static int g_object_move_prev_snap_ground = 0;
static int g_object_move_prev_dup = 0;
static int g_object_move_prev_delete = 0;
static int g_object_move_prev_sel_prev = 0;
static int g_object_move_prev_sel_next = 0;
static int g_object_move_prev_aim_toggle = 0;
static int g_object_move_prev_snap_toggle = 0;

static float object_move_wrap_deg(float deg) {
  while (deg >= 360.0f) deg -= 360.0f;
  while (deg < 0.0f) deg += 360.0f;
  return deg;
}

// Full pitch/roll/yaw is available only when both rotation natives resolved; otherwise the
// driver orients with SET_ENTITY_HEADING (yaw only). Guarding here keeps the build correct
// whether or not the crossmap pinned SET/GET_ENTITY_ROTATION.
static int object_move_can_rotate3(void) {
  return g_n.set_entity_rotation && g_n.get_entity_rotation;
}

// Aim-follow needs the camera origin; surface snap additionally needs the shape-test pair. Both
// degrade off when unresolved (the toggles refuse, the driver stays in the available mode).
static int object_move_aim_can(void) { return g_n.get_gameplay_cam_coord != 0; }
static int object_move_surface_can(void) {
  return g_n.start_shape_test_probe && g_n.get_shape_test_result;
}

// GET_GAMEPLAY_CAM_COORD() -> scrVector camera origin, no args (the same direct-call shape as
// noclip_cam_rot's GET_GAMEPLAY_CAM_ROT). Returns 0,0,0 when unresolved.
static Vec3 object_move_cam_coord(void) {
  Vec3 zero = {0.0f, 0.0f, 0.0f};
  if (!g_n.get_gameplay_cam_coord) return zero;
  gtavmenu::NativeCallContext ctx;
  ctx.reset();
  gtavmenu::note_native_call(g_n.get_gameplay_cam_coord);  // crash-breadcrumb parity with the helpers
  ((void (*)(gtavmenu::NativeArg*))(uintptr_t)g_n.get_gameplay_cam_coord)(&ctx.native_arg);
  Vec3 v;
  v.x = *reinterpret_cast<const float*>(&ctx.returns[0]);
  v.y = *reinterpret_cast<const float*>(&ctx.returns[1]);
  v.z = *reinterpret_cast<const float*>(&ctx.returns[2]);
  return v;
}

// Live menu-spawned objects (roster entries that still exist). The edit cursor walks this set;
// dead handles are skipped (the roster compacts them lazily on the next spawn/track).
static int object_move_live_count(void) {
  int n = 0;
  for (int i = 0; i < g_spawned_object_count; ++i)
    if (roster_handle_alive(g_spawned_object_roster[i])) ++n;
  return n;
}
static int object_move_live_at(int idx) {
  int live = 0;
  for (int i = 0; i < g_spawned_object_count; ++i) {
    const int h = g_spawned_object_roster[i];
    if (!roster_handle_alive(h)) continue;
    if (live == idx) return h;
    ++live;
  }
  return 0;
}
static int object_move_index_of(int handle) {
  int live = 0;
  for (int i = 0; i < g_spawned_object_count; ++i) {
    const int h = g_spawned_object_roster[i];
    if (!roster_handle_alive(h)) continue;
    if (h == handle) return live;
    ++live;
  }
  return -1;
}

// Resolve the current edit target. sel_index < 0 is the back-compatible default: the last-spawned
// object if still alive, else the most-recently-spawned live roster entry.
static int object_move_target(void) {
  const int count = object_move_live_count();
  if (count <= 0) return g_last_spawned_object;  // 0/dead -> caller aborts the mode
  if (g_object_move_sel_index < 0) {
    if (roster_handle_alive(g_last_spawned_object)) return g_last_spawned_object;
    return object_move_live_at(count - 1);
  }
  if (g_object_move_sel_index >= count) g_object_move_sel_index = count - 1;
  return object_move_live_at(g_object_move_sel_index);
}

// Clear every rising-edge latch (so a held button can't carry across sessions).
static void object_move_reset_latches(void) {
  g_object_move_prev_accept = 0;
  g_object_move_prev_cancel = 0;
  g_object_move_prev_snap_ground = 0;
  g_object_move_prev_dup = 0;
  g_object_move_prev_delete = 0;
  g_object_move_prev_sel_prev = 0;
  g_object_move_prev_sel_next = 0;
  g_object_move_prev_aim_toggle = 0;
  g_object_move_prev_snap_toggle = 0;
}

// Release the driver's hold on a prop WITHOUT moving the menu: re-enable its collision (solid
// again) and clear the acquired handle; FREEZE stays on so it holds its placed pose. Used when
// switching the edit target (selection / duplicate). Resets the frame-delta baseline.
static void object_move_detach(int e) {
  if (e && g_n.set_entity_collision) invoke_void(g_n.set_entity_collision, e, 1, 1);
  g_object_move_entity = 0;
  g_object_move_last_ms = 0;
}

// Finish a move session: leave the prop solid + frozen (pinned where placed), drop the driver's
// hold, and ask the worker to show the menu. Called from commit/cancel and from the disabled path
// (external clear, e.g. noclip mutual exclusion or shutdown). Idempotent via g_object_move_entity.
static void object_move_finish(int e) {
  object_move_detach(e);
  g_object_move_vis_request = 2;  // worker reopens the menu
}

// Acquire an object: freeze it, drop its collision so it ghost-moves, and latch its current
// pose (plus an entry copy for revert).
static void object_move_acquire(int e) {
  if (g_n.freeze_entity_position) invoke_void(g_n.freeze_entity_position, e, 1);
  if (g_n.set_entity_collision) invoke_void(g_n.set_entity_collision, e, 0, 0);
  g_object_move_pos = invoke_vector(g_n.get_entity_coords, e, 1);
  if (object_move_can_rotate3()) {
    // GET_ENTITY_ROTATION(entity, rotationOrder) -> (pitch=x, roll=y, yaw=z), order 2 = ZXY.
    const Vec3 rot = invoke_vector(g_n.get_entity_rotation, e, 2);
    g_object_move_pitch = rot.x;
    g_object_move_roll = rot.y;
    g_object_move_heading = rot.z;
  } else {
    g_object_move_heading =
        g_n.get_entity_heading ? invoke_return<float>(g_n.get_entity_heading, e) : 0.0f;
    g_object_move_pitch = 0.0f;
    g_object_move_roll = 0.0f;
  }
  g_object_move_entry_pos = g_object_move_pos;
  g_object_move_entry_heading = g_object_move_heading;
  g_object_move_entry_pitch = g_object_move_pitch;
  g_object_move_entry_roll = g_object_move_roll;
  g_object_move_entity = e;
}

// Assert the latched pose onto the (frozen) object: position, then orientation (full 3-axis via
// SET_ENTITY_ROTATION when available, else yaw-only via SET_ENTITY_HEADING).
static void object_move_apply(int e) {
  const uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  if (!e || !set_coords) return;
  if (g_n.set_entity_coords_no_offset) {
    invoke_void(set_coords, e, g_object_move_pos.x, g_object_move_pos.y, g_object_move_pos.z, 0, 0,
                1);
  } else {
    invoke_void(set_coords, e, g_object_move_pos.x, g_object_move_pos.y, g_object_move_pos.z, 0, 0,
                0, 0);
  }
  if (object_move_can_rotate3()) {
    // SET_ENTITY_ROTATION(entity, pitch, roll, yaw, rotationOrder, p5), order 2 = ZXY.
    invoke_void(g_n.set_entity_rotation, e, g_object_move_pitch, g_object_move_roll,
                g_object_move_heading, 2, 1);
  } else if (g_n.set_entity_heading) {
    invoke_void(g_n.set_entity_heading, e, g_object_move_heading);
  }
}

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick(). Drives
// the latched pose from the controller while the menu is closed and asserts it every frame.
static void object_move_game_thread_tick(void) {
  if (!g_object_move_mode_enabled) {
    // Cleared externally (mutual exclusion with noclip, shutdown) without a Cross/Circle:
    // finish the session once so the object is left solid + the menu reopens.
    if (g_object_move_entity) object_move_finish(g_object_move_entity);
    return;
  }
  const uint64_t set_coords =
      g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
  if (!set_coords || !g_n.get_entity_coords) return;

  // Collapse the many hook fires within one rendered frame to a single step.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_object_move_last_frame) return;
    g_object_move_last_frame = frame;
  }

  int e = object_move_target();
  if (!roster_handle_alive(e)) {
    // No live target (all despawned / cleared): abort cleanly and reopen the menu.
    g_object_move_mode_enabled = 0;
    g_object_move_entity = 0;
    g_object_move_vis_request = 2;
    return;
  }
  if (g_object_move_entity != e) object_move_acquire(e);

  // First tick after entry: seed every button latch from the LIVE pad state (read via the disabled
  // getters) so a button still physically held from the spawn/entry press is NOT misread as a fresh
  // press. Without this the held Cross from the spawn/select press fired the commit on the first
  // menu-closed frame and the prop "placed" before the user could move it. Done regardless of menu
  // visibility so the held press is consumed even if the panel is briefly still up.
  if (g_object_move_consume_entry) {
    g_object_move_consume_entry = 0;
    g_object_move_prev_accept = noclip_ctrl(GTAV_OBJMOVE_INPUT_ACCEPT);
    g_object_move_prev_cancel = noclip_ctrl(GTAV_OBJMOVE_INPUT_CANCEL);
    g_object_move_prev_snap_ground = noclip_ctrl(GTAV_OBJMOVE_INPUT_SNAP_GROUND);
    g_object_move_prev_dup = noclip_ctrl(GTAV_OBJMOVE_INPUT_DUPLICATE);
    g_object_move_prev_delete = noclip_ctrl(GTAV_OBJMOVE_INPUT_DELETE);
    g_object_move_prev_sel_prev = noclip_ctrl(GTAV_OBJMOVE_INPUT_SEL_PREV);
    g_object_move_prev_sel_next = noclip_ctrl(GTAV_OBJMOVE_INPUT_SEL_NEXT);
    g_object_move_prev_aim_toggle = noclip_ctrl(GTAV_OBJMOVE_INPUT_AIM_TOGGLE);
    g_object_move_prev_snap_toggle = noclip_ctrl(GTAV_OBJMOVE_INPUT_SNAP_TOGGLE);
  }

  // Frame-delta scale so the step is frame-rate independent (1.0 @60, ~2.0 @30), clamped.
  float step_scale = 1.0f;
  if (g_n.get_game_timer) {
    const uint32_t now_ms = invoke_return<uint32_t>(g_n.get_game_timer);
    if (g_object_move_last_ms != 0u) {
      uint32_t dt = now_ms - g_object_move_last_ms;
      if (dt > 100u) dt = 100u;
      step_scale = (float)dt / 16.667f;
    }
    g_object_move_last_ms = now_ms;
  }

  // Drive input only while the menu is closed (the move-mode contract). If the player reopens
  // the menu mid-edit, the object stays frozen at its current pose and we consume no input.
  if (!g_menu_open) {
    // Suppress the player's locomotion/fire/in-mode buttons so the ped stays put and the buttons
    // don't trigger gameplay. The LOOK axes are NOT suppressed in either mode now -- the right stick
    // drives the free gameplay camera so the user can look around while placing (object yaw moved to
    // L1/R1, object height to the L2/R2 triggers). The L1/R1 bumpers (205/206) + the L2/R2 triggers
    // read as ATTACK/AIM (24/25) are suppressed so the menu owns them while placing.
    if (g_n.disable_control_action) {
      static const int kSuppressBase[] = {
          GTAV_OBJMOVE_INPUT_MOVE_LR, GTAV_OBJMOVE_INPUT_MOVE_UD, GTAV_OBJMOVE_INPUT_SPRINT,
          GTAV_OBJMOVE_INPUT_ACCEPT,  GTAV_OBJMOVE_INPUT_CANCEL,
          32, 33, 34, 35,  // MOVE_UP/DOWN/LEFT/RIGHT (digital walk)
          22, 36,          // JUMP / DUCK
          24, 25,          // ATTACK / AIM = R2/L2 (object height; never fire while placing)
          44, 45,          // COVER / RELOAD
          23, 27, 37,      // ENTER / PHONE / SELECT_WEAPON (the in-mode buttons map here on foot)
          205, 206,        // L1 / R1 = object yaw (don't leak to weapon-select etc.)
      };
      for (unsigned i = 0; i < sizeof(kSuppressBase) / sizeof(kSuppressBase[0]); ++i)
        invoke_void(g_n.disable_control_action, 0, kSuppressBase[i], 1);
    }

    const int coarse = noclip_ctrl(GTAV_OBJMOVE_INPUT_SPRINT);
    g_object_move_coarse = coarse;
    const float speed_mult = coarse ? GTAV_OBJMOVE_COARSE_MULT : 1.0f;
    const float lin_step = GTAV_OBJMOVE_BASE_MPS * speed_mult * (step_scale * 0.016667f);
    const float yaw_step = GTAV_OBJMOVE_YAW_DPS * speed_mult * (step_scale * 0.016667f);

    // Read every edge-triggered button up front and latch it, so an early return (a target
    // switch) can't strand a latch and double-fire next frame.
    const int snap_ground = noclip_ctrl(GTAV_OBJMOVE_INPUT_SNAP_GROUND);
    const int dup = noclip_ctrl(GTAV_OBJMOVE_INPUT_DUPLICATE);
    const int del = noclip_ctrl(GTAV_OBJMOVE_INPUT_DELETE);
    const int sel_prev = noclip_ctrl(GTAV_OBJMOVE_INPUT_SEL_PREV);
    const int sel_next = noclip_ctrl(GTAV_OBJMOVE_INPUT_SEL_NEXT);
    const int aim_tog = noclip_ctrl(GTAV_OBJMOVE_INPUT_AIM_TOGGLE);
    const int snap_tog = noclip_ctrl(GTAV_OBJMOVE_INPUT_SNAP_TOGGLE);
    const int snap_ground_edge = snap_ground && !g_object_move_prev_snap_ground;
    const int dup_edge = dup && !g_object_move_prev_dup;
    const int del_edge = del && !g_object_move_prev_delete;
    const int sel_prev_edge = sel_prev && !g_object_move_prev_sel_prev;
    const int sel_next_edge = sel_next && !g_object_move_prev_sel_next;
    const int aim_tog_edge = aim_tog && !g_object_move_prev_aim_toggle;
    const int snap_tog_edge = snap_tog && !g_object_move_prev_snap_toggle;
    g_object_move_prev_snap_ground = snap_ground;
    g_object_move_prev_dup = dup;
    g_object_move_prev_delete = del;
    g_object_move_prev_sel_prev = sel_prev;
    g_object_move_prev_sel_next = sel_next;
    g_object_move_prev_aim_toggle = aim_tog;
    g_object_move_prev_snap_toggle = snap_tog;

    // --- Target-changing buttons: act, detach the old prop, and bail (next tick re-acquires) ---
    const int live = object_move_live_count();
    if ((sel_prev_edge || sel_next_edge) && live > 1) {
      int cur = (g_object_move_sel_index < 0) ? object_move_index_of(e) : g_object_move_sel_index;
      if (cur < 0) cur = live - 1;
      cur = sel_next_edge ? (cur + 1) % live : (cur + live - 1) % live;
      g_object_move_sel_index = cur;
      g_object_move_cancel_deletes = 0;  // editing a different prop now -> Circle reverts, not delete
      object_move_detach(e);  // leave the old prop solid; the next tick acquires the new target
      return;
    }
    if (del_edge && g_n.delete_entity) {
      int handle = e;
      if (g_n.set_entity_as_mission_entity)
        invoke_void(g_n.set_entity_as_mission_entity, handle, 1, 1);
      invoke_void(g_n.delete_entity, &handle);  // DELETE_ENTITY(Entity*) zeros the handle
      if (g_last_spawned_object == e) g_last_spawned_object = 0;
      g_object_move_cancel_deletes = 0;  // target consumed
      g_object_move_entity = 0;  // the prop is gone -> nothing to restore
      g_object_move_last_ms = 0;
      const int remaining = object_move_live_count();
      if (remaining <= 0) {
        g_object_move_mode_enabled = 0;
        g_object_move_vis_request = 2;  // nothing left to edit -> reopen the menu
      } else if (g_object_move_sel_index >= remaining) {
        g_object_move_sel_index = remaining - 1;
      }
      return;
    }
    if (dup_edge && g_n.create_object) {
      const int model = spawned_object_model_of(e);
      if (model) {
        // CREATE_OBJECT(modelHash, x, y, z, isNetwork, thisScriptCheck, dynamic) at the current
        // pose; freeze immediately so it can't settle before the next tick acquires it.
        int obj = invoke_return<int>(g_n.create_object, model, g_object_move_pos.x,
                                     g_object_move_pos.y, g_object_move_pos.z, 0, 1, 1);
        if (obj) {
          if (g_n.freeze_entity_position) invoke_void(g_n.freeze_entity_position, obj, 1);
          if (object_move_can_rotate3())
            invoke_void(g_n.set_entity_rotation, obj, g_object_move_pitch, g_object_move_roll,
                        g_object_move_heading, 2, 1);
          else if (g_n.set_entity_heading)
            invoke_void(g_n.set_entity_heading, obj, g_object_move_heading);
          if (g_n.set_entity_as_mission_entity)
            invoke_void(g_n.set_entity_as_mission_entity, obj, 1, 1);
          track_spawned_object_model(obj, model);
          g_last_spawned_object = obj;
          g_object_move_sel_index = -1;  // edit the fresh copy (the last-spawned default)
          g_object_move_cancel_deletes = 0;  // editing the clone now -> Circle reverts, not delete
          object_move_detach(e);         // leave the source solid
          return;
        }
      }
    }

    // --- Sub-mode toggles (no target change) ---
    if (aim_tog_edge && object_move_aim_can()) {
      g_object_move_aim_mode = !g_object_move_aim_mode;
      if (g_object_move_aim_mode) {
        // Seed the follow distance from the current camera->prop gap so it doesn't jump on entry.
        const Vec3 cam = object_move_cam_coord();
        const float dx = g_object_move_pos.x - cam.x;
        const float dy = g_object_move_pos.y - cam.y;
        const float dz = g_object_move_pos.z - cam.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < GTAV_OBJMOVE_DIST_MIN) d = 3.0f;
        if (d > GTAV_OBJMOVE_DIST_MAX) d = GTAV_OBJMOVE_DIST_MAX;
        g_object_move_aim_dist = d;
      } else {
        g_object_move_surface_snap = 0;  // snap is an aim sub-layer
      }
    }
    if (snap_tog_edge && g_object_move_aim_mode && object_move_surface_can())
      g_object_move_surface_snap = !g_object_move_surface_snap;
    // D-pad Down in FREE-MOVE (where surface-snap is aim-only) toggles continuous ground-lock: the
    // prop's Z follows the terrain under its XY each frame, so dragging it never leaves it floating.
    if (snap_tog_edge && !g_object_move_aim_mode)
      g_object_move_ground_lock = !g_object_move_ground_lock;

    // --- Positioning: aim-follow (prop tracks the camera) or free-move (stick translate) ---
    if (g_object_move_aim_mode && object_move_aim_can()) {
      if (noclip_ctrl(GTAV_OBJMOVE_INPUT_DIST_INC)) g_object_move_aim_dist += lin_step;
      if (noclip_ctrl(GTAV_OBJMOVE_INPUT_DIST_DEC)) g_object_move_aim_dist -= lin_step;
      if (g_object_move_aim_dist < GTAV_OBJMOVE_DIST_MIN)
        g_object_move_aim_dist = GTAV_OBJMOVE_DIST_MIN;
      if (g_object_move_aim_dist > GTAV_OBJMOVE_DIST_MAX)
        g_object_move_aim_dist = GTAV_OBJMOVE_DIST_MAX;

      // Project the prop along the camera forward (yaw + pitch), same basis convention as noclip.
      const Vec3 cam = object_move_cam_coord();
      const Vec3 rot = noclip_cam_rot();
      const float yaw = rot.z * GTAV_NOCLIP_DEG2RAD;
      const float pitch = rot.x * GTAV_NOCLIP_DEG2RAD;
      const float cp = noclip_cosf(pitch);
      const float fx = -noclip_sinf(yaw) * cp;
      const float fy = noclip_cosf(yaw) * cp;
      const float fz = noclip_sinf(pitch);
      float px = cam.x + fx * g_object_move_aim_dist;
      float py = cam.y + fy * g_object_move_aim_dist;
      float pz = cam.z + fz * g_object_move_aim_dist;
      // Surface snap: raycast along the forward and use the first world/object hit instead.
      if (g_object_move_surface_snap && object_move_surface_can()) {
        const float ex = cam.x + fx * GTAV_OBJMOVE_PROBE_LEN;
        const float ey = cam.y + fy * GTAV_OBJMOVE_PROBE_LEN;
        const float ez = cam.z + fz * GTAV_OBJMOVE_PROBE_LEN;
        // flags 1|16 = world + objects; ignore self (e); options 7. Synchronous probe -> the
        // result is ready on the same frame.
        const int h = invoke_return<int>(g_n.start_shape_test_probe, cam.x, cam.y, cam.z, ex, ey,
                                         ez, 1 | 16, e, 7);
        int hit_flag = 0;
        int hit_ent = 0;
        // 24-byte scrVector scratch (x@+0, y@+8, z@+16) -- the layout apply_vector_results drains
        // a Vector3* out-param into. A 12-byte Vec3 would be the wrong shape here.
        uint64_t end_sv[3] = {0, 0, 0};
        uint64_t norm_sv[3] = {0, 0, 0};
        invoke_return<int>(g_n.get_shape_test_result, h, (uint64_t)(uintptr_t)&hit_flag,
                           (uint64_t)(uintptr_t)end_sv, (uint64_t)(uintptr_t)norm_sv,
                           (uint64_t)(uintptr_t)&hit_ent);
        if (hit_flag) {
          px = *reinterpret_cast<const float*>(&end_sv[0]);
          py = *reinterpret_cast<const float*>(&end_sv[1]);
          pz = *reinterpret_cast<const float*>(&end_sv[2]);
        }
      }
      g_object_move_pos.x = px;
      g_object_move_pos.y = py;
      g_object_move_pos.z = pz;
    } else {
      // Left stick -> camera-relative translate on the ground plane (noclip's yaw read + basis).
      const float fwd_in = -noclip_axis(GTAV_OBJMOVE_INPUT_MOVE_UD);
      const float side_in = noclip_axis(GTAV_OBJMOVE_INPUT_MOVE_LR);
      if (fwd_in != 0.0f || side_in != 0.0f) {
        const Vec3 rot = noclip_cam_rot();
        const float yaw = rot.z * GTAV_NOCLIP_DEG2RAD;
        const float fwd_x = -noclip_sinf(yaw);
        const float fwd_y = noclip_cosf(yaw);
        const float right_x = noclip_cosf(yaw);
        const float right_y = noclip_sinf(yaw);
        g_object_move_pos.x += (fwd_x * fwd_in + right_x * side_in) * lin_step;
        g_object_move_pos.y += (fwd_y * fwd_in + right_y * side_in) * lin_step;
      }

      // Right stick now drives the gameplay CAMERA (freed above). Object yaw is on L1/R1 (digital);
      // object height on the L2/R2 analog triggers (R2 up, L2 down). Hold Duck (3-axis builds only):
      // the bumpers become roll and the triggers become pitch.
      const int rotate3 = object_move_can_rotate3() && noclip_ctrl(GTAV_OBJMOVE_INPUT_DUCK);
      const float yaw_in = (float)(noclip_ctrl(GTAV_OBJMOVE_INPUT_DIST_INC) -
                                   noclip_ctrl(GTAV_OBJMOVE_INPUT_DIST_DEC));
      const float z_in = noclip_axis(GTAV_OBJMOVE_INPUT_HEIGHT_UP) -
                         noclip_axis(GTAV_OBJMOVE_INPUT_HEIGHT_DOWN);
      if (rotate3) {
        if (yaw_in != 0.0f)
          g_object_move_roll = object_move_wrap_deg(g_object_move_roll + yaw_in * yaw_step);
        if (z_in != 0.0f)
          g_object_move_pitch = object_move_wrap_deg(g_object_move_pitch + z_in * yaw_step);
      } else {
        if (yaw_in != 0.0f)  // R1 -> rotate clockwise, L1 -> counter
          g_object_move_heading = object_move_wrap_deg(g_object_move_heading + yaw_in * yaw_step);
        if (z_in != 0.0f) g_object_move_pos.z += z_in * lin_step;
      }

      // Continuous ground-lock: glue Z to the terrain under the current XY each frame (probe from
      // just above so a prop already sunk slightly still resolves the surface). Toggled by D-pad
      // Down above; the one-shot Square snap (below) still works independently.
      if (g_object_move_ground_lock && g_n.get_ground_z_for_3d_coord) {
        float gz = 0.0f;
        if (invoke_return<int>(g_n.get_ground_z_for_3d_coord, g_object_move_pos.x,
                               g_object_move_pos.y, g_object_move_pos.z + 1.0f,
                               (uint64_t)(uintptr_t)&gz, 0, 0))
          g_object_move_pos.z = gz;
      }
    }

    // Ground snap (both modes): drop Z to the ground at the current XY.
    if (snap_ground_edge && g_n.get_ground_z_for_3d_coord) {
      float gz = 0.0f;
      // GET_GROUND_Z_FOR_3D_COORD(x, y, z, groundZ*, ignoreWater, p5): pass all 6 args -- the
      // trailing p5 was omitted, relying on the arg slot being zeroed by reset().
      if (invoke_return<int>(g_n.get_ground_z_for_3d_coord, g_object_move_pos.x,
                             g_object_move_pos.y, g_object_move_pos.z, (uint64_t)(uintptr_t)&gz, 0,
                             0))
        g_object_move_pos.z = gz;
    }
  }

  // Re-assert the latched pose every frame, including idle / menu-open frames.
  object_move_apply(e);

  if (g_menu_open) return;  // commit/cancel only while the panel is hidden

  // Commit / revert on the rising edge (latch prior state; no *_JUST_PRESSED native).
  const int accept = noclip_ctrl(GTAV_OBJMOVE_INPUT_ACCEPT);
  const int cancel = noclip_ctrl(GTAV_OBJMOVE_INPUT_CANCEL);
  if (accept && !g_object_move_prev_accept) {
    g_object_move_cancel_deletes = 0;
    g_object_move_mode_enabled = 0;
    object_move_finish(e);  // pinned where placed
  } else if (cancel && !g_object_move_prev_cancel) {
    if (g_object_move_cancel_deletes && g_n.delete_entity) {
      // Auto-edit-on-spawn: Circle cancels the SPAWN -> delete the just-spawned prop (mirror the L3
      // delete path; do NOT object_move_finish a freed handle), then reopen the menu.
      int handle = e;
      if (g_n.set_entity_as_mission_entity)
        invoke_void(g_n.set_entity_as_mission_entity, handle, 1, 1);
      invoke_void(g_n.delete_entity, &handle);  // DELETE_ENTITY(Entity*) zeros the handle
      if (g_last_spawned_object == e) g_last_spawned_object = 0;
      g_object_move_cancel_deletes = 0;
      g_object_move_entity = 0;
      g_object_move_last_ms = 0;
      g_object_move_mode_enabled = 0;
      g_object_move_vis_request = 2;  // canceled the spawn -> reopen the menu
      return;
    }
    // Manual edit: revert to the entry pose and release the (pre-existing) prop.
    g_object_move_pos = g_object_move_entry_pos;
    g_object_move_heading = g_object_move_entry_heading;
    g_object_move_pitch = g_object_move_entry_pitch;
    g_object_move_roll = g_object_move_entry_roll;
    object_move_apply(e);  // snap back before releasing
    g_object_move_mode_enabled = 0;
    object_move_finish(e);
  }
  g_object_move_prev_accept = accept;
  g_object_move_prev_cancel = cancel;
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Enter the interactive move mode for the last-spawned object. Worker-direct (only flips
// transient flags + raises a menu-hide request); the game-thread driver does the entity work.
// Refused unless the frame hook is live (the driver's natives fault off the game thread) and
// there is a live object to edit. Mutually exclusive with noclip.
static uint32_t enter_object_move_mode(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_MOVE_LAST_OBJECT;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!(g_n.set_entity_coords_no_offset || g_n.set_entity_coords) || !g_n.set_entity_heading ||
      !g_n.get_entity_coords || !g_n.freeze_entity_position) {
    return unavailable(action, "missing move natives");
  }
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  if (object_move_live_count() <= 0) {
    return unavailable(action, "no object to move (spawn one first)");
  }
  g_noclip_enabled = 0;            // mutual exclusion: don't fly the player while placing
  g_object_move_entity = 0;        // force the driver to re-acquire + re-latch the pose
  g_object_move_mode_enabled = 1;
  g_object_move_vis_request = 1;   // worker hides the menu
  // Start in free-move on the last-spawned prop with a clean cursor + edge latches.
  g_object_move_aim_mode = 0;
  g_object_move_surface_snap = 0;
  g_object_move_ground_lock = 0;
  g_object_move_cancel_deletes = 0;  // manual edit of an existing prop: Circle reverts, never deletes
  g_object_move_sel_index = -1;
  g_object_move_aim_dist = 3.0f;
  object_move_reset_latches();
  g_object_move_consume_entry = 1;  // ignore a Cross/Circle still held from the entry press
  return ok(action, "move: LStick move, RStick cam, L1/R1 spin, L2/R2 height, Cross place");
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

// Saved display toggle: auto-enter the move mode after every object spawn. Pure flag flip (no
// native), so it is safe to run at init via the profile-restore loop.
static uint32_t toggle_auto_edit_object(void) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_AUTO_EDIT_OBJECT;
  g_auto_edit_object_enabled = !g_auto_edit_object_enabled;
  return ok(action,
            g_auto_edit_object_enabled ? "auto-edit on spawn: ON" : "auto-edit on spawn: OFF");
}
