// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS && PS5 branch of the runtime pack lane.
//
// Pack `hide` rows: hide the stock map entities of one model inside a sphere for the session
// (CREATE_MODEL_HIDE, bSurviveMapReload set), e.g. where a pack map places a building over a stock
// prop. The one-press load applies every row once the pack's maps are active; the host
// `menu-ctl.sh pack-unhide` undoes the applied rows (REMOVE_MODEL_HIDE). Both natives run on the
// game thread only: after a successful add the handler reads the calling script thread's context
// without a null test. Each result is a pack_world_note line (`menu-ctl.sh pack-notes`).

// Per merged hide row (bit = row). uint64_t: GTAV_CUSTOM_PACK_HIDE_CAP rows.
static uint64_t g_pack_hides_applied;
static uint64_t g_pack_hides_failed;  // a refused row: the one-press load stops on it
static_assert(GTAV_CUSTOM_PACK_HIDE_CAP <= 64, "hide row masks are uint64_t");

// Every merged hide row's bit (64 rows fill the mask; a 64-bit shift would be undefined).
static uint64_t pack_hide_rows_mask() {
  return g_pack.hide_count >= 64 ? ~0ull : (1ull << g_pack.hide_count) - 1ull;
}
// apply_pack_hide param that undoes every applied row (host `menu-ctl.sh pack-unhide`).
#define GTAV_PACK_HIDE_REMOVE_ALL 0xffffu

static uint32_t remove_pack_hides() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PACK_HIDE;
  if (!g_n.remove_model_hide) {
    pack_world_note("pack unhide refused: REMOVE_MODEL_HIDE is not bound");
    return unavailable(action, "REMOVE_MODEL_HIDE is not bound");
  }
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  int removed = 0;
  for (int i = 0; i < g_pack.hide_count; ++i) {
    if (!(g_pack_hides_applied & (1ull << i))) continue;
    const uint32_t hash = pack_joaat(g_pack.hides[i].model);
    invoke_void(g_n.remove_model_hide, (float)g_pack.hides[i].x, (float)g_pack.hides[i].y,
                (float)g_pack.hides[i].z, (float)g_pack.hides[i].radius, hash, 0);
    g_pack_hides_applied &= ~(1ull << i);
    removed++;
    pack_world_note("pack unhide %d %s hash=0x%08x at (%d,%d,%d) r=%u removed", i,
                    g_pack.hides[i].model, (unsigned)hash, (int)g_pack.hides[i].x,
                    (int)g_pack.hides[i].y, (int)g_pack.hides[i].z,
                    (unsigned)g_pack.hides[i].radius);
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack unhide removed=%d", removed);
  return ok(action, message);
}

// param = merged hide row, or GTAV_PACK_HIDE_REMOVE_ALL to undo every applied row.
static uint32_t apply_pack_hide(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PACK_HIDE;
  if (param == GTAV_PACK_HIDE_REMOVE_ALL) return remove_pack_hides();
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) || g_rpf_index < 0)
    return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.hide_count)
    return unavailable(action, "pack hide row out of range");
  if (g_pack_hides_applied & (1ull << param))
    return unavailable(action, "pack hide row already applied");
  const int row = (int)param;
  if (!g_n.create_model_hide) {
    // Fail closed: without the reviewed native the stock entities stay and the load stops here.
    g_pack_hides_failed |= 1ull << param;
    pack_world_note("pack hide %d %s refused: CREATE_MODEL_HIDE is not bound", row,
                    g_pack.hides[row].model);
    return unavailable(action, "CREATE_MODEL_HIDE is not bound");
  }
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  const uint32_t hash = pack_joaat(g_pack.hides[row].model);
  invoke_void(g_n.create_model_hide, (float)g_pack.hides[row].x, (float)g_pack.hides[row].y,
              (float)g_pack.hides[row].z, (float)g_pack.hides[row].radius, hash, 1);
  g_pack_hides_applied |= 1ull << param;
  pack_world_note("pack hide %d %s hash=0x%08x at (%d,%d,%d) r=%u applied", row,
                  g_pack.hides[row].model, (unsigned)hash, (int)g_pack.hides[row].x,
                  (int)g_pack.hides[row].y, (int)g_pack.hides[row].z,
                  (unsigned)g_pack.hides[row].radius);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack hide %d %s applied", row, g_pack.hides[row].model);
  return ok(action, message);
}
