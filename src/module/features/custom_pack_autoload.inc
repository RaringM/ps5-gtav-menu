// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS && PS5 branch of the runtime pack lane.
//
// One-press pack loading (worker state machine) and the Custom Packs page rows.

// One-press pack loading from the menu (Custom Packs page): a worker-side state machine walks the
// proven host sequence (`menu-ctl pack-up`, then maps) by enqueueing the same main-thread actions
// and polling the globals they set: register, archive load (wait resident), each data row (load,
// then pump until parsed), archetype requests, labels, then maps (bounds load, finish retries).
// Every step keeps its own gates; a refusal or timeout stops the machine with its reason.
enum {
  PACK_AL_IDLE = 0,
  PACK_AL_REGISTER,
  PACK_AL_ARCHIVE,
  PACK_AL_DATA,
  PACK_AL_PUMP,
  PACK_AL_TYPS,
  PACK_AL_LABELS,
  PACK_AL_MAP,
  PACK_AL_MAP_FINISH,
  PACK_AL_BOUNDS,
  PACK_AL_HIDE,  // descriptor `hide` rows, after the maps (custom_pack_hide.inc)
  PACK_AL_DONE,
  PACK_AL_FAILED,
};
#define GTAV_PACK_AL_TIMEOUT_TICKS 1800u
#define GTAV_PACK_AL_RETRY_TICKS 30u
static volatile uint32_t g_pack_al_state;
static uint32_t g_pack_al_row;
static uint32_t g_pack_al_ticks;
static int g_pack_al_sent;
static char g_pack_al_status[48] = "Load custom pack";
// The failed load's short reason with the pack it stopped at, for the load row's footer.
static char g_pack_al_detail[GTAV_FEATURE_MESSAGE_LEN];
// Carcols gate wait (pack_carcols_gate kPackCarcolsInUse): tuned cars, parked ones too, draw from
// the kit or wheel arrays the data row would grow. Nothing of the row ran (no parse), so the load
// stays on it and asks again until the gate passes (gtav_custom_pack_gate_wait: every 2 s, at most
// 3 min). Monotonic ms: the worker rate is a menu setting.
static uint64_t g_pack_al_wait_ms;  // when the wait started; 0 while not waiting
static uint64_t g_pack_al_wait_sent_ms;

static void pack_al_toast(const char* text);
static uint64_t pack_al_now_ms() {
  struct timespec now = {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

// A load is walking its steps (the uninstall row waits: the steps read the selected packs' files).
static int pack_autoload_running() {
  const uint32_t state = __atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE);
  return state != PACK_AL_IDLE && state != PACK_AL_DONE && state != PACK_AL_FAILED;
}

// Ped model store headroom of the merged selection, before Load (S2 2026-10-08: four add-on peds
// took the store from 426 to its 430 cap). Each PED_METADATA_FILE row adds at least one ped model,
// so a selection with more rows than free slots cannot load; the row's own gate (pack_data_gate)
// would refuse it mid-load, after the earlier packs' rows ran (loads are one-way). Writes the
// refusal naming the first pack past the headroom and returns 1; 0 when the rows fit or the store
// is unreadable (the row gate stays the backstop, also for rows declaring several peds).
static int pack_ped_slots_refusal(char* out, size_t size) {
  uint32_t count = 0, capacity = 0;
  if (pack_model_factory_block(kPedModelFactory, kPedModelFactoryVtable, &count, &capacity) != 0)
    return 0;
  uint32_t rows = 0;
  for (int i = 0; i < g_pack.data_count && i < GTAV_CUSTOM_PACK_DATA_CAP; ++i) {
    if (g_pack.data[i].type != GTAV_CUSTOM_PACK_DATA_PED_METADATA || ++rows <= capacity - count)
      continue;
    const int source = g_pack.data_source[i];
    const char* pack = source < g_pack.source_count ? g_pack.source_ids[source] : "pack";
    pack_world_note("pack load refused before start: ped rows=%u store=%u/%u data=%d pack=%s",
                    (unsigned)rows, (unsigned)count, (unsigned)capacity, i, pack);
    snprintf(out, size, "%.30s: ped model slots full (%u); deselect a ped pack", pack,
             (unsigned)capacity);
    return 1;
  }
  return 0;
}

// Refuses before the first step when the selection would not load (pack_select_clash: a clash, a
// cap or a bad pack.cfg), naming the pack to deselect or reinstall: loads are one-way per process.
static uint32_t start_pack_autoload() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (g_pack_al_state == PACK_AL_FAILED)
    return unavailable(action, "Load failed; restart GTA to load again");
  if (g_pack_al_state != PACK_AL_IDLE)
    return unavailable(action, "Packs already loading or loaded");
  if (g_rpf_register_attempted) return unavailable(action, "Loaded by host commands; restart GTA");
  if (!gtav_frame_hook_is_active()) return unavailable(action, "Menu hook not active; reinject");
  pack_select_scan(1);
  if (!g_pack_selection_valid)
    return unavailable(action, "packs/active is invalid; fix it on the host");
  if (g_pack_selected_count == 0) return unavailable(action, "Select packs in Manage Packs first");
  if (const char* why = pack_clash_text(g_pack_selection_clash, g_pack_selected, 0)) {
    if (g_pack_selection_clash.kind != PACK_CLASH_BUSY) {
      pack_world_note("pack load refused before start: %s", why);
      return unavailable(action, why);
    }
  }
  char ped_refusal[GTAV_FEATURE_MESSAGE_LEN];
  if (g_pack_selection_clash.kind == PACK_CLASH_NONE &&
      pack_ped_slots_refusal(ped_refusal, sizeof(ped_refusal)))
    return unavailable(action, ped_refusal);
  // Early warning only: the kit/wheel growth is known once a carcols row is staged, after the
  // archives registered, and its gate then waits (pack_al_gate_wait). g_pack is the merged
  // selection the clash check just built.
  uint32_t carcols = 0;
  for (int i = 0; g_pack_selection_clash.kind == PACK_CLASH_NONE && i < g_pack.data_count; ++i)
    carcols += g_pack.data[i].type == GTAV_CUSTOM_PACK_DATA_CARCOLS;
  if (const uint32_t tuned = carcols ? pack_tuned_cars() : 0) {
    PackVarArrays vars;
    const int read = pack_var_arrays(&vars) == 0;
    pack_world_note("pack load warning tuned=%u carcols=%u kits=%u/%u", (unsigned)tuned,
                    (unsigned)carcols, read ? (unsigned)vars.kit_count : 0u,
                    read ? (unsigned)vars.kit_capacity : 0u);
    pack_al_toast("Tuned car nearby (parked ones count): the load may wait");
  }
  g_pack_al_row = 0;
  g_pack_al_ticks = 0;
  g_pack_al_sent = 0;
  g_pack_al_wait_ms = 0;
  snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: registering");
  __atomic_store_n(&g_pack_al_state, (uint32_t)PACK_AL_REGISTER, __ATOMIC_RELEASE);
  return ok(action, "custom pack loading");
}

