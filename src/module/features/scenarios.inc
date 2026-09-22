// Self > Scenarios: put the player into a named ped scenario (smoking, leaning, guard
// stand, ...) and cancel it.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace --
// these stay file-local statics sharing g_n / g_state and the helpers defined in
// features.cpp. Not a standalone translation unit.
//
// Both run on the game/script thread (via run_gated_action): TASK_START_SCENARIO_IN_PLACE and
// CLEAR_PED_TASKS mutate the player's task tree, neither safe on the worker. A scenario is
// identified by its name string; it travels here as the gtav_scenario_catalog index (the menu
// row's .param, since the game-thread job queue carries only uint32s), and we pass
// catalog[idx].name straight to the native -- no hashing, the native takes the name as a string.

// Enter the catalog scenario at `idx` on the player. Clears any current task first so switching
// scenarios starts cleanly. Returns the toast result; reports failure (rather than crashing) when
// the native is missing, the index is out of range, or there is no valid player ped.
static uint32_t start_scenario(uint32_t idx) {
  if (!g_n.task_start_scenario_in_place) {
    return unavailable(GTAV_NATIVE_SHELL_ACTION_START_SCENARIO, "missing scenario native");
  }
  if (idx >= gtav_scenario_catalog_count) {
    return failed(GTAV_NATIVE_SHELL_ACTION_START_SCENARIO, "scenario index out of range");
  }
  int ped = valid_player_ped();
  if (!ped) {
    return failed(GTAV_NATIVE_SHELL_ACTION_START_SCENARIO,
                  "no valid player ped (be in active gameplay)");
  }
  const char* name = gtav_scenario_catalog[idx].name;
  if (g_n.clear_ped_tasks_immediately) invoke_void(g_n.clear_ped_tasks_immediately, ped);
  // TASK_START_SCENARIO_IN_PLACE(ped, scenarioName, unkDelay=0, playEnterAnim=1).
  invoke_void(g_n.task_start_scenario_in_place, ped, name, 0, 1);
  return ok(GTAV_NATIVE_SHELL_ACTION_START_SCENARIO, gtav_scenario_catalog[idx].label);
}

// Cancel the active scenario. CLEAR_PED_TASKS lets the ped play its scenario-exit animation;
// fall back to CLEAR_PED_TASKS_IMMEDIATELY (snap out) if only that native resolved.
static uint32_t stop_scenario() {
  int ped = valid_player_ped();
  if (!ped) {
    return failed(GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO,
                  "no valid player ped (be in active gameplay)");
  }
  if (g_n.clear_ped_tasks) {
    invoke_void(g_n.clear_ped_tasks, ped);
  } else if (g_n.clear_ped_tasks_immediately) {
    invoke_void(g_n.clear_ped_tasks_immediately, ped);
  } else {
    return unavailable(GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO, "missing clear-tasks native");
  }
  return ok(GTAV_NATIVE_SHELL_ACTION_STOP_SCENARIO, "scenario cancelled");
}
