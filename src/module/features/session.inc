// session.inc -- session / story-flow one-shots.
//
// "Skip Prologue" (GTAV_MENU_ENABLE_PROLOGUE_SKIP, default off). GTA V's story
// flow-controller decides whether to launch the North Yankton prologue by reading a
// SAVED SCRIPT GLOBAL -- the per-mission completion array Global.f_9092.f_330[53]
// (flow mission 53 = "prologue1"): `if (f_330[53]) { node done; return; } else
// START_NEW_SCRIPT("prologue1")`. So setting f_330[53]=1 makes the flow skip it.
// (Verified against the decompiled GTA V flow scripts.)
//
// We do this with a plain in-process script-global write (the despawn-global path),
// which touches NO profile setting -> no failing cloud sync -> cannot trigger the
// 0xa0024301 suspend-timeout crash that the old SET_PROFILE_SETTING_PROLOGUE_COMPLETE
// (profile 903) path caused. Profile 903 was a one-way downstream mirror the game never
// reads to gate, which is why it was both ineffective and crash-prone; that path is gone.
//
// The menu injects at stable player control (= INSIDE the running prologue1), and the
// flow's launch decision happens earlier during the load, so a one-shot write is too late
// and in-session termination/warp both trip the mission's "Mission Failed" + restart
// (hardware-confirmed). Instead, skip_prologue() ARMS a flag that the WORKER tick re-stamps
// every tick (gtav_features_worker_tick; the worker runs during a load, the game-thread hook
// does not). On a story reload the save re-zeros the globals, so re-stamping races -- and
// should win -- the flow-controller's prologue-launch read, so prologue1 is never launched (a
// clean, no-fail skip). Flow: press Skip Prologue -> exit to main menu -> reload Story.
//
// The exact gate f_330[53] is one of a DIFF-DERIVED candidate set (GTAV_PROLOGUE_GLOBAL_-
// CANDIDATES: the 5 flow-completion flags the prologue flips 0->1 on completion, pinned by a
// live before/after global dump; see anchors.json). The armed worker stamps ALL of them,
// replicating the post-prologue flag state -- so we don't need to know which single index is
// THE gate, and writing the full set is the semantically-correct "prologue done".
//
// CAVEATS: the ported PC index 122655 was DISPROVEN live (written, prologue still launched) --
// hence the diff-derived set. Needs GTAV_MENU_ENABLE_SCRIPT_GLOBALS + the GTAV_GLOBALS_BASE_-
// ADDR anchor (same as the despawn fix); a wrong index is a benign no-op (gtav_global_ptr
// null-guards). Winning the load race may take a reload or two; if it never wins even with the
// right index, the durable fallback is a post-prologue savegame.

static uint32_t skip_prologue() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SKIP_PROLOGUE;
  // ARM the continuous write. We can't skip the prologue mid-mission (terminating it trips
  // the mission's "Mission Failed" + restart), and a one-shot flag write loses to the
  // corrupt save re-zeroing it on load. So instead: arm a flag the WORKER tick re-stamps
  // every tick (it runs during a story load, where the game-thread hook does not). The user
  // then exits to the main menu and reloads Story; the re-stamp wins the flow-controller's
  // prologue-launch read, so prologue1 is never launched -- a clean, no-fail skip.
#if defined(GTAV_MENU_ENABLE_SCRIPT_GLOBALS) && GTAV_MENU_ENABLE_SCRIPT_GLOBALS
  if ((uint32_t)GTAV_PROLOGUE_GLOBAL_INDEX != 0u && (uintptr_t)GTAV_GLOBALS_BASE_ADDR != 0) {
    g_prologue_skip_armed = 1;
    return ok(action, "prologue skip ARMED -- now exit to main menu and reload Story to apply");
  }
#endif
  return unavailable(action, "skip prologue not built (needs SCRIPT_GLOBALS + GTAV_PROLOGUE_GLOBAL_INDEX)");
}