// The pack (merged-set source) whose row the load stopped at: the failing row's own pack, the
// archive or override the registration refused (custom_rpf.inc g_pack_fail_archive), or the only
// pack; -1 when the step covers every pack (labels, archive streaming) or nothing is merged yet.
static int pack_al_source(uint32_t state, uint32_t row) {
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return -1;
  int source = -1;
  switch (state) {
    case PACK_AL_DATA:
    case PACK_AL_PUMP:
      source = row < (uint32_t)g_pack.data_count ? g_pack.data_source[row] : -1;
      break;
    case PACK_AL_TYPS:
      source = row < (uint32_t)g_pack.typ_count ? g_pack.typ_source[row] : -1;
      break;
    case PACK_AL_MAP:
    case PACK_AL_MAP_FINISH:
      source = row < (uint32_t)g_pack.map_count ? g_pack.map_source[row] : -1;
      break;
    case PACK_AL_BOUNDS:
      source = row < (uint32_t)g_pack.bounds_count ? g_pack.bounds_source[row] : -1;
      break;
    case PACK_AL_HIDE:
      source = row < (uint32_t)g_pack.hide_count ? g_pack.hide_source[row] : -1;
      break;
    case PACK_AL_REGISTER: {
      const int archive = __atomic_load_n(&g_pack_fail_archive, __ATOMIC_ACQUIRE);
      if (archive == 0) source = 0;
      if (archive > 0 && archive <= g_pack.extra_archive_count)
        source = g_pack.extra_archive_source[archive - 1];
      break;
    }
    default:
      break;
  }
  if (source < 0 && g_pack.source_count == 1) source = 0;
  return source < g_pack.source_count ? source : -1;
}

// Stop at `stage`, naming the step's own refusal when it gave one (toast, page row, status), and
// the pack it stopped at when one row's pack is known (toast and the load row's footer).
static void pack_al_fail(const char* stage) {
  char why[GTAV_FEATURE_MESSAGE_LEN + GTAV_CUSTOM_PACK_ID_MAX];
  char refusal[GTAV_FEATURE_MESSAGE_LEN];
  pack_step_refusal(refusal, sizeof(refusal));
  if (refusal[0])
    snprintf(why, sizeof(why), "%s: %s", stage, refusal);
  else
    snprintf(why, sizeof(why), "%s", stage);
  const int source =
      pack_al_source(__atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE), g_pack_al_row);
  const char* pack = source >= 0 ? g_pack.source_ids[source] : nullptr;
  if (pack) snprintf(why + strlen(why), sizeof(why) - strlen(why), " pack=%s", pack);
  // The klog / pack-notes line and the status keep the technical stage + refusal; the page row and
  // the toast show the short player-facing text (custom_pack_checks.c), which fits the row.
  char line[sizeof(why) + 32];
  snprintf(line, sizeof(line), "GTAVMenu pack load failed: %s", why);
  gtav_worker_klog(line);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack autoload failed: %s", why);
  const char* reason = gtav_custom_pack_user_reason(refusal[0] ? refusal : stage);
  snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Load failed: %s", reason);
  char toast[GTAV_FEATURE_MESSAGE_LEN];
  if (pack)
    snprintf(toast, sizeof(toast), "%.36s: %s", pack, reason);
  else
    snprintf(toast, sizeof(toast), "Custom packs failed: %s", reason);
  snprintf(g_pack_al_detail, sizeof(g_pack_al_detail), "%s", toast);
  gtav_native_bridge_push_toast(toast, GTAV_FEATURE_RESULT_FAILED);
  __atomic_store_n(&g_pack_al_state, (uint32_t)PACK_AL_FAILED, __ATOMIC_RELEASE);
}

