// Vehicle-spawner image preview: stream the highlighted vehicle's preview texture dict
//
// Source fragment of features.cpp, textually included inside its anonymous namespace --
// these stay file-local statics sharing g_n / the invoke helpers. Not a standalone TU.
//
// A vehicle's preview thumbnail lives in a streamed texture dictionary; we DRAW_SPRITE a named
// texture out of it. Crucially the dict name and the texture name are tracked INDEPENDENTLY
// (see include/gtavmenu/vehicle_preview_catalog_generated.h): a model-named dict frequently
// loads, yet the texture inside it is not named after the model, so the old DRAW_SPRITE(model,
// model) drew a white quad. We therefore stream the dict here and let the render path draw only
// a (dict, texture) pair that came from the verified registry.
//
// Threading: REQUEST_STREAMED_TEXTURE_DICT is a streaming native (NOT worker-safe, like
// REQUEST_MODEL), so request/keep-alive/release run on the game-thread frame-hook drain. The
// worker publishes a claimed immutable target and reads generation-tagged residency. With the
// phase renderer, DRAW_SPRITE is copied into the immutable menu list and the old dict is released
// only after a replacement generation is published and every older phase reader has drained.

// Inline target-string buffers. Sized to match the GtavVehiclePreviewEntry fields (+1 for NUL).
#define GTAV_PREVIEW_DICT_LEN 32u
#define GTAV_PREVIEW_TEX_LEN 48u
#define GTAV_PREVIEW_CAP_LEN 40u

#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "preview publication requires lock-free qwords");

struct PreviewTarget {
  uint64_t generation;
  char dict[GTAV_PREVIEW_DICT_LEN];
  char texture[GTAV_PREVIEW_TEX_LEN];
};
static PreviewTarget g_preview_targets[2];
static uint64_t g_preview_target_token;
static uint64_t g_preview_target_readers[2];
static uint64_t g_preview_target_generation;

// These strings and generation are worker-owned. The renderer reads exactly what the setter wrote
// this tick; only the claimed PreviewTarget slots cross to the game thread.
static char g_preview_worker_dict[GTAV_PREVIEW_DICT_LEN];
static char g_preview_worker_texture[GTAV_PREVIEW_TEX_LEN];
static char g_preview_caption[GTAV_PREVIEW_CAP_LEN];
static uint64_t g_preview_worker_generation;

// Game-thread results are tagged with the exact target generation they describe. A cursor change
// therefore makes stale residency structurally invisible even if an old job finishes late.
static uint64_t g_preview_ready_generation;
static uint32_t g_preview_res_w;
static uint32_t g_preview_res_h;

#if GTAV_MENU_PHASE_DRAW_LIST
// Seqlock-style atomic publication of the latest immutable list generation and the one preview
// dict it references (hash 0 = no preview sprite). All fields are atomically accessed, so unlike
// the old volatile character buffers this has no C/C++ data race.
static uint64_t g_preview_phase_seq;
static uint64_t g_preview_phase_generation;
static uint64_t g_preview_phase_dict_hash;
#endif

// Bounded copy into a fixed buffer, always NUL-terminated. src may be NULL ("" then).
static void preview_copy(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  size_t i = 0;
  for (; src[i] && i + 1 < cap; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

static uint32_t preview_target_index(uint64_t token) {
  const uint32_t encoded = (uint32_t)(token & 0xffu);
  return encoded >= 1u && encoded <= 2u ? encoded - 1u : UINT32_MAX;
}

static int preview_publish_target(void) {
  const uint64_t token = __atomic_load_n(&g_preview_target_token, __ATOMIC_ACQUIRE);
  const uint32_t current = preview_target_index(token);
  const uint32_t candidate = current == UINT32_MAX ? 0u : current ^ 1u;
  if (__atomic_load_n(&g_preview_target_readers[candidate], __ATOMIC_ACQUIRE) != 0) return -1;
  PreviewTarget* target = &g_preview_targets[candidate];
  target->generation = ++g_preview_target_generation;
  preview_copy(target->dict, sizeof(target->dict), g_preview_worker_dict);
  preview_copy(target->texture, sizeof(target->texture), g_preview_worker_texture);
  __atomic_store_n(&g_preview_target_token,
                   (target->generation << 8u) | (uint64_t)(candidate + 1u), __ATOMIC_RELEASE);
  g_preview_worker_generation = target->generation;
  return 0;
}

static const PreviewTarget* preview_acquire_target(uint64_t* token_out) {
  for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
    const uint64_t token = __atomic_load_n(&g_preview_target_token, __ATOMIC_ACQUIRE);
    const uint32_t index = preview_target_index(token);
    if (index == UINT32_MAX) return NULL;
    __atomic_fetch_add(&g_preview_target_readers[index], 1u, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_preview_target_token, __ATOMIC_ACQUIRE) == token) {
      *token_out = token;
      return &g_preview_targets[index];
    }
    __atomic_fetch_sub(&g_preview_target_readers[index], 1u, __ATOMIC_RELEASE);
  }
  return NULL;
}

