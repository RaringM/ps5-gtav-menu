// Ped control: retask the peds near the player (everyone attacks / flees / stop).
//
// Source fragment of features.cpp, textually included inside its anonymous
// namespace -- these stay file-local statics sharing g_n / g_state and the
// helpers defined in features.cpp. Not a standalone translation unit.
//
// All three run on the game/script thread (via run_gated_action): GET_PED_NEARBY_PEDS walks
// the script ped pool and the task natives mutate the ped task tree, neither safe on the
// worker. GET_PED_NEARBY_PEDS uses the classic sizeAndPeds buffer: element 0 is the requested
// size in / count out, elements 1..N receive ped handles. The first run logs the returned
// count + first handles so the buffer ABI can be confirmed on hardware.

enum {
  GTAV_PED_CONTROL_ATTACK = 0,
  GTAV_PED_CONTROL_FLEE = 1,
  GTAV_PED_CONTROL_STOP = 2,
  GTAV_PED_CONTROL_RAGDOLL = 3,  // used by the Ragdoll minigame; no one-shot menu action
};

// Enumerate the peds near the player and retask them. Side-effect-free with respect to the
// result/toast state (it never calls ok()/unavailable()/failed()), so the per-frame minigame
// driver can call it every logical frame without clobbering last_action/last_result. Returns
// the number of peds touched, or -1 if the required natives / a valid player ped are missing.
// Must run on the game/script thread (pool walk + ped-task mutation).
static int retask_peds_core(int mode) {
  if (!g_n.get_ped_nearby_peds || !g_n.player_ped_id) return -1;
  int player = native_player_ped_id();
  if (player <= 0) return -1;

  // sizeAndPeds: [0] = capacity in / count out, [1..96] = handles.
  int buf[97];
  memset(buf, 0, sizeof(buf));
  buf[0] = 96;
  invoke_void(g_n.get_ped_nearby_peds, player, &buf[0], -1);

  static int s_logged_abi = 0;
  if (!s_logged_abi) {
    s_logged_abi = 1;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "ped_nearby count=%d h1=%d h2=%d h3=%d",
                       buf[0], buf[1], buf[2], buf[3]);
  }

  int touched = 0;
  for (int i = 1; i < 97; ++i) {
    int h = buf[i];
    if (h <= 0) continue;
    if (g_n.is_ped_a_player && invoke_return<int>(g_n.is_ped_a_player, h)) continue;
    // Never retask the player's own bodyguards. "Everyone Attacks Me" used to TASK_COMBAT_PED
    // them against the player; being in the player's group they cannot comply and just flee or
    // mill about. Skipping the roster keeps them defending instead of being turned/scattered.
    if (handle_is_bodyguard(h)) continue;
    if (mode == GTAV_PED_CONTROL_ATTACK) {
      if (g_n.set_ped_keep_task) invoke_void(g_n.set_ped_keep_task, h, 1);
      if (g_n.set_blocking_of_non_temporary_events)
        invoke_void(g_n.set_blocking_of_non_temporary_events, h, 1);
      if (g_n.task_combat_ped) invoke_void(g_n.task_combat_ped, h, player, 0, 16);
    } else if (mode == GTAV_PED_CONTROL_FLEE) {
      if (g_n.set_ped_flee_attributes) invoke_void(g_n.set_ped_flee_attributes, h, 0, 1);
      if (g_n.task_smart_flee_ped)
        invoke_void(g_n.task_smart_flee_ped, h, player, 100.0f, -1, 0, 0);
    } else if (mode == GTAV_PED_CONTROL_RAGDOLL) {
      // SET_PED_TO_RAGDOLL(ped, time1, time2, type, p4, p5, p6): same args as force_ragdoll().
      if (g_n.set_ped_to_ragdoll) invoke_return<int>(g_n.set_ped_to_ragdoll, h, 2000, 3000, 0, 1, 1, 0);
    } else {  // STOP
      if (g_n.set_blocking_of_non_temporary_events)
        invoke_void(g_n.set_blocking_of_non_temporary_events, h, 0);
      if (g_n.clear_ped_tasks_immediately) invoke_void(g_n.clear_ped_tasks_immediately, h);
    }
    ++touched;
  }
  return touched;
}

static uint32_t retask_nearby_peds(uint32_t action, int mode) {
  if (!g_n.get_ped_nearby_peds || !g_n.player_ped_id) {
    return unavailable(action, "missing ped-control natives");
  }
  // Guard the mode's required task native so an unresolved one reports unavailable instead of
  // a false "done" toast (retask_peds_core inner-guards each call, so a null native would
  // otherwise no-op while still counting peds as touched).
  if (mode == GTAV_PED_CONTROL_RAGDOLL && !g_n.set_ped_to_ragdoll) {
    return unavailable(action, "missing ragdoll native");
  }
  int touched = retask_peds_core(mode);
  if (touched < 0) return failed(action, "no valid player ped (be in active gameplay)");
  if (!touched) return ok(action, "no nearby peds");
  if (mode == GTAV_PED_CONTROL_ATTACK) return ok(action, "peds now hostile");
  if (mode == GTAV_PED_CONTROL_FLEE) return ok(action, "peds fleeing");
  if (mode == GTAV_PED_CONTROL_RAGDOLL) return ok(action, "peds ragdolled");
  return ok(action, "peds calmed");
}

static uint32_t peds_attack_player() {
  return retask_nearby_peds(GTAV_NATIVE_SHELL_ACTION_PEDS_ATTACK_PLAYER, GTAV_PED_CONTROL_ATTACK);
}
static uint32_t peds_flee_player() {
  return retask_nearby_peds(GTAV_NATIVE_SHELL_ACTION_PEDS_FLEE_PLAYER, GTAV_PED_CONTROL_FLEE);
}
static uint32_t peds_stop() {
  return retask_nearby_peds(GTAV_NATIVE_SHELL_ACTION_PEDS_STOP, GTAV_PED_CONTROL_STOP);
}
// One-shot "bar brawl": ragdoll every nearby ped (skips the player). Same game-thread lane as
// the other ped-control actions; reuses the GTAV_PED_CONTROL_RAGDOLL retask mode.
static uint32_t ragdoll_nearby_peds() {
  return retask_nearby_peds(GTAV_NATIVE_SHELL_ACTION_RAGDOLL_NEARBY, GTAV_PED_CONTROL_RAGDOLL);
}