static void pack_al_next(uint32_t state, uint32_t row) {
  if (g_pack_al_wait_ms) {  // the waiting data row went on (pack_al_gate_wait)
    pack_world_note("pack data %u carcols gate clear after %us", (unsigned)g_pack_al_row,
                    (unsigned)((pack_al_now_ms() - g_pack_al_wait_ms) / 1000u));
    pack_al_toast("Custom packs: tuned car gone, loading on");
    g_pack_al_wait_ms = 0;
  }
  g_pack_al_row = row;
  g_pack_al_ticks = 0;
  g_pack_al_sent = 0;
  pack_step_refusal_clear();
  __atomic_store_n(&g_pack_al_state, state, __ATOMIC_RELEASE);
}

static void pack_al_toast(const char* text) {
  gtav_native_bridge_push_toast(text, GTAV_FEATURE_RESULT_OK);
}

// Stage name for a timeout in `state`.
static const char* pack_al_stage(uint32_t state) {
  switch (state) {
    case PACK_AL_REGISTER:
      return "registration";
    case PACK_AL_ARCHIVE:
      return "archive streaming";
    case PACK_AL_DATA:
    case PACK_AL_PUMP:
      return "data files";
    case PACK_AL_TYPS:
      return "archetypes";
    case PACK_AL_LABELS:
      return "labels";
    case PACK_AL_MAP:
      return g_pack_map_waiting_typs ? "waiting for archetypes" : "map request";
    case PACK_AL_MAP_FINISH:
      return "map activation";
    case PACK_AL_BOUNDS:
      return "collision bounds";
    case PACK_AL_HIDE:
      return "stock hides";
    default:
      return "load";
  }
}

// Enqueue `action(param)` once per step (or again every retry interval when `retry`).
static void pack_al_send(uint32_t action, uint32_t param, int retry) {
  if (!g_pack_al_sent || (retry && g_pack_al_ticks % GTAV_PACK_AL_RETRY_TICKS == 0u)) {
    if (gtav_frame_hook_enqueue(action, param)) g_pack_al_sent = 1;
  }
}

// The data row's step refused with the carcols in-use refusal (or, once waiting, has not answered
// yet or found the game busy): keep the load on the row and ask again. Returns 0 when the row is
// not waiting (any other refusal fails the step as before).
static int pack_al_gate_wait(uint32_t row) {
  char refusal[GTAV_FEATURE_MESSAGE_LEN];
  pack_step_refusal(refusal, sizeof(refusal));
  const int in_use = strcmp(refusal, kPackCarcolsInUse) == 0;
  const uint64_t now = pack_al_now_ms();
  if (!g_pack_al_wait_ms && in_use) {
    g_pack_al_wait_ms = g_pack_al_wait_sent_ms = now ? now : 1u;
    pack_world_note("pack data %u carcols gate waiting hits=%u gfx=%u", (unsigned)row,
                    (unsigned)g_pack_carcols_hits, (unsigned)g_pack_carcols_gfx);
    snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Waiting: tuned car nearby; move away");
    pack_al_toast("Packs waiting: tuned car nearby (parked ones count); move away");
  }
  const uint64_t waited = g_pack_al_wait_ms ? now - g_pack_al_wait_ms : 0;
  // Once waiting, a busy game (the player drives off) holds the wait like the gate does.
  const int busy = g_pack_al_wait_ms && (strcmp(refusal, "pack phase/thread gate refused") == 0 ||
                                         strcmp(refusal, "parser is not idle") == 0);
  switch (gtav_custom_pack_gate_wait(g_pack_al_wait_ms != 0, refusal[0] != '\0', in_use || busy,
                                     waited, now - g_pack_al_wait_sent_ms)) {
    case GTAV_CUSTOM_PACK_GATE_NONE:
      return 0;
    case GTAV_CUSTOM_PACK_GATE_GIVE_UP:
      pack_world_note("pack data %u carcols gate gave up after %us hits=%u", (unsigned)row,
                      (unsigned)(waited / 1000u), (unsigned)g_pack_carcols_hits);
      pack_al_fail("carcols gate wait timed out");
      return 1;
    case GTAV_CUSTOM_PACK_GATE_ASK:
      g_pack_al_wait_sent_ms = now;
      pack_step_refusal_clear();
      g_pack_al_sent = 0;
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA, row, 0);
      break;
    default:
      break;
  }
  // The step timeout counts from the last ask; an unanswered one past the limit is left to it.
  if (waited < GTAV_CUSTOM_PACK_GATE_WAIT_MS) g_pack_al_ticks = 0;
  return 1;
}