static void preview_release_target(const PreviewTarget* target) {
  const uint32_t index = target == &g_preview_targets[0] ? 0u : 1u;
  __atomic_fetch_sub(&g_preview_target_readers[index], 1u, __ATOMIC_RELEASE);
}

#if GTAV_MENU_PHASE_DRAW_LIST
static uint64_t preview_dict_hash(const char* value) {
  uint64_t hash = 1469598103934665603ull;
  if (!value || !value[0]) return 0;
  for (size_t i = 0; value[i]; ++i) {
    hash ^= (uint8_t)value[i];
    hash *= 1099511628211ull;
  }
  return hash;
}
#endif
#endif

// Worker-safe: publish the preview target (dict to stream, texture to draw, caption to show) and
// -- throttled -- kick a game-thread PREVIEW_TXD job to (re)request the dict so it stays resident
// while shown. dict == "" stops the preview (cursor left a mapped row); texture/caption may still
// carry the vehicle label for the placeholder card. NEVER calls a streaming native on the worker.
// No-op unless built with the preview gate + the frame hook.
extern "C" void gtav_features_preview_set_target(const char* dict, const char* texture,
                                                 const char* caption) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  if (!dict) dict = "";
  // Detect a dict change vs the last one we published: that is the streamer key, so a change
  // clears the stale loaded flag immediately (no sprite flash for a not-yet-streamed dict) and
  // forces a fresh request. texture/caption are refreshed every call so the card stays correct
  // even when two different vehicles share an (empty) dict.
  const int changed = strncmp(dict, g_preview_worker_dict, sizeof(g_preview_worker_dict)) != 0 ||
                      strncmp(texture ? texture : "", g_preview_worker_texture,
                              sizeof(g_preview_worker_texture)) != 0;
  preview_copy(g_preview_worker_dict, sizeof(g_preview_worker_dict), dict);
  preview_copy(g_preview_worker_texture, sizeof(g_preview_worker_texture), texture);
  preview_copy(g_preview_caption, sizeof(g_preview_caption), caption);
  if (changed) {
    g_preview_worker_generation = 0;
    __atomic_store_n(&g_preview_ready_generation, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preview_res_w, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_preview_res_h, 0u, __ATOMIC_RELAXED);
  }
  if ((changed || !g_preview_worker_generation) && preview_publish_target() != 0) return;
  // Keep-alive: the streamer drops an un-requested dict, so re-request periodically while the
  // cursor dwells. Empty targets also retry at this cadence until a superseded dict is safely
  // released; otherwise leaving the vehicle row could permanently pin its texture dictionary.
  static uint32_t calls;
  if (!changed && (calls++ % 6u) != 0u) return;
  if (gtav_frame_hook_is_active())
    gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_PREVIEW_TXD, 0);
#else
  (void)dict;
  (void)texture;
  (void)caption;
#endif
}

// Worker-safe getter: 1 when the highlighted vehicle's preview dict is resident (safe to draw).
extern "C" int gtav_features_preview_texture_loaded(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  const uint64_t ready = __atomic_load_n(&g_preview_ready_generation, __ATOMIC_ACQUIRE);
  return ready && ready == g_preview_worker_generation ? 1 : 0;
#else
  return 0;
#endif
}

