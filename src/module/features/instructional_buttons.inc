// Native instructional-button bar: GTA's own "instructional_buttons" Scaleform
//
// Source fragment of features.cpp, textually included inside its anonymous namespace -- these stay
// file-local statics sharing g_n / the invoke helpers. Not a standalone TU.
//
// This is the exact widget the pause menu draws along the bottom ("Browse L1/R1 / Back / Select"),
// so the menu's bottom-right controls strip shows real DualSense button glyphs in the native format
// instead of our hand-drawn coloured chips. The glyph artwork + X/O swap are resolved by the game
// for whatever controller is active -- no per-platform texture mapping on our side.
//
// Threading: scripts prepare scaleforms from the game/script thread, and the method-building
// natives push onto a per-movie arg stack the engine owns -- neither is safe on the scePad worker.
// REQUEST/load/data-slot setup therefore stays on the game-thread hook. With the phase renderer,
// only the final DRAW_SCALEFORM_MOVIE_FULLSCREEN submission moves to the verified render callback;
// a reader claim prevents it from overlapping a slot rebuild. The worker's only job is to publish
// an immutable button set. The worker render path calls
// gtav_features_instructional_active() to suppress its own fallback bar once this one is drawing, so
// the two never stack. Mirrors the vehicle-preview split (worker publishes, game thread does the
// streaming/native work). Inert -- and the publish/active API a stable no-op -- without the gate.

#define GTAV_INSTR_MAX_BUTTONS 6u

#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES

static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "instructional phase publication requires lock-free qwords");

struct InstrButton {
  int control;
  int control2;
  const char* glyph;  // "~INPUT_*~" token fallback when the control id yields no glyph string
  const char* label;
};
struct InstrButtonSet {
  uint64_t generation;
  uint32_t count;
  uint32_t reserved;
  InstrButton buttons[GTAV_INSTR_MAX_BUTTONS];
};
static InstrButtonSet g_instr_sets[2];
static uint64_t g_instr_token;
static uint64_t g_instr_readers[2];
static InstrButton g_instr_worker[GTAV_INSTR_MAX_BUTTONS];
static uint32_t g_instr_worker_count;
static uint32_t g_instr_publish_pending;
static uint64_t g_instr_generation;
static uint32_t g_instr_drawing;

// scaleform handle (0 = not requested yet) + the generation the slots were last built for.
static int g_instr_handle = 0;
static uint64_t g_instr_built_gen = UINT64_MAX;
#if GTAV_MENU_PHASE_DRAW_LIST
static uint64_t g_instr_phase_handle;
static uint64_t g_instr_phase_readers;
#endif

// Diagnostic state, written by the game-thread tick, read worker-side by
// gtav_features_instructional_debug() so the bar can show WHY the Scaleform is/ isn't drawing
// (handle, loaded, natives-resolved, menu-open, draw-call count). Cheap volatiles; surfaced on the
// instructional bar while debugging Route B on hardware.
static int g_instr_dbg_handle = 0;
static int g_instr_dbg_loaded = 0;
static int g_instr_dbg_ready = 0;
static int g_instr_dbg_open = 0;
static uint32_t g_instr_dbg_draws = 0u;

static uint32_t instr_set_index(uint64_t token) {
  const uint32_t encoded = (uint32_t)(token & 0xffu);
  return encoded >= 1u && encoded <= 2u ? encoded - 1u : UINT32_MAX;
}

static int instr_publish_set(void) {
  const uint64_t token = __atomic_load_n(&g_instr_token, __ATOMIC_ACQUIRE);
  const uint32_t current = instr_set_index(token);
  const uint32_t candidate = current == UINT32_MAX ? 0u : current ^ 1u;
  if (__atomic_load_n(&g_instr_readers[candidate], __ATOMIC_ACQUIRE) != 0) return -1;
  InstrButtonSet* set = &g_instr_sets[candidate];
  set->generation = ++g_instr_generation;
  set->count = g_instr_worker_count;
  for (uint32_t i = 0; i < set->count; ++i) set->buttons[i] = g_instr_worker[i];
  __atomic_store_n(&g_instr_token, (set->generation << 8u) | (uint64_t)(candidate + 1u),
                   __ATOMIC_RELEASE);
  g_instr_publish_pending = 0u;
  return 0;
}

