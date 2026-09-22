// Self > Emotes: play a curated animation on the player ped, and cancel it.
//
// Source fragment of features.cpp, textually included inside its anonymous namespace.
//
// REQUEST_ANIM_DICT streams the dict (NOT worker-safe, like REQUEST_MODEL) and TASK_PLAY_ANIM
// mutates the ped task tree, so all the native work runs on the game-thread emote tick. PLAY/STOP
// are worker-direct flag flips (like the minigame toggles): PLAY_EMOTE records the catalog index
// and refuses until the frame hook is live; the tick streams the dict then plays the clip.

// Loop-vs-once flag for the emote player. Declared UNCONDITIONALLY (outside the frame-hook gate)
// because the feature_toggles.def consumers in features.cpp (toggle-mask / init reset / import
// restore) reference it in every build. Default off = play once (so one-shot clips like "Throw"
// don't loop unnaturally); the Loop Emote toggle flips it to AF_LOOP.
static int g_emote_loop_enabled = 0;

#if GTAV_MENU_ENABLE_FRAME_HOOK
// Frames to wait for a dict to stream before giving up (a bad dict name never loads). The tick
// fires per rendered frame, so this is a generous ~3s ceiling.
#define GTAV_EMOTE_WAIT_TICKS 180

static int g_emote_pending = -1;       // catalog index being streamed/played, or -1 = none
static int g_emote_stop = 0;           // set by STOP_EMOTE; the tick clears the player's tasks
static int g_emote_wait = 0;           // streaming-wait tick counter (timeout)
static int g_emote_clear_pending = 0;  // a new selection arrived: clear the old clip once first

// Game-thread tick: drive the request->wait->play state machine and handle stop. Cheap no-op when
// nothing is pending. Runs in valid script context (frame hook), so it uses the plain invoke_*.
static void emote_game_thread_tick(void) {
  // Once per rendered frame, not once per hook fire. While a clip streams this issues
  // REQUEST_ANIM_DICT + HAS_ANIM_DICT_LOADED, and the hook fires ~137x per frame -- the same
  // streaming flood that wedged the model path. The streamer advances per frame, so retrying
  // faster cannot help, and 60/s is far more responsive than any emote needs.
  if (g_n.get_frame_count) {
    static uint32_t s_last_frame;
    const uint32_t frame = invoke_return<uint32_t>(g_n.get_frame_count);
    if (frame == s_last_frame) return;
    s_last_frame = frame;
  }
  if (g_emote_stop) {
    g_emote_stop = 0;
    g_emote_pending = -1;
    int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
    if (ped && g_n.clear_ped_tasks) invoke_void(g_n.clear_ped_tasks, ped);
    return;
  }
  if (g_emote_pending < 0) return;
  // A freshly-selected emote stops whatever is currently playing BEFORE streaming the new clip, so
  // a new selection always cancels the old one -- even if the new dict never streams (a bad name).
  // Without this, a still-looping prior emote would keep playing on a failed stream. One
  // CLEAR_PED_TASKS; the request->wait->play below then restarts the ped with the new clip.
  if (g_emote_clear_pending) {
    g_emote_clear_pending = 0;
    int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
    if (ped && g_n.clear_ped_tasks) invoke_void(g_n.clear_ped_tasks, ped);
  }
  if ((uint32_t)g_emote_pending >= gtav_anim_catalog_count || !g_n.request_anim_dict ||
      !g_n.has_anim_dict_loaded || !g_n.task_play_anim) {
    g_emote_pending = -1;
    return;
  }
  const GtavAnimEntry* e = &gtav_anim_catalog[g_emote_pending];
  invoke_void(g_n.request_anim_dict, e->dict);
  if (invoke_return<int>(g_n.has_anim_dict_loaded, e->dict)) {
    int ped = g_n.player_ped_id ? native_player_ped_id() : 0;
    if (ped) {
      // TASK_PLAY_ANIM(ped, dict, clip, blendIn 8, blendOut -8, duration -1, flag, playbackRate 0,
      // lockX/Y/Z 0). flag = AF_LOOP (1) when Loop Emote is on, else 0 (play once -> the ped blends
      // back to idle when the clip ends). Blend/flag values may need hardware tuning.
      const int anim_flag = g_emote_loop_enabled ? 1 : 0;
      invoke_void(g_n.task_play_anim, ped, e->dict, e->clip, 8.0f, -8.0f, -1, anim_flag, 0.0f, 0, 0,
                  0);
    }
    g_emote_pending = -1;
    g_emote_wait = 0;
  } else if (++g_emote_wait > GTAV_EMOTE_WAIT_TICKS) {
    g_emote_pending = -1;  // dict never loaded (wrong name) -- give up rather than spin forever
    g_emote_wait = 0;
  }
}
#endif  // emote tick (frame-hook only)

// Worker-direct: record the emote to play. The game-thread tick does the streaming + play, so this
// refuses until the frame hook is live (like the minigame toggles).
static uint32_t play_emote(uint32_t idx) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PLAY_EMOTE;
  if (idx >= gtav_anim_catalog_count) return failed(action, "emote index out of range");
#if GTAV_MENU_ENABLE_FRAME_HOOK
  if (!g_n.task_play_anim || !g_n.request_anim_dict) {
    return unavailable(action, "missing emote natives");
  }
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs main-thread hook");
  g_emote_pending = (int)idx;
  g_emote_stop = 0;
  g_emote_wait = 0;
  g_emote_clear_pending = 1;  // cancel any in-flight emote before the new one streams
  return ok(action, gtav_anim_catalog[idx].label);
#else
  return unavailable(action, "needs main-thread hook");
#endif
}

static uint32_t stop_emote() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_STOP_EMOTE;
#if GTAV_MENU_ENABLE_FRAME_HOOK
  g_emote_pending = -1;
  g_emote_stop = 1;  // the tick runs CLEAR_PED_TASKS on the game thread
#endif
  return ok(action, "emote stopped");
}

// Loop-vs-once toggle. Pure worker-side flag flip (the game-thread tick reads g_emote_loop_enabled
// for the TASK_PLAY_ANIM flag), so it works in every build and needs no frame hook.
static uint32_t toggle_emote_loop() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_EMOTE_LOOP;
  g_emote_loop_enabled = !g_emote_loop_enabled;
  return ok(action, g_emote_loop_enabled ? "emotes loop until Stop" : "emotes play once");
}
