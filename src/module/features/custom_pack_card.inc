// Source fragment of features.cpp, textually included after custom_rpf.inc.
//
// CUSTOM_PACKS=1 host-debug texture card for the last requested pack card (`menu-ctl.sh
// pack-request N`, then `pack-card-show`). The game thread owns every streaming native; the worker
// only draws the published sprite into the immutable phase list and schedules two internal jobs:
//
//   SHOW (operator param 0)   confirms HAS_STREAMED_TEXTURE_DICT_LOADED and a non-zero
//                             GET_TEXTURE_RESOLUTION for the card's texture before publishing.
//   SHOW (internal param 1)   keep-alive: touches the dict only while it is still resident, so a
//                             card can never trigger a second cold load. Eviction hides the card.
//   RELEASE (operator 0)      hides the card. No native call.
//   RELEASE (internal 1)      queued by the worker only after a published list without the card
//                             exists and every older list has drained; then releases the dict.

#if GTAV_MENU_ENABLE_CUSTOM_PACKS && GTAV_MENU_PHASE_DRAW_LIST

#define GTAV_PACK_CARD_KEEPALIVE_PUBLISHES 30u

// Written on the game thread, read by the worker.
static uint32_t g_card_shown;
static uint32_t g_card_res_w;
static uint32_t g_card_res_h;
static uint32_t g_card_hide_requested;
static uint32_t g_card_released;
// Worker-owned bookkeeping.
static uint64_t g_card_hidden_generation;
static uint32_t g_card_release_queued;
static uint32_t g_card_publishes;

// The card shows the pack card the operator requested last; it is chosen only by an operator SHOW
// on the game thread (hide, switch, re-check), so the worker never draws a dictionary that the
// show path has not just confirmed resident.
static uint32_t g_card_member;  // n > 0: pack card n - 1; 0: none chosen yet
static int card_requested(void) { return g_rpf_request_attempted; }
static const char* card_dict(void) {
  const uint32_t member = __atomic_load_n(&g_card_member, __ATOMIC_ACQUIRE);
  return member && member <= (uint32_t)g_pack.card_count ? g_pack.cards[member - 1u].dict : "";
}
static const char* card_texture(void) {
  const uint32_t member = __atomic_load_n(&g_card_member, __ATOMIC_ACQUIRE);
  return member && member <= (uint32_t)g_pack.card_count ? g_pack.cards[member - 1u].texture : "";
}

// Preserve the status ABI/text and also retain the full diagnostic in klog. Pack-notes mirrors
// its first 159 characters: ordinary names fit; long dictionary/texture pairs need the klog.
static void card_shown_note(uint32_t width, uint32_t height, uint32_t flags) {
  char line[256];  // Two 63-character names and the full uint32_t dimensions/flags fit.
  snprintf(line, sizeof(line),
           "GTAVMenu pack card shown dict=%s tex=%s res=%ux%u flags=0x%x", card_dict(),
           card_texture(), (unsigned)width, (unsigned)height, (unsigned)flags);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
}

static void card_released_note(int loaded) {
  char line[128];
  snprintf(line, sizeof(line), "GTAVMenu pack card released dict=%s loaded=%d", card_dict(),
           loaded);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
}

static int card_dict_loaded(void) {
  return g_n.has_streamed_texture_dict_loaded &&
         invoke_return_ctx<int>(0, g_n.has_streamed_texture_dict_loaded, card_dict());
}

// Resident, non-empty and resolvable: the only state in which the worker may draw the sprite.
static int card_query_resident(uint32_t* width, uint32_t* height) {
  *width = 0;
  *height = 0;
  if (!g_n.has_streamed_texture_dict_loaded || !g_n.get_texture_resolution) return 0;
  if (!card_dict_loaded()) return 0;
  const PreviewRes res = query_texture_resolution(card_dict(), card_texture());
  *width = res.w > 0.0f ? (uint32_t)(res.w + 0.5f) : 0u;
  *height = res.h > 0.0f ? (uint32_t)(res.h + 0.5f) : 0u;
  return *width > 0u && *height > 0u;
}

static void card_hide(void) {
  __atomic_store_n(&g_card_shown, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_card_res_w, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_card_res_h, 0u, __ATOMIC_RELAXED);
}