// "Revert overrides" wait (custom_rpf.inc pack_revert_wait): while a row is still in use, ask the
// revert again every 2 s for at most 3 min (gtav_custom_pack_gate_wait, the carcols gate's rule),
// then toast that it went through, stopped or gave up. An ask lost before it answered gives up a
// minute later.
static void pack_revert_tick() {
  const uint64_t since = __atomic_load_n(&g_revert_wait_ms, __ATOMIC_ACQUIRE);
  if (!since) return;
  const uint32_t answer = __atomic_load_n(&g_revert_answer, __ATOMIC_ACQUIRE);
  const uint64_t now = pack_al_now_ms();
  const unsigned waited = (unsigned)((now - since) / 1000u);
  char text[GTAV_FEATURE_MESSAGE_LEN];
  const char* stop = nullptr;
  if (answer == PACK_REVERT_DONE) {
    pack_world_note("pack revert wait clear after %us", waited);
    snprintf(text, sizeof(text), "Overrides reverted after %us: stock assets are back", waited);
    gtav_native_bridge_push_toast(text, GTAV_FEATURE_RESULT_OK);
  } else if (answer == PACK_REVERT_REFUSED) {
    pack_world_note("pack revert wait stopped after %us", waited);
    stop = "Revert stopped; see pack-notes";
  } else {
    const int answered = answer != PACK_REVERT_ASKED;  // in use, or the game thread was busy
    const uint64_t asked = __atomic_load_n(&g_revert_asked_ms, __ATOMIC_ACQUIRE);
    switch (gtav_custom_pack_gate_wait(1, answered, answered, now - since, now - asked)) {
      case GTAV_CUSTOM_PACK_GATE_ASK:
        __atomic_store_n(&g_revert_asked_ms, now, __ATOMIC_RELEASE);
        __atomic_store_n(&g_revert_answer, (uint32_t)PACK_REVERT_ASKED, __ATOMIC_RELEASE);
        if (!gtav_frame_hook_enqueue(GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES, 0))
          __atomic_store_n(&g_revert_answer, (uint32_t)PACK_REVERT_BUSY, __ATOMIC_RELEASE);
        return;
      case GTAV_CUSTOM_PACK_GATE_GIVE_UP:
        pack_world_note("pack revert wait gave up after %us on %s", waited, g_revert_member);
        snprintf(text, sizeof(text), "Revert gave up: %.32s still in use", g_revert_member);
        stop = text;
        break;
      default:
        if (now - since < GTAV_CUSTOM_PACK_GATE_WAIT_MS + 60000u) return;
        pack_world_note("pack revert wait gave up after %us (no answer)", waited);
        stop = "Revert gave up; press it again";
        break;
    }
  }
  if (stop) gtav_native_bridge_push_toast(stop, GTAV_FEATURE_RESULT_FAILED);
  __atomic_store_n(&g_revert_wait_ms, (uint64_t)0, __ATOMIC_RELEASE);
}