static int instr_snapshot(InstrButton* buttons, uint32_t* count, uint64_t* generation) {
  for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
    const uint64_t token = __atomic_load_n(&g_instr_token, __ATOMIC_ACQUIRE);
    const uint32_t index = instr_set_index(token);
    if (index == UINT32_MAX) return -1;
    __atomic_fetch_add(&g_instr_readers[index], 1u, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_instr_token, __ATOMIC_ACQUIRE) == token) {
      const InstrButtonSet* set = &g_instr_sets[index];
      *count = set->count;
      *generation = set->generation;
      for (uint32_t i = 0; i < set->count; ++i) buttons[i] = set->buttons[i];
      __atomic_fetch_sub(&g_instr_readers[index], 1u, __ATOMIC_RELEASE);
      return 0;
    }
    __atomic_fetch_sub(&g_instr_readers[index], 1u, __ATOMIC_RELEASE);
  }
  return -1;
}

// True once every native the bar needs is resolved (all live in the feature table under the gate).
static int instr_natives_ready(void) {
  return g_n.request_scaleform_movie && g_n.has_scaleform_movie_loaded &&
         g_n.begin_scaleform_movie_method && g_n.end_scaleform_movie_method &&
         g_n.scaleform_movie_method_add_param_int &&
         g_n.scaleform_movie_method_add_param_player_name_string &&
         g_n.begin_text_command_scaleform_string && g_n.end_text_command_scaleform_string &&
         g_n.add_text_component_substring_player_name && g_n.draw_scaleform_movie_fullscreen &&
         g_n.get_control_instructional_buttons_string;
}

// GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING(2, controlId, allowXOSwap) -> the controller's glyph
// markup string (game-owned buffer, reused between calls). Copy it out immediately so a follow-up
// call for a second glyph cannot clobber it. control 2 / allowXOSwap 1 mirror the documented usage.
static void instr_copy_glyph(int control_id, char* dst, size_t cap) {
  dst[0] = '\0';
  if (control_id < 0) return;
  const char* s = invoke_return_ctx<const char*>(0, g_n.get_control_instructional_buttons_string, 2,
                                                  control_id, 1);
  if (!s) return;
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) dst[i] = s[i];
  dst[i] = '\0';
}

// Push one string param to the scaleform via the text-command path (resolves ~INPUT_*~ glyph
// tokens, ~ accents, etc.). Used for the verb label and for the glyph FALLBACK token.
static void instr_push_text_param(const char* s) {
  invoke_void_ctx(0, g_n.begin_text_command_scaleform_string, "STRING");
  invoke_void_ctx(0, g_n.add_text_component_substring_player_name, (const char*)(s ? s : ""));
  invoke_void_ctx(0, g_n.end_text_command_scaleform_string);
}

// Build one SET_DATA_SLOT: slot index, the glyph, then the verb label. Primary glyph source is the
// control id via GET_CONTROL_INSTRUCTIONAL_BUTTONS_STRING (handles X/O swap; pushed as the documented
// PLAYER_NAME control-string param). Some controls (the d-pad directions, R3) return an empty string
// on Enhanced -- then fall back to the `~INPUT_*~` token pushed through the text path, which the
// Scaleform resolves to the same glyph. Either way the label is a text param after it.
static void instr_set_data_slot(int slot, int control, int control2, const char* glyph_token,
                                const char* label) {
  char glyph[96];
  instr_copy_glyph(control, glyph, sizeof(glyph));
  if (control2 >= 0) {
    char glyph2[48];
    instr_copy_glyph(control2, glyph2, sizeof(glyph2));
    const size_t n = strnlen(glyph, sizeof(glyph));
    snprintf(glyph + n, sizeof(glyph) - n, "%s", glyph2);  // two glyphs in one slot (e.g. L1 R1)
  }
  invoke_return_ctx<int>(0, g_n.begin_scaleform_movie_method, g_instr_handle, "SET_DATA_SLOT");
  invoke_void_ctx(0, g_n.scaleform_movie_method_add_param_int, slot);
  if (glyph[0]) {
    invoke_void_ctx(0, g_n.scaleform_movie_method_add_param_player_name_string, (const char*)glyph);
  } else {
    instr_push_text_param(glyph_token);  // control id had no glyph -> try the ~INPUT_*~ token
  }
  instr_push_text_param(label);
  invoke_void_ctx(0, g_n.end_scaleform_movie_method);
}