static uint32_t show_pack_card(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_SHOW_PACK_CARD;
  const int keepalive = param == 1u;
  if (keepalive && !__atomic_load_n(&g_card_shown, __ATOMIC_ACQUIRE))
    return GTAV_FEATURE_RESULT_NONE;
  if (!keepalive) {
    if (!card_requested()) return unavailable(action, "no texture dictionary has been requested");
    if (__atomic_load_n(&g_card_released, __ATOMIC_ACQUIRE))
      return unavailable(action, "pack card already released this process");
    if (!texture_action_is_stable_main_thread())
      return unavailable(action, "pack card phase/thread gate refused");
    const uint32_t member = (uint32_t)g_pack_last_card;
    if (member == 0u) return unavailable(action, "no pack card has been requested");
    if (member != __atomic_load_n(&g_card_member, __ATOMIC_ACQUIRE)) {
      card_hide();
      __atomic_store_n(&g_card_member, member, __ATOMIC_RELEASE);
    }
  }

  // Archive members have no loose association: resident means the engine reports the dict loaded.
  const uint32_t flags = card_dict_loaded() ? 1u : 0u;
  if ((flags & 3u) != 1u) {
    // Not resident: never request again from here. A keep-alive that finds the dict evicted hides
    // the card and reports it once instead of starting a second cold load.
    const int was_shown = __atomic_load_n(&g_card_shown, __ATOMIC_ACQUIRE) != 0;
    card_hide();
    if (keepalive) {
      if (was_shown)
        gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack card evicted flags=0x%x",
                           (unsigned)flags);
      return GTAV_FEATURE_RESULT_NONE;
    }
    char message[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(message, sizeof(message), "pack card not loaded flags=0x%x", (unsigned)flags);
    return unavailable(action, message);
  }

  // Resident: re-requesting is a reference touch that keeps the streamer from dropping it.
  if (keepalive && g_n.request_streamed_texture_dict)
    invoke_void_ctx(0, g_n.request_streamed_texture_dict, card_dict(), 0);
  uint32_t width = 0, height = 0;
  if (!card_query_resident(&width, &height)) {
    card_hide();
    if (keepalive) return GTAV_FEATURE_RESULT_NONE;
    return failed(action, "pack card loaded but texture did not resolve");
  }
  __atomic_store_n(&g_card_res_w, width, __ATOMIC_RELAXED);
  __atomic_store_n(&g_card_res_h, height, __ATOMIC_RELAXED);
  if (keepalive) return GTAV_FEATURE_RESULT_NONE;

  __atomic_store_n(&g_card_hide_requested, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_card_shown, 1u, __ATOMIC_RELEASE);
  card_shown_note(width, height, flags);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack card shown res=%ux%u", (unsigned)width,
           (unsigned)height);
  return ok(action, message);
}

static uint32_t release_pack_card(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_RELEASE_PACK_CARD;
  if (param != 1u) {
    if (!card_requested()) return unavailable(action, "no texture dictionary has been requested");
    if (__atomic_load_n(&g_card_released, __ATOMIC_ACQUIRE))
      return ok(action, "pack card already released");
    card_hide();
    __atomic_store_n(&g_card_hide_requested, 1u, __ATOMIC_RELEASE);
    return ok(action, "pack card hidden; release follows list drain");
  }

  // Internal: the worker proved no published or in-flight list can still reference the dict.
  if (!__atomic_load_n(&g_card_hide_requested, __ATOMIC_ACQUIRE) ||
      __atomic_load_n(&g_card_released, __ATOMIC_ACQUIRE))
    return GTAV_FEATURE_RESULT_NONE;
  __atomic_store_n(&g_card_released, 1u, __ATOMIC_RELEASE);
  if (g_n.set_streamed_texture_dict_as_no_longer_needed)
    invoke_void_ctx(0, g_n.set_streamed_texture_dict_as_no_longer_needed, card_dict());
  card_released_note(card_dict_loaded());
  return GTAV_FEATURE_RESULT_NONE;
}

// Worker-safe: 1 while the card should be drawn, with the resolved texture size.
extern "C" int gtav_features_custom_card(const char** dict, const char** texture, uint32_t* width,
                                         uint32_t* height) {
  if (!__atomic_load_n(&g_card_shown, __ATOMIC_ACQUIRE)) return 0;
  *width = __atomic_load_n(&g_card_res_w, __ATOMIC_RELAXED);
  *height = __atomic_load_n(&g_card_res_h, __ATOMIC_RELAXED);
  if (!*width || !*height) return 0;
  *dict = card_dict();
  *texture = card_texture();
  return 1;
}

// Worker: called after every published phase list, with whether that list contains the card.
extern "C" void gtav_features_custom_card_published(uint64_t generation, int drawn) {
  if (__atomic_load_n(&g_card_hide_requested, __ATOMIC_ACQUIRE)) {
    if (drawn || g_card_release_queued) return;
    if (!g_card_hidden_generation) g_card_hidden_generation = generation;
    if (gtav_menu_draw_list_before_generation_drained(g_card_hidden_generation) &&
        gtav_frame_hook_is_active()) {
      g_card_release_queued = 1;
      gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_RELEASE_PACK_CARD, 1u);
    }
    return;
  }
  if (!drawn) return;
  if ((g_card_publishes++ % GTAV_PACK_CARD_KEEPALIVE_PUBLISHES) == 0u &&
      gtav_frame_hook_is_active())
    gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_SHOW_PACK_CARD, 1u);
}

#else

static uint32_t show_pack_card(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_SHOW_PACK_CARD,
                     "pack card requires CUSTOM_PACKS=1 phase build");
}

static uint32_t release_pack_card(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_RELEASE_PACK_CARD,
                     "pack card requires CUSTOM_PACKS=1 phase build");
}

#endif