static void pack_autoload_tick() {
  ++g_pack_worker_ticks;  // pack root rescan pacing (custom_pack_select.inc)
  pack_revert_tick();
  const uint32_t state = __atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE);
  if (state == PACK_AL_IDLE || state == PACK_AL_DONE || state == PACK_AL_FAILED) return;
  if (!gtav_frame_hook_is_active()) return;
  if (++g_pack_al_ticks > GTAV_PACK_AL_TIMEOUT_TICKS) {
    char stage[48];
    snprintf(stage, sizeof(stage), "timed out in %s", pack_al_stage(state));
    pack_al_fail(stage);
    return;
  }
  const uint32_t row = g_pack_al_row;
  switch (state) {
    case PACK_AL_REGISTER:
      // The Custom Packs selection check held the descriptor buffer (custom_rpf.inc
      // g_pack_scratch) when the registration ran: send it again.
      if (__atomic_exchange_n(&g_pack_register_busy, 0, __ATOMIC_ACQ_REL)) g_pack_al_sent = 0;
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK, 0, 0);
      if (g_rpf_index >= 0) {
        pack_al_next(PACK_AL_ARCHIVE, 0);
        snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: archive");
      } else if (g_rpf_register_attempted || g_pack_al_ticks > 300u) {
        if (g_pack_al_ticks > 300u) pack_al_fail("registration refused");
      }
      break;
    case PACK_AL_ARCHIVE:
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE, 0, 0);
      if (pack_archives_resident()) {
        char text[GTAV_FEATURE_MESSAGE_LEN];
        snprintf(text, sizeof(text), "Custom packs: %d archive(s) streamed, loading data",
                 pack_archive_count());
        pack_al_toast(text);
        pack_al_next(PACK_AL_DATA, 0);
      }
      break;
    case PACK_AL_DATA:
      if (row >= (uint32_t)g_pack.data_count) {
        pack_al_next(PACK_AL_TYPS, 0);
        break;
      }
      if (!g_pack_al_wait_ms)
        snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: data %u/%d",
                 (unsigned)row + 1u, g_pack.data_count);
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA, row, 0);
      if ((g_pack_data_loaded & (1ull << row)) &&
          __atomic_load_n(&g_pack_pending_row, __ATOMIC_ACQUIRE) == (int32_t)row)
        pack_al_next(PACK_AL_PUMP, row);
      else if (__atomic_load_n(&g_pack_data_parsed, __ATOMIC_ACQUIRE) & (1ull << row))  // sync row
        pack_al_next(PACK_AL_DATA, row + 1u);
      else if (!pack_al_gate_wait(row) && g_pack_al_ticks > 300u)
        pack_al_fail("data row refused");
      break;
    case PACK_AL_PUMP:
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_PUMP_PACK_DATA, 0, 1);
      // Set by pump_pack_data once the parse returned; a stuck pump ends in the step timeout.
      if (__atomic_load_n(&g_pack_data_parsed, __ATOMIC_ACQUIRE) & (1ull << row))
        pack_al_next(PACK_AL_DATA, row + 1u);
      break;
    case PACK_AL_TYPS:
      if (row >= (uint32_t)g_pack.typ_count) {
        pack_al_next(PACK_AL_LABELS, 0);
        break;
      }
      snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: archetypes %u/%d",
               (unsigned)row + 1u, g_pack.typ_count);
      // Retried: a stock (`retail`) row waits until the previous stock typ is resident, a typ with a
      // stock dependency (`typdep`) for archetype pool headroom.
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP, row,
                   g_pack.typ_retail[row] || g_pack.typ_deps[row][0]);
      if (g_pack_typs_failed & (1ull << row))
        pack_al_fail(g_pack.typ_retail[row] ? "stock typ" : "typ dependency");
      else if (g_pack_typs_loaded & (1ull << row))
        pack_al_next(PACK_AL_TYPS, row + 1u);
      break;
    case PACK_AL_LABELS:
      if (!g_pack.label_count) {
        pack_al_next(PACK_AL_BOUNDS, 0);
        break;
      }
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS, 0, 0);
      // Bounds before maps: an interior map binds its MLO's resident .pbn when it finishes.
      if (g_pack_labels_added) pack_al_next(PACK_AL_BOUNDS, 0);
      break;
    case PACK_AL_MAP:
      // After the last map: the hide rows, then back here (all applied) to finish.
      if (row >= (uint32_t)g_pack.map_count && g_pack.hide_count &&
          g_pack_hides_applied != pack_hide_rows_mask()) {
        pack_al_next(PACK_AL_HIDE, 0);
        break;
      }
      if (row >= (uint32_t)g_pack.map_count) {
        snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Ready: %.38s", g_pack.pack_id);
        gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack autoload done id=%s", g_pack.pack_id);
        int kinds[7] = {};
        for (int i = 0; i < g_pack.spawn_count; ++i)
          if (g_pack.spawns[i].kind < 7u) kinds[g_pack.spawns[i].kind]++;
        // Each pack particle dictionary should now have a PtFxAssetStore slot (klog only).
        pack_ptfx_report();
        char text[GTAV_FEATURE_MESSAGE_LEN];
        snprintf(text, sizeof(text),
                 "Custom packs ready: %d vehicles, %d props, %d peds, %d weapons, %d effects, %d maps",
                 kinds[GTAV_CUSTOM_PACK_SPAWN_VEHICLE], kinds[GTAV_CUSTOM_PACK_SPAWN_OBJECT],
                 kinds[GTAV_CUSTOM_PACK_SPAWN_PED], kinds[GTAV_CUSTOM_PACK_SPAWN_WEAPON],
                 kinds[GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE] + kinds[GTAV_CUSTOM_PACK_SPAWN_PTFX],
                 g_pack.map_count);
        pack_al_toast(text);
        __atomic_store_n(&g_pack_al_state, (uint32_t)PACK_AL_DONE, __ATOMIC_RELEASE);
        break;
      }
      snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: map %u/%d", (unsigned)row + 1u,
               g_pack.map_count);
      // Retried: load_pack_map refuses until every typ row is resident.
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP, row, 1);
      // Waiting for the archetype definitions to become resident is bounded by the overall timeout.
      if (g_pack_maps_requested & (1u << row)) pack_al_next(PACK_AL_MAP_FINISH, row);
      else if (g_pack_maps_failed & (1u << row))
        pack_al_fail("map dependency");
      else if (g_pack_al_ticks > 300u && !g_pack_map_waiting_typs)
        pack_al_fail("map row refused");
      break;
    case PACK_AL_MAP_FINISH:
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP, row, 1);
      if (g_pack_maps_failed & (1u << row))
        pack_al_fail("map activation");
      else if (g_pack_maps_finished & (1u << row))
        pack_al_next(PACK_AL_MAP, row + 1u);
      break;
    case PACK_AL_BOUNDS:
      if (row >= (uint32_t)g_pack.bounds_count) {
        pack_al_next(PACK_AL_MAP, 0);
        break;
      }
      snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: collision %u/%d",
               (unsigned)row + 1u, g_pack.bounds_count);
      // Retried: the first call requests, later calls confirm once the bounds are resident.
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS, row, 1);
      if (g_pack_bounds_failed & (1u << row))
        pack_al_fail("collision bounds");
      else if (g_pack_bounds_finished & (1u << row))
        pack_al_next(PACK_AL_BOUNDS, row + 1u);
      break;
    case PACK_AL_HIDE:
      if (row >= (uint32_t)g_pack.hide_count) {
        pack_world_note("pack hides applied %d/%d", __builtin_popcountll(g_pack_hides_applied),
                        g_pack.hide_count);
        pack_al_next(PACK_AL_MAP, (uint32_t)g_pack.map_count);
        break;
      }
      snprintf(g_pack_al_status, sizeof(g_pack_al_status), "Loading: hides %u/%d",
               (unsigned)row + 1u, g_pack.hide_count);
      // Retried: the game-thread gate may refuse a frame; an applied row is never applied twice.
      pack_al_send(GTAV_NATIVE_SHELL_ACTION_PACK_HIDE, row, 1);
      if (g_pack_hides_failed & (1ull << row))
        pack_al_fail("stock hides");
      else if (g_pack_hides_applied & (1ull << row))
        pack_al_next(PACK_AL_HIDE, row + 1u);
      break;
    default:
      break;
  }
}