// Worker-safe getters for the current preview target. Stable, NUL-terminated, never NULL; ""
// when there is no mapping (or the gate is off). The render path reads these to DRAW_SPRITE the
// (dict, texture) pair and label the card, keeping the preview layer the single source of truth.
extern "C" const char* gtav_features_preview_dict(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  return g_preview_worker_dict;
#else
  return "";
#endif
}
extern "C" const char* gtav_features_preview_texture(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  return g_preview_worker_texture;
#else
  return "";
#endif
}
extern "C" const char* gtav_features_preview_caption(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  return g_preview_caption;
#else
  return "";
#endif
}
extern "C" uint32_t gtav_features_preview_res_w(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  if (!gtav_features_preview_texture_loaded()) return 0;
  return __atomic_load_n(&g_preview_res_w, __ATOMIC_RELAXED);
#else
  return 0;
#endif
}
extern "C" uint32_t gtav_features_preview_res_h(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  if (!gtav_features_preview_texture_loaded()) return 0;
  return __atomic_load_n(&g_preview_res_h, __ATOMIC_RELAXED);
#else
  return 0;
#endif
}

extern "C" void gtav_features_preview_phase_published(uint64_t generation, const char* dict) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK && GTAV_MENU_PHASE_DRAW_LIST
  __atomic_fetch_add(&g_preview_phase_seq, 1u, __ATOMIC_ACQ_REL);
  __atomic_store_n(&g_preview_phase_generation, generation, __ATOMIC_RELAXED);
  __atomic_store_n(&g_preview_phase_dict_hash, preview_dict_hash(dict), __ATOMIC_RELAXED);
  __atomic_fetch_add(&g_preview_phase_seq, 1u, __ATOMIC_RELEASE);
#else
  (void)generation;
  (void)dict;
#endif
}

#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
// GET_TEXTURE_RESOLUTION(dict, texture) -> scrVector {width, height, 0}. Returns 0x0 when the
// texture is absent from the (loaded) dict. Reads returns[0]/[1] as floats, mirroring
// invoke_vector (the scrVector lands one float per padded 8-byte return slot). Game-thread only.
struct PreviewRes {
  float w;
  float h;
};
static PreviewRes query_texture_resolution(const char* dict, const char* texture) {
  PreviewRes r = {0.0f, 0.0f};
  if (!g_n.get_texture_resolution || !dict || !dict[0] || !texture || !texture[0]) return r;
  gtavmenu::NativeCallContext ctx;
  ctx.reset();
  ctx.push((const char*)dict);
  ctx.push((const char*)texture);
  gtavmenu::note_native_call(g_n.get_texture_resolution);
  ((void (*)(gtavmenu::NativeArg*))(uintptr_t)g_n.get_texture_resolution)(&ctx.native_arg);
  r.w = *reinterpret_cast<const float*>(&ctx.returns[0]);
  r.h = *reinterpret_cast<const float*>(&ctx.returns[1]);
  return r;
}
#endif

