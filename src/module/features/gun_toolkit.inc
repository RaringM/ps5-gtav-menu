// Gun toolkit (Gravity / Teleport / Kaboom guns).
//
// Source fragment of features.cpp, textually included AFTER noclip.inc / fly.inc / object_move.inc
// inside its anonymous namespace -- reuses noclip's libm-free trig + control reads, object_move's
// camera-origin helper (object_move_cam_coord) and the synchronous shape-test (raycast) idiom with
// the 24-byte scrVector out-param, fly_hook_ready(), and the Vec3 / g_n / invoke helpers. Not a
// standalone TU.
//
// One "active gun" at a time (a single volatile mode, NOT three saved toggles -- mutually exclusive
// is the natural UX and it conserves the scarce toggle mask). The per-frame driver runs on the GAME
// thread from gtav_features_game_thread_tick(): it raycasts the camera aim each frame and, on the
// fire button, applies the selected effect at the hit point. The raycast + ADD_EXPLOSION natives
// fault on the scePad worker thread, so the driver is game-thread-only (and the cycler refuses until
// the frame hook is live, like fly / autopilot / noclip). The mode enum/labels and the cycler live
// OUTSIDE the frame-hook gate so gtav_features_value_label + the cycler compile in a no-hook build.

enum {
  GTAV_GUN_NONE = 0,
  GTAV_GUN_GRAVITY = 1,
  GTAV_GUN_TELEPORT = 2,
  GTAV_GUN_KABOOM = 3,
  GTAV_GUN_FREEZE = 4,
  GTAV_GUN_CLONE = 5,
  GTAV_GUN_LAUNCH = 6,
  GTAV_GUN_RAGDOLL = 7,
};

static const struct GunModeRow {
  char label[10];  // shown in the cycler "< ... >" and as the action toast
} kGunModes[] = {{"Off"},    {"Gravity"}, {"Teleport"}, {"Kaboom"},
                 {"Freeze"}, {"Clone"},   {"Launch"},   {"Ragdoll"}};

// Diagnostic counters (mirror g_wfx_* in weapon_fx.inc). The gun toolkit fails *silently* --
// every guard in the tick is a bare return -- so these are surfaced on the Debug overlay to
// bisect the chain on hardware. Declared OUTSIDE the frame-hook gate so gtav_features_debug_stats
// links in a no-hook build (they stay 0 when the tick can never run). Written ONLY from the
// game-thread tick; read on the render path. Never gtav_logf from that thread (worker-only ring).
//   gun_tick_fired     once per rendered frame the tick runs with a gun active.
//   gun_fire_detected  when Fire (R2) is recognized and we proceed to raycast.
//   gun_raycast_hit    when the shape test returned a hit.
//   gun_effect_applied when an effect native was actually invoked.
static volatile uint32_t g_gun_tick_fired;
static volatile uint32_t g_gun_fire_detected;
static volatile uint32_t g_gun_raycast_hit;
static volatile uint32_t g_gun_effect_applied;
static volatile float g_gun_last_x;
static volatile float g_gun_last_y;
static volatile float g_gun_last_z;

#if GTAV_MENU_ENABLE_FRAME_HOOK
static const float GTAV_GUN_PROBE_LEN = 1000.0f;       // aim ray length (metres)
static const float GTAV_GUN_TELEPORT_ZOFF = 1.0f;      // land on top of the hit surface, not inside
static const int GTAV_GUN_INPUT_FIRE = 24;             // INPUT_ATTACK (R2)
// Gravity gun as telekinesis: the grabbed entity floats GTAV_GUN_GRAVITY_HOLD_DIST metres in front
// of the aim and is pulled toward that point each frame (proportional gain), with the speed clamped
// so it follows the aim instead of slamming into the player. Releasing R2 flings it forward.
// Tuned gentler after test feedback that the grabbed entity "floats toward the player and speeds
// up": a larger hold distance keeps it further out (not in the player's face), a lower gain eases
// the approach, and a lower speed clamp stops it rushing/slamming in when grabbed from range. Feel
// values -- confirm on hardware.
static const float GTAV_GUN_GRAVITY_HOLD_DIST = 11.0f;  // float distance in front of the camera (m)
static const float GTAV_GUN_GRAVITY_GAIN = 3.0f;        // proportional velocity gain (1/s)
static const float GTAV_GUN_GRAVITY_MAX_MPS = 14.0f;    // clamp so it tracks gently, never rockets in
static const float GTAV_GUN_GRAVITY_LAUNCH = 40.0f;    // fling speed on release (m/s)
static const float GTAV_GUN_LAUNCH_IMPULSE = 60.0f;    // Launch-gun forward impulse magnitude
static const float GTAV_GUN_IMPACT_EPS2 = 0.0625f;     // (0.25 m)^2: a fresh impact moved this far