// TELEPORT_PACK_MAP param of descriptor `place` row i: GTAV_PACK_PLACE_PARAM + i.
#define GTAV_PACK_PLACE_PARAM 0x100u

// A loaded pack map row's declared place, or a `place` row's (row = GTAV_PACK_PLACE_PARAM + i);
// 0 when the row has none or the pack is not loaded.
static int pack_map_place(uint32_t row, int32_t* x, int32_t* y, int32_t* z, const char** text,
                          int* interior) {
  if (row >= GTAV_PACK_PLACE_PARAM) {
    const uint32_t place = row - GTAV_PACK_PLACE_PARAM;
    if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) ||
        place >= (uint32_t)g_pack.place_count || g_rpf_index < 0)
      return 0;
    *x = g_pack.places[place].x;
    *y = g_pack.places[place].y;
    *z = g_pack.places[place].z;
    *text = g_pack.places[place].text;
    *interior = 0;
    return 1;
  }
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) || row >= (uint32_t)g_pack.map_count ||
      !g_pack.map_places[row].text[0] || g_rpf_index < 0)
    return 0;
  *x = g_pack.map_places[row].x;
  *y = g_pack.map_places[row].y;
  *z = g_pack.map_places[row].z;
  *text = g_pack.map_places[row].text;
  *interior = g_pack.map_interior[row] != 0;
  return 1;
}

// Worker: rows for the Custom Packs pages. Row 0 loads the selected packs (or shows progress); per
// loaded pack (in `active` order) a header row (action NONE, label = the pack id), its spawn rows,
// a teleport row per map row that declares a place and one per `place` row, actionable once
// loaded; then the installed-list row and every installed pack (up to GTAV_PACK_INSTALLED_MAX),
// each a selection toggle; last, while any pack is installed, the "Uninstall: <id> < n/m >" row
// (custom_pack_select.inc uninstall_pack). The menu (native_bridge.cpp build_custom_pack_items)
// sorts the rows into the Custom Packs folders: the load row on the root, the installed list and
// the uninstall row under Manage Packs, and the content rows by kind under Vehicles / Weapons /
// Peds / Objects / Effects / Locations, each pack's rows under its header.
// Menu action for a descriptor spawn kind (param: the model or weapon name's joaat).
static uint32_t pack_spawn_action(uint32_t kind) {
  switch (kind) {
    case GTAV_CUSTOM_PACK_SPAWN_VEHICLE:
      return GTAV_NATIVE_SHELL_ACTION_SPAWN_VEHICLE;
    case GTAV_CUSTOM_PACK_SPAWN_PED:
      return GTAV_NATIVE_SHELL_ACTION_SPAWN_PED;
    case GTAV_CUSTOM_PACK_SPAWN_WEAPON:
      return GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON;
    case GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE:
      return GTAV_NATIVE_SHELL_ACTION_APPLY_PACK_TIMECYCLE;
    case GTAV_CUSTOM_PACK_SPAWN_PTFX:
      return GTAV_NATIVE_SHELL_ACTION_PLAY_PACK_PTFX;
    case GTAV_CUSTOM_PACK_SPAWN_COMPONENT:
      return GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_COMPONENT;
    default:
      return GTAV_NATIVE_SHELL_ACTION_SPAWN_OBJECT;
  }
}

static GtavPackMenuRow* pack_menu_row(GtavPackMenuRow* rows, int* count, uint32_t action,
                                      uint32_t param, int ready, const char* reason) {
  GtavPackMenuRow* row = &rows[(*count)++];
  row->action = action;
  row->param = param;
  row->ready = (uint32_t)(ready != 0);
  row->reason = ready ? nullptr : reason;
  row->label[0] = '\0';
  row->toggle = -1;
  return row;
}