// Game-thread PREVIEW_TXD job, drained by the frame hook (run_gated_action). Reads the worker's
// latest dict, releases the previously-streamed dict when it changes, REQUEST_STREAMED_TEXTURE_
// DICTs the new one (cheap refcount touch when already resident -- this is the keep-alive), and
// publishes whether it is resident. Streams ONLY the dict (DRAW_SPRITE's texture-name argument is
// drawn worker-side and needs no streaming). Does NOT touch g_state / the toast -- a silent
// background helper, exactly like run_prewarm_model_job. No self-requeue: the worker re-enqueues
// on its keep-alive cadence, so a single job runs per enqueue.
static void run_preview_txd_job(void) {
#if GTAV_MENU_ENABLE_VEHICLE_PREVIEW && GTAV_MENU_ENABLE_FRAME_HOOK
  static char streamed[GTAV_PREVIEW_DICT_LEN];  // game-thread-owned: the dict currently requested
  uint64_t target_token = 0;
  const PreviewTarget* target = preview_acquire_target(&target_token);
  if (!target) return;

  if (strncmp(target->dict, streamed, sizeof(streamed)) != 0) {
    // In the phase build, a target change alone is not permission to release the old resource:
    // the phase callback may still own the previous immutable list. Wait until the worker reports
    // a replacement list that does not reference `streamed`, then require every older reader to
    // drain. This is a bounded defer; the worker's keep-alive cadence retries the job.
#if GTAV_MENU_PHASE_DRAW_LIST
    if (streamed[0]) {
      uint64_t sequence = 0, phase_generation = 0, phase_hash = 0;
      int snapshot_ok = 0;
      for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
        const uint64_t before = __atomic_load_n(&g_preview_phase_seq, __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        phase_generation =
            __atomic_load_n(&g_preview_phase_generation, __ATOMIC_RELAXED);
        phase_hash = __atomic_load_n(&g_preview_phase_dict_hash, __ATOMIC_RELAXED);
        const uint64_t after = __atomic_load_n(&g_preview_phase_seq, __ATOMIC_ACQUIRE);
        if (before == after) {
          sequence = after;
          snapshot_ok = 1;
          break;
        }
      }
      (void)sequence;
      if (!snapshot_ok || !phase_generation || phase_hash == preview_dict_hash(streamed) ||
          !gtav_menu_draw_list_before_generation_drained(phase_generation)) {
        preview_release_target(target);
        return;
      }
    }
#endif
    if (streamed[0] && g_n.set_streamed_texture_dict_as_no_longer_needed)
      invoke_void_ctx(0, g_n.set_streamed_texture_dict_as_no_longer_needed, streamed);
    preview_copy(streamed, sizeof(streamed), target->dict);
  }
  if (!streamed[0]) {
    __atomic_store_n(&g_preview_ready_generation, target->generation, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preview_res_w, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_preview_res_h, 0u, __ATOMIC_RELAXED);
    preview_release_target(target);
    return;
  }
  if (!g_n.request_streamed_texture_dict || !g_n.has_streamed_texture_dict_loaded) {
    __atomic_store_n(&g_preview_ready_generation, 0u, __ATOMIC_RELEASE);
    preview_release_target(target);
    return;
  }
  invoke_void_ctx(0, g_n.request_streamed_texture_dict, streamed, 0);
  const int loaded = invoke_return_ctx<int>(0, g_n.has_streamed_texture_dict_loaded, streamed);

  // Diagnostic: once the dict is resident, ask whether it actually contains the texture we intend
  // to DRAW_SPRITE. The texture name is worker-owned (g_preview_texture); reading it here is the
  // same benign cross-thread read as the dict. res 0x0 => wrong texture name / no image in the
  // dict; res NxM (with the sprite still white) => DRAW_SPRITE can't bind it on the worker render
  // thread. Logged once per (dict, residency, resolution) change so it does not spam at ~10 Hz.
  uint32_t rw = 0, rh = 0;
  if (loaded) {
    const PreviewRes res = query_texture_resolution(streamed, target->texture);
    rw = res.w > 0.0f ? (uint32_t)(res.w + 0.5f) : 0u;
    rh = res.h > 0.0f ? (uint32_t)(res.h + 0.5f) : 0u;
  }
  // Publish only if this is still the current target. A newer worker publication may coexist
  // while this claimed slot is in use; its token mismatch prevents stale readiness from escaping.
  if (__atomic_load_n(&g_preview_target_token, __ATOMIC_ACQUIRE) == target_token) {
    __atomic_store_n(&g_preview_res_w, rw, __ATOMIC_RELAXED);
    __atomic_store_n(&g_preview_res_h, rh, __ATOMIC_RELAXED);
    __atomic_store_n(&g_preview_ready_generation, loaded ? target->generation : 0u,
                     __ATOMIC_RELEASE);
  }
  static char last_diag[112];
  char diag[112];
  snprintf(diag, sizeof(diag), "dict=%s tex=%s loaded=%u res=%ux%u", streamed,
           target->texture, (unsigned)(loaded ? 1u : 0u), (unsigned)rw, (unsigned)rh);
  if (strncmp(diag, last_diag, sizeof(last_diag)) != 0) {
    snprintf(last_diag, sizeof(last_diag), "%s", diag);
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_BRIDGE, "preview txd %s", diag);
  }
  preview_release_target(target);
#endif
}