// Re-publish all data slots from the current button set, then ask the movie to lay them out.
static void instr_rebuild_slots(const InstrButton* buttons, uint32_t count) {
  invoke_return_ctx<int>(0, g_n.begin_scaleform_movie_method, g_instr_handle, "CLEAR_ALL");
  invoke_void_ctx(0, g_n.end_scaleform_movie_method);
  for (uint32_t i = 0; i < count; ++i)
    instr_set_data_slot((int)i, buttons[i].control, buttons[i].control2, buttons[i].glyph,
                        buttons[i].label);
  // DRAW_INSTRUCTIONAL_BUTTONS(-1): lay out every slot we just set (no slot highlighted).
  invoke_return_ctx<int>(0, g_n.begin_scaleform_movie_method, g_instr_handle,
                         "DRAW_INSTRUCTIONAL_BUTTONS");
  invoke_void_ctx(0, g_n.scaleform_movie_method_add_param_int, -1);
  invoke_void_ctx(0, g_n.end_scaleform_movie_method);
}

#endif  // gate

// Worker-safe publish of the focused row's ordered buttons. Stores the set and -- only on a real
// change -- bumps the generation so the game thread rebuilds the slots. No native calls here.
extern "C" void gtav_features_instructional_publish(uint32_t count,
                                                    const GtavInstructionalButton* buttons) {
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (count > GTAV_INSTR_MAX_BUTTONS) count = GTAV_INSTR_MAX_BUTTONS;
  if (!buttons) count = 0u;
  // Detect a change vs the last published set so we do not churn the game-thread rebuild every
  // frame (the focused row is stable while the cursor dwells).
  int changed = (count != g_instr_worker_count);
  for (uint32_t i = 0; i < count && !changed; ++i) {
    if (g_instr_worker[i].control != buttons[i].control ||
        g_instr_worker[i].control2 != buttons[i].control2 ||
        g_instr_worker[i].glyph != buttons[i].glyph ||
        g_instr_worker[i].label != buttons[i].label)
      changed = 1;
  }
  if (changed) {
    for (uint32_t i = 0; i < count; ++i) {
      g_instr_worker[i].control = buttons[i].control;
      g_instr_worker[i].control2 = buttons[i].control2;
      g_instr_worker[i].glyph = buttons[i].glyph;
      g_instr_worker[i].label = buttons[i].label;
    }
    g_instr_worker_count = count;
    g_instr_publish_pending = 1u;
  }
  if (g_instr_publish_pending) (void)instr_publish_set();
#else
  (void)count;
  (void)buttons;
#endif
}

// Worker-safe: 1 while the native Scaleform bar is built + drawing (game thread). The worker render
// path reads this to stand its own fallback bar down so the two do not overlap.
extern "C" int gtav_features_instructional_active(void) {
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES
  return __atomic_load_n(&g_instr_drawing, __ATOMIC_ACQUIRE) ? 1 : 0;
#else
  return 0;
#endif
}

// Worker-safe diagnostic snapshot of the Route B Scaleform state (all cached volatiles written by
// the game-thread tick; no native calls here). Lets the bar show WHY the native glyphs are/aren't
// up: handle = REQUEST_SCALEFORM_MOVIE result, loaded = HAS_SCALEFORM_MOVIE_LOADED, ready = all
// scaleform natives resolved, open = menu-open seen by the game thread, draws = DRAW_SCALEFORM calls.
extern "C" void gtav_features_instructional_debug(int* handle, int* loaded, int* ready, int* open,
                                                  uint32_t* draws) {
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES
  if (handle) *handle = __atomic_load_n(&g_instr_dbg_handle, __ATOMIC_RELAXED);
  if (loaded) *loaded = __atomic_load_n(&g_instr_dbg_loaded, __ATOMIC_RELAXED);
  if (ready) *ready = __atomic_load_n(&g_instr_dbg_ready, __ATOMIC_RELAXED);
  if (open) *open = __atomic_load_n(&g_instr_dbg_open, __ATOMIC_RELAXED);
  if (draws) *draws = __atomic_load_n(&g_instr_dbg_draws, __ATOMIC_RELAXED);
#else
  if (handle) *handle = -1;
  if (loaded) *loaded = -1;
  if (ready) *ready = -1;
  if (open) *open = -1;
  if (draws) *draws = 0u;
#endif
}