extern "C" int gtav_features_pack_menu(GtavPackMenuRow* rows, int max) {
  if (!rows || max < 1) return 0;
  const uint32_t state = __atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE);
  pack_select_scan(0);
  int count = 0;
  const int idle = state == PACK_AL_IDLE && !g_rpf_register_attempted;
  const int nothing_selected = idle && g_pack_selection_valid && g_pack_selected_count == 0;
  const int unavailable_here = !gtav_custom_game_root_available();
  // A failed load stays selectable: its footer names the pack it stopped at (gtav_features_pack_
  // footer) and Cross says to restart GTA.
  GtavPackMenuRow* first = pack_menu_row(
      rows, &count, GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD, 0,
      !unavailable_here && ((idle && !nothing_selected) || state == PACK_AL_FAILED),
      unavailable_here   ? GTAV_CUSTOM_DATA_UNAVAILABLE
      : nothing_selected ? "Select packs in Manage Packs first"
                         : "Packs are loading or loaded");
  // The selection's own clash (pack_select_check_selection): the row stays selectable, its footer
  // names the packs and Cross refuses with the fix, before anything loads.
  const int clash = !unavailable_here && idle && !nothing_selected ? g_pack_selection_clash.kind
                                                                 : PACK_CLASH_NONE;
  if (unavailable_here)
    snprintf(first->label, sizeof(first->label), "Custom packs unavailable");
  else if (nothing_selected)
    snprintf(first->label, sizeof(first->label), "Load packs (none selected)");
  else if (state == PACK_AL_IDLE && g_rpf_register_attempted)
    snprintf(first->label, sizeof(first->label), "Loaded by host commands");
  else if (clash == PACK_CLASH_NAME)
    snprintf(first->label, sizeof(first->label), "Load blocked: packs clash");
  else if (clash == PACK_CLASH_CAP)
    snprintf(first->label, sizeof(first->label), "Load blocked: over a limit");
  else if (clash == PACK_CLASH_INVALID)
    snprintf(first->label, sizeof(first->label), "Load blocked: bad pack.cfg");
  else if (idle)
    snprintf(first->label, sizeof(first->label), "Load %d selected pack%s", g_pack_selected_count,
             g_pack_selected_count == 1 ? "" : "s");
  else
    snprintf(first->label, sizeof(first->label), "%s", g_pack_al_status);
  if (unavailable_here) return count;
  const int loaded = state == PACK_AL_DONE || (state == PACK_AL_IDLE && g_rpf_index >= 0);
  const char* not_loaded = "Load the packs first";
  if (__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) {
    for (int source = 0; source < g_pack.source_count && count < max; ++source) {
      // Every pack gets its header: the menu groups each folder's rows under it.
      GtavPackMenuRow* header =
          pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_NONE, 0, 0, nullptr);
      snprintf(header->label, sizeof(header->label), "%.47s", g_pack.source_ids[source]);
      for (int i = 0; i < g_pack.spawn_count && count < max; ++i) {
        if (g_pack.spawn_source[i] != source) continue;
        GtavPackMenuRow* row = pack_menu_row(
            rows, &count,
            pack_spawn_action(g_pack.spawns[i].kind), pack_joaat(g_pack.spawns[i].model), loaded, not_loaded);
        // A weapon reads as the wheel names it: its pack's label for the meta's HumanNameHash.
        const char* weapon_name = g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_WEAPON && loaded
                                      ? pack_weapon_label(i)
                                      : nullptr;
        snprintf(row->label, sizeof(row->label), "%s",
                 weapon_name ? weapon_name : g_pack.spawns[i].text);
      }
      for (int m = 0; m < g_pack.map_count && count < max; ++m) {
        if (g_pack.map_source[m] != source || !g_pack.map_places[m].text[0]) continue;
        GtavPackMenuRow* row = pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PACK_MAP,
                                             (uint32_t)m, loaded && (g_pack_maps_finished & (1u << m)),
                                             "Map is not active yet");
        // Under the Locations folder: the place itself is the label.
        snprintf(row->label, sizeof(row->label), "%s", g_pack.map_places[m].text);
      }
      for (int p = 0; p < g_pack.place_count && count < max; ++p) {
        if (g_pack.place_source[p] != source) continue;
        GtavPackMenuRow* row = pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_TELEPORT_PACK_MAP,
                                             GTAV_PACK_PLACE_PARAM + (uint32_t)p, loaded, not_loaded);
        // Same action as a map row's place, so the menu files it under Locations too.
        snprintf(row->label, sizeof(row->label), "%s", g_pack.places[p].text);
      }
    }
  }
  // Installed packs: the selection the next load reads (this process's load, or the next launch's).
  // The list row comes first; the menu turns it into the page selector and shows one page of the
  // toggles below it (param = installed index, so host `pack-select N` needs no page).
  if (count < max) {
    GtavPackMenuRow* list = pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE,
                                          GTAV_PACK_MENU_PAGE_ROW, 1, nullptr);
    if (!g_pack_selection_valid)
      snprintf(list->label, sizeof(list->label), "packs/active is invalid");
    else
      snprintf(list->label, sizeof(list->label), "%s (%d)",
               idle ? "Installed packs" : "Installed, next launch", g_pack_installed_count);
  }
  for (int i = 0; i < g_pack_installed_count && count < max; ++i) {
    GtavPackMenuRow* row = pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE,
                                         (uint32_t)i, g_pack_selection_valid,
                                         "Fix packs/active on the host");
    snprintf(row->label, sizeof(row->label), "%.47s", g_pack_installed[i]);
    row->toggle = pack_select_is_active(g_pack_installed[i]);
  }
  // A list row (list_kinds.def CHOICE): Left/Right pick the pack, Cross uninstalls it after the
  // menu's two-press confirm. Param 0, so that confirm applies; the target lives in the worker
  // (the label names it, the list value is its position).
  const int target = pack_uninstall_target();
  if (target >= 0 && count < max) {
    const int running = pack_autoload_running();
    GtavPackMenuRow* row =
        pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_UNINSTALL_PACK, 0,
                      g_pack_selection_valid && !running,
                      running ? "Wait for the pack load to finish" : "Fix packs/active on the host");
    snprintf(row->label, sizeof(row->label), "Uninstall: %.36s", g_pack_installed[target]);
  }
  // Once packs with stock overrides are registered (the load re-points them): put those stock assets
  // back (two-press confirm, param 0). The game thread refuses while any of them is in use.
  if (g_rpf_index >= 0 && !pack_autoload_running() &&
      __atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) && g_pack.override_count > 0 &&
      count < max) {
    const int reverted = __atomic_load_n(&g_overrides_reverted, __ATOMIC_ACQUIRE);
    GtavPackMenuRow* row =
        pack_menu_row(rows, &count, GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES, 0, !reverted,
                      "Stock assets are back until the next launch");
    if (reverted)
      snprintf(row->label, sizeof(row->label), "Overrides reverted");
    else if (__atomic_load_n(&g_revert_wait_ms, __ATOMIC_ACQUIRE))  // pack_revert_tick asks again
      snprintf(row->label, sizeof(row->label), "Waiting: %.30s in use", g_revert_member);
    else
      snprintf(row->label, sizeof(row->label), "Revert overrides (%d)", g_pack.override_count);
  }
  return count;
}