static uint32_t g_gun_last_frame = 0;
static int g_gun_fire_prev = 0;        // edge latch (no *_JUST_PRESSED native resolved)
static int g_gravity_held_ent = 0;     // entity currently suspended by the gravity gun (0 = none)
static int g_gun_have_impact = 0;      // a weapon impact has been recorded (dedup baseline valid)
static Vec3 g_gun_last_impact;         // last impact point we acted on (fresh-shot edge detection)

// Per-frame driver, run on the game/script thread from gtav_features_game_thread_tick(). Raycasts
// the camera aim and applies the active gun's effect when Fire is held (gravity) or pressed
// (teleport/kaboom, rising edge).
static void gun_toolkit_game_thread_tick(void) {
  const int gun = g_active_gun;
  if (gun == GTAV_GUN_NONE) {
    g_gun_fire_prev = 0;
    g_gravity_held_ent = 0;
    g_gun_have_impact = 0;
    return;
  }
  if (!g_n.get_gameplay_cam_coord || !g_n.start_shape_test_probe || !g_n.get_shape_test_result ||
      !g_n.player_ped_id) {
    return;
  }

  // Collapse the many hook fires within one rendered frame to a single action.
  if (g_n.get_frame_count) {
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == g_gun_last_frame) return;
    g_gun_last_frame = frame;
  }
  // A fresh rendered frame's tick is running with a gun active -- diagnostic liveness signal.
  // We intentionally DO NOT gate on g_menu_open: pad_input.c always restores L2/R2 to the game
  // (menu nav is D-pad/Cross/Circle), so the player aims and fires R2 with the menu panel up,
  // and the gun must respond either way. The edge latch self-corrects each frame from fire_down.
  g_gun_tick_fired++;

  const int ped = native_player_ped_id();
  if (!ped) return;
  const int self_ent = current_player_entity();  // ignore the player (or their vehicle) in the ray
  const int ignore = self_ent ? self_ent : ped;

  // Camera forward basis (libm-free trig), used by both the gravity-hold follow and the raycast.
  const Vec3 cam = object_move_cam_coord();
  const Vec3 rot = noclip_cam_rot();
  const float yaw = rot.z * GTAV_NOCLIP_DEG2RAD;
  const float pitch = rot.x * GTAV_NOCLIP_DEG2RAD;
  const float cp = noclip_cosf(pitch);
  const float fx = -noclip_sinf(yaw) * cp;
  const float fy = noclip_cosf(yaw) * cp;
  const float fz = noclip_sinf(pitch);

  const int fire_down = noclip_ctrl(GTAV_GUN_INPUT_FIRE);
  g_gun_fire_prev = fire_down;

  // --- Gravity telekinesis HOLD: runs every frame, independent of new impacts. While R2 is held
  // the grabbed entity is suspended floating in front of the aim and pulled toward that point with
  // a clamped velocity (so it follows the camera instead of slamming into the player). Releasing R2
  // flings it forward. The latch itself happens on the shot edge below.
  if (gun == GTAV_GUN_GRAVITY && g_gravity_held_ent) {
    if (!g_n.does_entity_exist || !invoke_return<int>(g_n.does_entity_exist, g_gravity_held_ent) ||
        !g_n.get_entity_coords || !g_n.set_entity_velocity) {
      g_gravity_held_ent = 0;
    } else if (fire_down) {
      const float tx = cam.x + fx * GTAV_GUN_GRAVITY_HOLD_DIST;
      const float ty = cam.y + fy * GTAV_GUN_GRAVITY_HOLD_DIST;
      const float tz = cam.z + fz * GTAV_GUN_GRAVITY_HOLD_DIST + 0.5f;
      const Vec3 oc = invoke_vector(g_n.get_entity_coords, g_gravity_held_ent, 1);
      float vx = (tx - oc.x) * GTAV_GUN_GRAVITY_GAIN;
      float vy = (ty - oc.y) * GTAV_GUN_GRAVITY_GAIN;
      float vz = (tz - oc.z) * GTAV_GUN_GRAVITY_GAIN;
      const float s2 = vx * vx + vy * vy + vz * vz;
      if (s2 > GTAV_GUN_GRAVITY_MAX_MPS * GTAV_GUN_GRAVITY_MAX_MPS) {
        const float inv = GTAV_GUN_GRAVITY_MAX_MPS / sqrtf(s2);
        vx *= inv;
        vy *= inv;
        vz *= inv;
      }
      invoke_void(g_n.set_entity_velocity, g_gravity_held_ent, vx, vy, vz);
      g_gun_effect_applied++;
    } else {
      // Released -> fling the held entity along the camera forward (the "fun" launch).
      invoke_void(g_n.set_entity_velocity, g_gravity_held_ent, fx * GTAV_GUN_GRAVITY_LAUNCH,
                  fy * GTAV_GUN_GRAVITY_LAUNCH, fz * GTAV_GUN_GRAVITY_LAUNCH + 2.0f);
      g_gravity_held_ent = 0;
    }
  }

  // --- Trigger: wait for the ACTUAL shot, not the trigger pull. Act on a fresh weapon impact (the
  // same GET_PED_LAST_WEAPON_IMPACT_COORD edge the combat-effects use), so the gun fires when the
  // bullet lands instead of the instant R2 goes down. >0.25 m of movement = a new impact.
  Vec3 impact = {0.0f, 0.0f, 0.0f};
  int impact_edge = 0;
  if (g_n.get_ped_last_weapon_impact_coord && last_weapon_impact(ped, &impact)) {
    const float dx = impact.x - g_gun_last_impact.x;
    const float dy = impact.y - g_gun_last_impact.y;
    const float dz = impact.z - g_gun_last_impact.z;
    if (!g_gun_have_impact || (dx * dx + dy * dy + dz * dz) > GTAV_GUN_IMPACT_EPS2) impact_edge = 1;
    g_gun_last_impact = impact;
    g_gun_have_impact = 1;
  }
  if (!impact_edge) return;
  g_gun_fire_detected++;  // a real shot landed; proceeding to raycast for the aimed entity/point

  // Raycast along the camera forward to resolve the entity/surface the shot was aimed at (same basis
  // + 24-byte scrVector out-param shape as the object spawner's surface snap). flags:
  // vehicles(2)+peds(4)+objects(16), plus world(1) for the teleport/kaboom guns (which can target
  // terrain); gravity/launch/freeze/ragdoll target entities only. options 7.
  const float ex = cam.x + fx * GTAV_GUN_PROBE_LEN;
  const float ey = cam.y + fy * GTAV_GUN_PROBE_LEN;
  const float ez = cam.z + fz * GTAV_GUN_PROBE_LEN;
  // Clone/Ragdoll target peds only (those natives need a ped) -> ped bit 4; gravity/launch/freeze
  // target entities (no world); the rest (teleport/kaboom) also hit terrain.
  const int flags = (gun == GTAV_GUN_CLONE || gun == GTAV_GUN_RAGDOLL) ? 4
                    : (gun == GTAV_GUN_GRAVITY || gun == GTAV_GUN_LAUNCH ||
                       gun == GTAV_GUN_FREEZE)
                        ? (2 | 4 | 16)
                        : (1 | 2 | 4 | 16);
  const int h =
      invoke_return<int>(g_n.start_shape_test_probe, cam.x, cam.y, cam.z, ex, ey, ez, flags, ignore,
                         7);
  int hit_flag = 0;
  int hit_ent = 0;
  uint64_t end_sv[3] = {0, 0, 0};
  uint64_t norm_sv[3] = {0, 0, 0};
  invoke_return<int>(g_n.get_shape_test_result, h, (uint64_t)(uintptr_t)&hit_flag,
                     (uint64_t)(uintptr_t)end_sv, (uint64_t)(uintptr_t)norm_sv,
                     (uint64_t)(uintptr_t)&hit_ent);
  if (!hit_flag) return;
  const float hx = *reinterpret_cast<const float*>(&end_sv[0]);
  const float hy = *reinterpret_cast<const float*>(&end_sv[1]);
  const float hz = *reinterpret_cast<const float*>(&end_sv[2]);
  g_gun_raycast_hit++;  // shape test returned a hit; record the point we act on (debug bisection)
  g_gun_last_x = hx;
  g_gun_last_y = hy;
  g_gun_last_z = hz;

  if (gun == GTAV_GUN_KABOOM) {
    // type 4 = ROCKET, owned by the ped so it raises no wanted level (same args as explosive ammo).
    if (g_n.add_owned_explosion) {
      invoke_void(g_n.add_owned_explosion, ped, hx, hy, hz, 4, 1.0f, 1, 0, 0.5f);
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_TELEPORT) {
    // Teleport the player (the vehicle if driving, else the ped) onto the aimed surface. The target
    // is always on-screen -> the area is streamed in, so a direct SET_ENTITY_COORDS is safe here.
    const uint64_t set_coords =
        g_n.set_entity_coords_no_offset ? g_n.set_entity_coords_no_offset : g_n.set_entity_coords;
    const int mv = self_ent ? self_ent : ped;
    if (set_coords) {
      if (g_n.set_entity_coords_no_offset) {
        invoke_void(set_coords, mv, hx, hy, hz + GTAV_GUN_TELEPORT_ZOFF, 0, 0, 1);
      } else {
        invoke_void(set_coords, mv, hx, hy, hz + GTAV_GUN_TELEPORT_ZOFF, 0, 0, 0, 0);
      }
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_GRAVITY) {
    // LATCH the hit entity for the telekinesis hold above (it floats while R2 stays held and is
    // flung on release). World-only hits (no entity) are ignored. does_entity_exist guards a
    // stale/invalid handle from the probe.
    if (hit_ent && g_n.does_entity_exist && invoke_return<int>(g_n.does_entity_exist, hit_ent)) {
      g_gravity_held_ent = hit_ent;
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_LAUNCH) {
    // Launch gun: yeet the hit entity up + along the camera forward with one impulse (same
    // APPLY_FORCE_TO_ENTITY pattern as vehicle Rocket Boost). forceType 1 = impulse.
    if (hit_ent && g_n.apply_force_to_entity && g_n.does_entity_exist &&
        invoke_return<int>(g_n.does_entity_exist, hit_ent)) {
      invoke_void(g_n.apply_force_to_entity, hit_ent, 1, fx * GTAV_GUN_LAUNCH_IMPULSE,
                  fy * GTAV_GUN_LAUNCH_IMPULSE, GTAV_GUN_LAUNCH_IMPULSE, 0.0f, 0.0f, 0.0f, 0, 0, 1,
                  1, 0, 1);
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_RAGDOLL) {
    // Ragdoll gun: knock the hit ped into a ragdoll (the probe targeted peds only). SET_PED_TO_RAGDOLL
    // mutates the ped task tree, safe here on the game/script thread.
    if (hit_ent && g_n.set_ped_to_ragdoll && g_n.does_entity_exist &&
        invoke_return<int>(g_n.does_entity_exist, hit_ent)) {
      // SET_PED_TO_RAGDOLL(ped, time1, time2, ragdollType, p4, p5, p6).
      invoke_return<int>(g_n.set_ped_to_ragdoll, hit_ent, 2000, 3000, 0, 1, 1, 0);
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_FREEZE) {
    // Freeze gun: lock the hit entity in place (FREEZE_ENTITY_POSITION, modify-existing). World-only
    // hits (no entity) are ignored. does_entity_exist guards a stale/invalid handle from the probe.
    if (hit_ent && g_n.freeze_entity_position && g_n.does_entity_exist &&
        invoke_return<int>(g_n.does_entity_exist, hit_ent)) {
      invoke_void(g_n.freeze_entity_position, hit_ent, 1);
      g_gun_effect_applied++;
    }
  } else if (gun == GTAV_GUN_CLONE) {
    // Clone gun: duplicate the aimed ped (the probe targeted peds only). CLONE_PED allocates a ped,
    // but this tick runs on the game/script thread. Track the clone so Clear Spawned Peds removes it.
    if (hit_ent && g_n.clone_ped && g_n.does_entity_exist &&
        invoke_return<int>(g_n.does_entity_exist, hit_ent)) {
      // CLONE_PED(ped, isNetwork, bScriptHostPed, copyHeadBlend) -> Ped.
      const int clone = invoke_return<int>(g_n.clone_ped, hit_ent, 0, 1, 1);
      g_gun_effect_applied++;  // native invoked (count regardless of the returned handle)
      if (clone) track_spawned_ped(clone);
    }
  }
}
#endif  // GTAV_MENU_ENABLE_FRAME_HOOK

// Worker-side cycler. Advances the active gun; the game-thread driver reads the live value. A
// non-Off gun is refused until the frame hook is live (the raycast/explosion natives run on that
// tick), mirroring cycle_fly_mode() / cycle_autopilot_mode().
static uint32_t cycle_active_gun(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_CYCLE_ACTIVE_GUN;
  const uint32_t count = (uint32_t)(sizeof(kGunModes) / sizeof(kGunModes[0]));
  if (param == 1u || param == 2u) {
    g_active_gun = (int)list_step((uint32_t)g_active_gun, count, param == 2u ? -1 : 1);
  }
  const uint32_t mode = (uint32_t)g_active_gun % count;
  if (mode != (uint32_t)GTAV_GUN_NONE && !fly_hook_ready()) {
    return unavailable(action, "needs main-thread hook");
  }
  // Decouple from Combat Effects: selecting an active gun owns the shot, so clear the explosive /
  // fire / explosive-melee ammo flags (they also fire on the weapon-impact edge and would otherwise
  // stack with the gun on the same shot). g_gun_have_impact is reset so the first shot after the
  // switch isn't deduped against a stale impact.
  if (mode != (uint32_t)GTAV_GUN_NONE) {
    g_explosive_ammo_enabled = 0;
    g_fire_ammo_enabled = 0;
    g_explosive_melee_enabled = 0;
  }
#if GTAV_MENU_ENABLE_FRAME_HOOK
  g_gun_have_impact = 0;
#endif
  return ok(action, kGunModes[mode].label);
}