#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES
// Game-thread driver, called every frame from gtav_features_game_thread_tick. Requests the movie
// once, rebuilds the data slots when the worker's published set changes, and draws it fullscreen
// whenever the worker has published a non-empty set. The worker arbiter publishes the menu-row hints
// while the panel is open AND a mode's hints while it hides the panel (object move / free cam), so
// keying on the published count -- not g_menu_open -- lets the bar follow the active mode. The worker
// publishes count 0 when nothing should show, which stands the bar down. Cheap when idle (a couple of
// volatile reads + early return).
static void instructional_game_thread_tick(void) {
  InstrButton buttons[GTAV_INSTR_MAX_BUTTONS];
  uint32_t count = 0;
  uint64_t generation = 0;
  __atomic_store_n(&g_instr_dbg_open, g_menu_open ? 1 : 0, __ATOMIC_RELAXED);
  if (instr_snapshot(buttons, &count, &generation) != 0) return;
  if (count == 0u) {
#if GTAV_MENU_PHASE_DRAW_LIST
    __atomic_store_n(&g_instr_phase_handle, 0u, __ATOMIC_RELEASE);
#endif
    __atomic_store_n(&g_instr_drawing, 0u, __ATOMIC_RELEASE);
    return;
  }
  const int ready = instr_natives_ready();
  __atomic_store_n(&g_instr_dbg_ready, ready, __ATOMIC_RELAXED);
  if (!ready) {
    __atomic_store_n(&g_instr_drawing, 0u, __ATOMIC_RELEASE);
    return;
  }
  // Request only until the movie is resident (requesting every frame is wasteful, and once we hold
  // the handle the streamer keeps it loaded). Re-request if it ever drops (handle goes invalid).
  if (g_instr_handle == 0)
    g_instr_handle = invoke_return_ctx<int>(0, g_n.request_scaleform_movie, "instructional_buttons");
  __atomic_store_n(&g_instr_dbg_handle, g_instr_handle, __ATOMIC_RELAXED);
  const int loaded =
      g_instr_handle ? invoke_return_ctx<int>(0, g_n.has_scaleform_movie_loaded, g_instr_handle) : 0;
  __atomic_store_n(&g_instr_dbg_loaded, loaded ? 1 : 0, __ATOMIC_RELAXED);
  if (!g_instr_handle || !loaded) {
    __atomic_store_n(&g_instr_drawing, 0u, __ATOMIC_RELEASE);
    return;
  }
  // Rebuild the data slots ONLY when the focused row's buttons change (g_instr_gen). Rebuilding
  // every frame relayouts the whole Scaleform and tanks the framerate -- that was the menu lag.
  // The built slots persist in the movie; DRAW just re-renders them each frame.
  if (generation != g_instr_built_gen) {
#if GTAV_MENU_PHASE_DRAW_LIST
    __atomic_store_n(&g_instr_phase_handle, 0u, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_instr_phase_readers, __ATOMIC_ACQUIRE) != 0) {
      __atomic_store_n(&g_instr_drawing, 0u, __ATOMIC_RELEASE);
      return;
    }
#endif
    instr_rebuild_slots(buttons, count);
    g_instr_built_gen = generation;
  }
#if GTAV_MENU_PHASE_DRAW_LIST
  __atomic_store_n(&g_instr_phase_handle, (uint64_t)(uint32_t)g_instr_handle, __ATOMIC_RELEASE);
  __atomic_store_n(&g_instr_drawing, 1u, __ATOMIC_RELEASE);
#else
  // DRAW_SCALEFORM_MOVIE_FULLSCREEN(handle, r, g, b, a, unk) -- submit the bar to the render list
  // every frame, exactly as a script would. p5 0 = default blend.
  invoke_void_ctx(0, g_n.draw_scaleform_movie_fullscreen, g_instr_handle, 255, 255, 255, 255, 0);
  __atomic_store_n(&g_instr_drawing, 1u, __ATOMIC_RELEASE);
  __atomic_fetch_add(&g_instr_dbg_draws, 1u, __ATOMIC_RELAXED);
#endif
}
#endif

extern "C" int gtav_features_instructional_phase_draw(void) {
#if defined(GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM) && GTAV_MENU_ENABLE_INSTRUCTIONAL_SCALEFORM && \
    GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_ENABLE_NATIVE_FEATURES && GTAV_MENU_PHASE_DRAW_LIST
  const uint64_t handle = __atomic_load_n(&g_instr_phase_handle, __ATOMIC_ACQUIRE);
  if (!handle) return 0;
  __atomic_fetch_add(&g_instr_phase_readers, 1u, __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&g_instr_phase_handle, __ATOMIC_ACQUIRE) != handle) {
    __atomic_fetch_sub(&g_instr_phase_readers, 1u, __ATOMIC_RELEASE);
    return 0;
  }
  invoke_void_ctx(0, g_n.draw_scaleform_movie_fullscreen, (int)handle, 255, 255, 255, 255, 0);
  __atomic_fetch_add(&g_instr_dbg_draws, 1u, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g_instr_phase_readers, 1u, __ATOMIC_RELEASE);
  return 1;
#else
  return 0;
#endif
}