// Worker: footer text for a Custom Packs row (native_bridge.cpp item_description). An installed
// pack's toggle (param = installed index): its Manage Packs line (pack_select_info). The load row:
// before a load, the selection's clash with both pack ids; after a failed load, the pack and reason.
// A pack weapon row (Weapon Browser Custom, the Weapons folder): its wheel slot, borrowed icon and
// component count (pack_weapon_footer).
extern "C" const char* gtav_features_pack_footer(uint32_t action, uint32_t param) {
  if (action == GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE && param != GTAV_PACK_MENU_PAGE_ROW)
    return pack_select_info(param);
  if (action == GTAV_NATIVE_SHELL_ACTION_GIVE_WEAPON) return pack_weapon_footer(param);
  if (action >= GTAV_NATIVE_SHELL_ACTION_CYCLE_ATTACH_SUPP &&
      action <= GTAV_NATIVE_SHELL_ACTION_REMOVE_ATTACHMENTS)
    return wattach_footer();
  if (action != GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD) return "";
  const uint32_t state = __atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE);
  if (state == PACK_AL_FAILED) return g_pack_al_detail;
  if (g_pack_al_wait_ms)
    return "A tuned car nearby (parked ones and yours count) uses parts this load moves; go away "
           "and the load goes on";
  if (state != PACK_AL_IDLE || g_rpf_register_attempted || !g_pack_selection_valid) return "";
  static char text[160];
  const PackClash& clash = g_pack_selection_clash;
  if (clash.kind == PACK_CLASH_NAME && clash.other >= 0)
    snprintf(text, sizeof(text), "%.40s and %.40s: same %.40s; deselect one",
             g_pack_selected[clash.other], g_pack_selected[clash.pack], clash.what);
  else if (const char* why = clash.kind != PACK_CLASH_BUSY
                                 ? pack_clash_text(clash, g_pack_selected, 0)
                                 : nullptr)
    snprintf(text, sizeof(text), "%s", why);
  else
    text[0] = '\0';
  return text;
}

// Save/Load Map and the object spawner (spooner.inc, spawn_entity.inc; read-only): the pack spawn
// row (vehicle, object or ped) whose model hashes to `model`, with its pack id and model name, and
// in `*ready` whether the packs are loaded (the model can be created). 0 while no pack set is
// parsed (before Load, g_pack is the selection check's scratch) or when no row declares the model.
// Any thread: the set is written only before g_pack_parsed is published; reads stay in bounds.
static int scene_pack_model(uint32_t model, const char** pack_id, const char** name, int* ready) {
  if (!model || !__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  const int count = g_pack.spawn_count < (int)GTAV_CUSTOM_PACK_SPAWN_CAP
                        ? g_pack.spawn_count
                        : (int)GTAV_CUSTOM_PACK_SPAWN_CAP;
  for (int i = 0; i < count; ++i) {
    const uint32_t kind = g_pack.spawns[i].kind;
    if (kind != GTAV_CUSTOM_PACK_SPAWN_VEHICLE && kind != GTAV_CUSTOM_PACK_SPAWN_OBJECT &&
        kind != GTAV_CUSTOM_PACK_SPAWN_PED)
      continue;
    const int source = g_pack.spawn_source[i];
    if (source >= g_pack.source_count || source >= (int)GTAV_CUSTOM_PACK_ACTIVE_MAX ||
        memchr(g_pack.spawns[i].model, 0, sizeof(g_pack.spawns[i].model)) == nullptr ||
        pack_joaat(g_pack.spawns[i].model) != model)
      continue;
    if (pack_id) *pack_id = g_pack.source_ids[source];
    if (name) *name = g_pack.spawns[i].model;
    if (ready) {
      const uint32_t state = __atomic_load_n(&g_pack_al_state, __ATOMIC_ACQUIRE);
      *ready = state == PACK_AL_DONE || (state == PACK_AL_IDLE && g_rpf_index >= 0);
    }
    return 1;
  }
  return 0;
}
