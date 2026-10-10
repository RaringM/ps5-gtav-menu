// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS && PS5 branch of the runtime pack lane.
//
// Installed packs and the active selection (Custom Packs page). Worker thread only, plain libc IO
// on the shared /data pack root, never engine devices.
// An installed pack is a <pack root>/<id>/resources/pack.cfg. The selection is the `active` file
// the next load reads: before this process's first load it applies at once; afterwards (loads are
// one-way per process) on the next game launch. A pack is added only when the resulting set still
// merges (capacities, no repeated names), so the load never meets a selection it would refuse.
// The installed list comes from listing the pack root, else from the uploader's index
// (`<pack root>/installed`, one id per line): a hardware run found the in-game directory listing
// empty while fopen of files below it worked, so every check here is an fopen. The menu pages the
// installed list (native_bridge.cpp build_custom_pack_items); tools/upload_runtime_pack.py keeps
// the index within the same cap.
//
// Uninstall (the page's "Uninstall < id >" row) mirrors `tools/upload_runtime_pack.py --uninstall`:
// the id leaves `active` (first, so no load names a hidden pack) and the `installed` index. The
// files stay; purging is host-only. Because the scan also lists the ids the directory listing
// finds, the menu also renames <id>/resources/pack.cfg to pack.cfg.uninstalled (one atomic rename,
// reversible by hand): every scan path and every load opens pack.cfg, so the pack is invisible
// whether or not the in-game opendir sees the directory. Re-uploading the pack restores it.
//
#define GTAV_PACK_INSTALLED_MAX 64
// Worker ticks between rescans of the pack root while something asks for the page rows.
#define GTAV_PACK_SCAN_TICKS 120u

static char g_pack_installed[GTAV_PACK_INSTALLED_MAX][GTAV_CUSTOM_PACK_ID_MAX];
static int g_pack_installed_count;
static char g_pack_selected[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
static int g_pack_selected_count;
static int g_pack_selection_valid;  // `active` parsed (or is absent: nothing selected)
static uint32_t g_pack_worker_ticks;
static uint32_t g_pack_scan_at;
static int g_pack_scanned;

// Whole small file into `buffer` (NUL-terminated). Returns the byte count, or -1.
static int64_t pack_select_read(const char* path, char* buffer, size_t capacity) {
  errno = 0;
  FILE* fp = fopen(path, "rb");
  if (!fp) return -1;
  const size_t got = fread(buffer, 1, capacity - 1u, fp);
  const int complete = !ferror(fp) && (feof(fp) || fgetc(fp) == EOF) && !ferror(fp);
  fclose(fp);
  if (!complete) return -1;
  buffer[got] = '\0';
  return (int64_t)got;
}

static void pack_select_read_active() {
  char text[GTAV_CUSTOM_PACK_ACTIVE_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  char path[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), nullptr, "/active");
  const int64_t size = pack_select_read(path, text, sizeof(text));
  g_pack_selected_count = 0;
  memset(g_pack_selected, 0, sizeof(g_pack_selected));
  if (size < 0) {
    g_pack_selection_valid = errno == ENOENT;  // absent: nothing selected; unreadable: invalid
    return;
  }
  g_pack_selection_valid =
      gtav_custom_pack_parse_active(text, (size_t)size, g_pack_selected, &g_pack_selected_count) == 0;
  if (!g_pack_selection_valid) g_pack_selected_count = 0;
}

static int pack_select_installed(const char* id) {
  char path[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), id, "/resources/pack.cfg");
  if (!path[0]) return 0;
  FILE* fp = fopen(path, "rb");
  if (!fp) return 0;
  fclose(fp);
  return 1;
}

// Add `id` to the installed list in sorted order (insertion sort keeps rescans stable). Returns 1
// when added.
static int pack_select_add_installed(const char* id) {
  if (g_pack_installed_count >= GTAV_PACK_INSTALLED_MAX || !gtav_custom_pack_id_valid(id)) return 0;
  // Repeats first: the directory and the index usually list the same ids, so skip their fopen.
  for (int i = 0; i < g_pack_installed_count; ++i)
    if (strcmp(g_pack_installed[i], id) == 0) return 0;
  if (!pack_select_installed(id)) return 0;
  int at = g_pack_installed_count++;
  while (at > 0 && strcmp(g_pack_installed[at - 1], id) > 0) {
    memcpy(g_pack_installed[at], g_pack_installed[at - 1], GTAV_CUSTOM_PACK_ID_MAX);
    --at;
  }
  snprintf(g_pack_installed[at], GTAV_CUSTOM_PACK_ID_MAX, "%s", id);
  return 1;
}

static int pack_autoload_running();  // custom_pack_autoload.inc
static void pack_select_check_selection();

// Rescan the pack root (sorted ids) and the active file, at most every GTAV_PACK_SCAN_TICKS.
static void pack_select_scan(int force) {
  if (!force && g_pack_scanned && g_pack_worker_ticks - g_pack_scan_at < GTAV_PACK_SCAN_TICKS) return;
  g_pack_scanned = 1;
  g_pack_scan_at = g_pack_worker_ticks;
  g_pack_installed_count = 0;
  if (!gtav_custom_game_root_available()) {
    g_pack_selected_count = 0;
    memset(g_pack_selected, 0, sizeof(g_pack_selected));
    g_pack_selection_valid = 0;
    return;
  }
  int dir_errno = 0;
  uint32_t dir_entries = 0;
  char path[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), nullptr, "");
  if (DIR* dir = opendir(path)) {
    while (struct dirent* entry = readdir(dir)) {
      ++dir_entries;
      pack_select_add_installed(entry->d_name);
    }
    closedir(dir);
  } else {
    dir_errno = errno;
  }
  const int from_dir = g_pack_installed_count;
  // One id (at most GTAV_CUSTOM_PACK_ID_MAX - 1 chars) plus '\n' per line; static, ~4 KiB.
  static char index[GTAV_PACK_INSTALLED_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  pack_root_path(path, sizeof(path), nullptr, "/installed");
  const int64_t index_size = pack_select_read(path, index, sizeof(index));
  for (char* line = index; index_size > 0 && line && *line;) {
    char* newline = strchr(line, '\n');
    if (newline) *newline = '\0';
    pack_select_add_installed(line);
    line = newline ? newline + 1 : nullptr;
  }
  pack_select_read_active();
  pack_select_check_selection();
  // Logged when the result changes, so a scan that finds nothing says why; also to the klog
  // (pack_world_note), so a run can confirm the installed count after the status ring wrapped.
  static int s_logged_dir = -1, s_logged_total = -1;
  if (s_logged_dir != from_dir || s_logged_total != g_pack_installed_count) {
    s_logged_dir = from_dir;
    s_logged_total = g_pack_installed_count;
    pack_world_note("pack scan dir_errno=%d entries=%u from_dir=%d index=%lld installed=%d "
                    "selected=%d valid=%d",
                    dir_errno, (unsigned)dir_entries, from_dir, (long long)index_size,
                    g_pack_installed_count, g_pack_selected_count, g_pack_selection_valid);
  }
}

static int pack_select_is_active(const char* id) {
  for (int i = 0; i < g_pack_selected_count; ++i)
    if (strcmp(g_pack_selected[i], id) == 0) return 1;
  return 0;
}

// Why a selection would not load (pack_select_clash), for the menu texts.
enum {
  PACK_CLASH_NONE = 0,
  PACK_CLASH_NAME = GTAV_CUSTOM_PACK_CLASH_NAME,  // pack and other hold the same name (what)
  PACK_CLASH_CAP = GTAV_CUSTOM_PACK_CLASH_CAP,    // adding pack overflows a merged cap (what)
  PACK_CLASH_INVALID = 3,                         // pack's pack.cfg is missing or invalid
  PACK_CLASH_BUSY = 4,                            // a load holds the descriptor buffer
};
struct PackClash {
  int kind;
  int pack;   // index in the checked ids of the pack that does not merge
  int other;  // PACK_CLASH_NAME: index of the earlier pack holding the same name, else -1
  char what[72];  // "map gm_x.pmap", "too many maps"
};

// One descriptor buffer for the selection check and the Manage Packs line (worker thread only).
static char g_pack_select_text[GTAV_CUSTOM_PACK_DESCRIPTOR_MAX + 1];

// Read and parse pack `id`'s descriptor into `out`. Returns 1 when it parses and names `id`.
static int pack_select_parse(const char* id, GtavCustomPack* out) {
  char path[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), id, "/resources/pack.cfg");
  const int64_t size = pack_select_read(path, g_pack_select_text, sizeof(g_pack_select_text));
  return size > 0 && gtav_custom_pack_parse(g_pack_select_text, (size_t)size, out) == 0 &&
         strcmp(out->pack_id, id) == 0;
}

// Parse and merge every pack of `ids` the way the load will, each descriptor parsed into the
// shared g_pack_scratch (custom_rpf.inc), which the caller holds, and merged into g_pack, which no
// one uses before the load has parsed the selection (pack_load_descriptor merges into it while it
// holds the same scratch, and marks it parsed before releasing that). On a repeated name the
// earlier pack holding it is found by re-checking the failing pack against each earlier one alone
// (the merged set is not needed any more). Returns the PackClash kind (*out filled).
static int pack_select_clash_locked(
    const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX], int count,
    PackClash* out) {
  GtavCustomPack& set = g_pack;
  GtavCustomPack& one = g_pack_scratch;
  memset(out, 0, sizeof(*out));
  out->other = -1;
  // Loaded (or being loaded by host commands): g_pack is the live set. Only each pack's own
  // pack.cfg is checked; the next launch's load checks the whole selection before it starts.
  const int live = __atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) || g_rpf_register_attempted;
  if (!live) memset(&set, 0, sizeof(set));
  for (int i = 0; i < count; ++i) {
    out->pack = i;
    if (!pack_select_parse(ids[i], &one)) return out->kind = PACK_CLASH_INVALID;
    if (live) continue;
    const int clash = gtav_custom_pack_clash(&set, &one, out->what, sizeof(out->what));
    if (clash == GTAV_CUSTOM_PACK_CLASH_CAP) return out->kind = PACK_CLASH_CAP;
    if (clash == GTAV_CUSTOM_PACK_CLASH_NAME) {
      for (int j = 0; j < i && out->other < 0; ++j)
        if (pack_select_parse(ids[j], &set) &&
            gtav_custom_pack_clash(&set, &one, out->what, sizeof(out->what)) ==
                GTAV_CUSTOM_PACK_CLASH_NAME)
          out->other = j;
      return out->kind = PACK_CLASH_NAME;
    }
    if (clash != GTAV_CUSTOM_PACK_CLASH_NONE || gtav_custom_pack_merge(&set, &one) != 0)
      return out->kind = PACK_CLASH_INVALID;
  }
  return PACK_CLASH_NONE;
}

// pack_select_clash_locked under the descriptor buffer; PACK_CLASH_BUSY while a load holds it.
static int pack_select_clash(const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                             int count, PackClash* out) {
  if (!pack_scratch_take(PACK_SCRATCH_WORKER)) {
    memset(out, 0, sizeof(*out));
    out->other = -1;
    return out->kind = PACK_CLASH_BUSY;
  }
  const int kind = pack_select_clash_locked(ids, count, out);
  pack_scratch_release();
  return kind;
}

// Toast for a refused selection: names the pack to deselect (`adding`: the selected pack the new
// one clashes with; otherwise the later of the two selected ones) and the clash. Fits the toast's
// 80 bytes with its status glyph; nullptr when nothing clashes.
static const char* pack_clash_text(
    const PackClash& clash, const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
    int adding) {
  static char text[GTAV_FEATURE_MESSAGE_LEN];
  switch (clash.kind) {
    case PACK_CLASH_NAME: {
      const int named = adding && clash.other >= 0 ? clash.other : clash.pack;
      snprintf(text, sizeof(text), "Deselect %.36s: same %.32s", ids[named], clash.what);
      return text;
    }
    case PACK_CLASH_CAP:
      snprintf(text, sizeof(text), "Limit reached: %.40s; deselect a pack", clash.what);
      return text;
    case PACK_CLASH_INVALID:
      snprintf(text, sizeof(text), "%.40s: pack.cfg invalid; reinstall it", ids[clash.pack]);
      return text;
    case PACK_CLASH_BUSY:
      return "Pack load is running; retry";
    default:
      return nullptr;
  }
}

// The current selection's own clash, checked at each rescan before any load (custom_pack_autoload
// shows it on the load row and in Manage Packs; a host-written `active` can hold packs that clash).
static PackClash g_pack_selection_clash;
static uint32_t g_pack_scan_serial;  // bumped by every rescan: the Manage Packs line re-reads

// Nothing of this process's one load has started: the selection can still change what loads.
static int pack_select_before_load() {
  return !g_rpf_register_attempted && !__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE) &&
         !pack_autoload_running();
}

// Rescan step: check the selection (up to 8 descriptor reads) before any load, keep the last
// result while a load holds the buffer, and log each new result once (pack-notes).
static void pack_select_check_selection() {
  ++g_pack_scan_serial;
  PackClash clash = {};
  clash.other = -1;
  if (g_pack_selection_valid && g_pack_selected_count > 0 && pack_select_before_load() &&
      pack_select_clash(g_pack_selected, g_pack_selected_count, &clash) == PACK_CLASH_BUSY)
    return;
  const int changed = clash.kind != g_pack_selection_clash.kind ||
                      clash.pack != g_pack_selection_clash.pack ||
                      clash.other != g_pack_selection_clash.other ||
                      strcmp(clash.what, g_pack_selection_clash.what) != 0;
  g_pack_selection_clash = clash;
  if (!changed) return;
  if (clash.kind == PACK_CLASH_NONE)
    pack_world_note("pack selection ok selected=%d", g_pack_selected_count);
  else
    pack_world_note("pack selection clash kind=%d pack=%.40s other=%.40s what=%.40s", clash.kind,
                    g_pack_selected[clash.pack],
                    clash.other >= 0 ? g_pack_selected[clash.other] : "-", clash.what);
}

// Replace the control file `path` with `body`: write `temp`, then rename it over `path`.
static int pack_select_publish(const char* temp, const char* path, const char* body,
                               size_t length) {
  FILE* fp = fopen(temp, "wb");
  if (!fp) return -1;
  int bad = fwrite(body, 1, length, fp) != length;
  if (fflush(fp) != 0 || ferror(fp)) bad = 1;
  if (fclose(fp) != 0) bad = 1;
  if (bad || rename(temp, path) != 0) {
    unlink(temp);
    return -1;
  }
  return 0;
}

// Publish the selection: temp file then rename, or no `active` file at all when empty.
static int pack_select_write(const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                             int count) {
  char path[GTAV_PACK_PATH_MAX];
  char temp[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), nullptr, "/active");
  pack_root_path(temp, sizeof(temp), nullptr, "/active.tmp");
  if (!path[0] || !temp[0]) return -1;
  if (count == 0) return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
  char body[GTAV_CUSTOM_PACK_ACTIVE_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  const int length = gtav_custom_pack_render_active(ids, count, body, sizeof(body));
  if (length <= 0) return -1;
  return pack_select_publish(temp, path, body, (size_t)length);
}

// Worker-direct action: add or remove installed pack `param` (installed order, across pages) from
// the selection. Cross on the page selector row (GTAV_PACK_MENU_PAGE_ROW) only says how to page.
// Refusals name the pack and the fix and fit a toast (GTAV_FEATURE_MESSAGE_LEN with the status
// glyph); the klog gets the clash with both ids.
static uint32_t toggle_pack_active(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (param == GTAV_PACK_MENU_PAGE_ROW) return unavailable(action, "Left/Right turns the page");
  pack_select_scan(1);
  if (param >= (uint32_t)g_pack_installed_count)
    return unavailable(action, "Pack list changed; press again");
  if (!g_pack_selection_valid)
    return unavailable(action, "packs/active is invalid; fix it on the host");
  char id[GTAV_CUSTOM_PACK_ID_MAX];
  snprintf(id, sizeof(id), "%s", g_pack_installed[param]);
  char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
  int count = g_pack_selected_count;
  memcpy(ids, g_pack_selected, sizeof(ids));
  const int added = gtav_custom_pack_active_toggle(ids, &count, id);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  if (added < 0) {
    snprintf(message, sizeof(message), "%d packs selected; deselect one first",
             GTAV_CUSTOM_PACK_ACTIVE_MAX);
    return unavailable(action, message);
  }
  if (added) {
    PackClash clash;
    if (pack_select_clash(ids, count, &clash) != PACK_CLASH_NONE) {
      pack_world_note("pack select refused id=%.40s kind=%d other=%.40s what=%.40s", id, clash.kind,
                      clash.other >= 0 ? ids[clash.other] : "-", clash.what);
      return unavailable(action, pack_clash_text(clash, ids, 1));
    }
  }
  if (pack_select_write(ids, count) != 0)
    return failed(action, "Could not save the selection; retry");
  pack_select_scan(1);
  const int loaded_here = g_rpf_register_attempted || g_pack_parsed;
  const char* note = loaded_here ? " (next launch)" : "";
  snprintf(message, sizeof(message), "%s %.40s%s", added ? "Selected" : "Deselected", id, note);
  return ok(action, message);
}

// ---- Uninstall (Custom Packs page "Uninstall < id >", a pick-then-apply list row) ----

// The pack the uninstall row names: Left/Right step it through the installed list, Cross (after
// the menu's two-press confirm) uninstalls it. Worker thread only.
static char g_pack_uninstall_id[GTAV_CUSTOM_PACK_ID_MAX];

static int pack_select_installed_index(const char* id) {
  for (int i = 0; i < g_pack_installed_count; ++i)
    if (strcmp(g_pack_installed[i], id) == 0) return i;
  return -1;
}

// The uninstall target, following the installed list: a target that left it becomes the first
// installed pack. Returns its index, or -1 when nothing is installed.
static int pack_uninstall_target() {
  if (g_pack_installed_count == 0) return -1;
  const int at = pack_select_installed_index(g_pack_uninstall_id);
  if (at >= 0) return at;
  snprintf(g_pack_uninstall_id, sizeof(g_pack_uninstall_id), "%s", g_pack_installed[0]);
  return 0;
}

// The uninstall row's "< n/m >" (its label, built by gtav_features_pack_menu, names the pack: a pack
// id does not fit the menu's list values). Pushed after every dispatch; the bridge rebuilds the page
// when it changes, so the label follows a step at once. "" until the page has scanned once.
static const char* pack_uninstall_value_label() {
  static char text[16];
  const int at = g_pack_scanned ? pack_uninstall_target() : -1;
  if (at < 0) return "";
  snprintf(text, sizeof(text), "%d/%d", at + 1, g_pack_installed_count);
  return text;
}

// Drop `id` from the installed index (temp file then rename; the file is deleted when nothing is
// left, as the uploader does). *listed says whether the index named it. Returns 0 or -1.
static int pack_select_unlist(const char* id, int* listed) {
  static char index[GTAV_PACK_INSTALLED_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  static char body[GTAV_PACK_INSTALLED_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  char path[GTAV_PACK_PATH_MAX];
  char temp[GTAV_PACK_PATH_MAX];
  *listed = 0;
  pack_root_path(path, sizeof(path), nullptr, "/installed");
  pack_root_path(temp, sizeof(temp), nullptr, "/installed.tmp");
  if (!path[0] || !temp[0]) return -1;
  const int64_t size = pack_select_read(path, index, sizeof(index));
  if (size < 0) return errno == ENOENT ? 0 : -1;
  size_t length = 0;
  for (char* line = index; *line;) {
    char* newline = strchr(line, '\n');
    if (newline) *newline = '\0';
    if (strcmp(line, id) == 0) {
      *listed = 1;
    } else if (gtav_custom_pack_id_valid(line)) {
      length += (size_t)snprintf(body + length, sizeof(body) - length, "%s\n", line);
      if (length >= sizeof(body)) return -1;
    }
    if (!newline) break;
    line = newline + 1;
  }
  if (!*listed) return 0;
  if (length == 0) return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
  return pack_select_publish(temp, path, body, length);
}

// Whether this GTA session already loaded pack `id` (it stays loaded until a restart).
static int pack_uninstall_loaded_here(const char* id) {
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.source_count; ++i)
    if (strcmp(g_pack.source_ids[i], id) == 0) return 1;
  return 0;
}

// Worker-direct action. param 1 / 2 (Right / Left) step the target; 0 (Cross, confirmed by the
// menu) uninstalls it: `active` first (when selected), then the pack.cfg rename, then the index.
// The result is checked by a rescan and logged to the klog either way.
static uint32_t uninstall_pack(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_UNINSTALL_PACK;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  if (param == 1u || param == 2u) {
    if (g_pack_installed_count == 0) return unavailable(action, "No installed packs");
    const int at = pack_select_installed_index(g_pack_uninstall_id);
    const int n = g_pack_installed_count;
    const int next = at < 0 ? 0 : (param == 2u ? at + n - 1 : at + 1) % n;
    snprintf(g_pack_uninstall_id, sizeof(g_pack_uninstall_id), "%s", g_pack_installed[next]);
    snprintf(message, sizeof(message), "Uninstall target: %.48s", g_pack_uninstall_id);
    return ok(action, message);
  }
  char id[GTAV_CUSTOM_PACK_ID_MAX];
  snprintf(id, sizeof(id), "%s", g_pack_uninstall_id);
  if (pack_autoload_running()) {
    pack_world_note("pack uninstall refused id=%.40s: a pack load is running", id);
    return unavailable(action, "Wait for the pack load to finish");
  }
  pack_select_scan(1);
  const int at = id[0] ? pack_select_installed_index(id) : -1;
  if (at < 0) {
    pack_world_note("pack uninstall refused id=%.40s: not installed", id);
    return unavailable(action, "Pack list changed; pick the pack again");
  }
  if (!g_pack_selection_valid) {
    pack_world_note("pack uninstall refused id=%.40s: packs/active invalid", id);
    return unavailable(action, "packs/active is invalid; fix it on the host");
  }
  // 1. Deselect: a load must never name a pack whose pack.cfg is hidden.
  const int selected = pack_select_is_active(id);
  if (selected) {
    char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
    int count = g_pack_selected_count;
    memcpy(ids, g_pack_selected, sizeof(ids));
    if (gtav_custom_pack_active_toggle(ids, &count, id) != 0 ||
        pack_select_write(ids, count) != 0) {
      pack_world_note("pack uninstall FAILED id=%.40s: active write errno=%d", id, errno);
      return failed(action, "Could not save the selection; retry");
    }
  }
  // 2. Hide: rename pack.cfg so neither the directory listing nor the index lists the pack.
  char cfg[GTAV_PACK_PATH_MAX];
  char hidden[GTAV_PACK_PATH_MAX];
  pack_root_path(cfg, sizeof(cfg), id, "/resources/pack.cfg");
  pack_root_path(hidden, sizeof(hidden), id, "/resources/pack.cfg.uninstalled");
  const int rename_errno = !cfg[0] || !hidden[0]     ? ENAMETOOLONG
                           : rename(cfg, hidden) == 0 ? 0
                                                      : errno;
  // 3. Unlist: drop the id from the installed index.
  int listed = 0;
  const int index_ok = pack_select_unlist(id, &listed) == 0;
  const int index_errno = index_ok ? 0 : (errno ? errno : -1);
  const int before = g_pack_installed_count;
  pack_select_scan(1);
  const int gone = pack_select_installed_index(id) < 0 && !pack_select_is_active(id);
  const int loaded = pack_uninstall_loaded_here(id);
  // Two klog lines: the GTAV_FEATURE_MESSAGE_LEN note buffer would truncate one.
  pack_world_note("pack uninstall steps desel=%d ren_errno=%d idx_errno=%d listed=%d "
                  "installed=%d->%d",
                  selected, rename_errno, index_errno, listed, before, g_pack_installed_count);
  pack_world_note("pack uninstall %s id=%.36s loaded_here=%d", gone ? "ok" : "FAILED", id, loaded);
  if (!gone) {
    // The errnos are in the klog line above.
    snprintf(message, sizeof(message), "%.40s still listed; uninstall on the host", id);
    return failed(action, message);
  }
  // The row moves on to the pack that took this one's place in the list.
  if (g_pack_installed_count > 0)
    snprintf(g_pack_uninstall_id, sizeof(g_pack_uninstall_id), "%s",
             g_pack_installed[at < g_pack_installed_count ? at : g_pack_installed_count - 1]);
  snprintf(message, sizeof(message), "Uninstalled %.40s%s", id,
           loaded ? " (loaded until GTA restarts)" : " (files kept)");
  return ok(action, message);
}

// ---- Manage Packs line (the footer of an installed pack's toggle row) ----

// The clash pack `id` takes part in, as a footer prefix ("" when none): `clash` was checked over
// `ids` (the selection, or the selection plus `id`).
static void pack_info_clash(char* out, size_t size, const PackClash& clash,
                            const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                            const char* id) {
  out[0] = '\0';
  const int is_pack = clash.kind != PACK_CLASH_NONE && strcmp(ids[clash.pack], id) == 0;
  const int is_other = clash.other >= 0 && strcmp(ids[clash.other], id) == 0;
  if (clash.kind == PACK_CLASH_NAME && (is_pack || is_other))
    snprintf(out, size, "Clashes with %.40s: same %.40s. ",
             is_pack ? (clash.other >= 0 ? ids[clash.other] : "a selected pack") : ids[clash.pack],
             clash.what);
  else if (clash.kind == PACK_CLASH_CAP && is_pack)
    snprintf(out, size, "Over the limit with the others: %.40s. ", clash.what);
  else if (clash.kind == PACK_CLASH_INVALID && is_pack)
    snprintf(out, size, "pack.cfg invalid; reinstall it. ");
}

// Footer for installed pack `index` (Manage Packs): before any load, the clash that selecting it
// (or keeping it selected) causes; then gtav_custom_pack_about's description, version, author
// and what the pack adds. Recomputed after each rescan or for another row: one descriptor read,
// plus the selection check for an unselected pack. "" for an index out of range.
static const char* pack_select_info(uint32_t index) {
  static char info[256];
  static char cached[GTAV_CUSTOM_PACK_ID_MAX];
  static uint32_t cached_serial = UINT32_MAX;
  if (index >= (uint32_t)g_pack_installed_count) return "";
  const char* id = g_pack_installed[index];
  if (cached_serial == g_pack_scan_serial && strcmp(cached, id) == 0) return info;
  cached_serial = g_pack_scan_serial;
  snprintf(cached, sizeof(cached), "%s", id);
  char about[192];
  char path[GTAV_PACK_PATH_MAX];
  pack_root_path(path, sizeof(path), id, "/resources/pack.cfg");
  const int64_t size = pack_select_read(path, g_pack_select_text, sizeof(g_pack_select_text));
  if (size <= 0 ||
      gtav_custom_pack_about(g_pack_select_text, (size_t)size, about, sizeof(about)) != 0)
    snprintf(about, sizeof(about), "pack.cfg unreadable; reinstall it");
  char prefix[112] = "";
  if (pack_select_before_load() && g_pack_selection_valid) {
    if (pack_select_is_active(id)) {
      pack_info_clash(prefix, sizeof(prefix), g_pack_selection_clash, g_pack_selected, id);
    } else if (g_pack_selected_count < GTAV_CUSTOM_PACK_ACTIVE_MAX) {
      char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
      int count = g_pack_selected_count;
      memcpy(ids, g_pack_selected, sizeof(ids));
      PackClash clash;
      if (gtav_custom_pack_active_toggle(ids, &count, id) == 1 &&
          pack_select_clash(ids, count, &clash) != PACK_CLASH_BUSY)
        pack_info_clash(prefix, sizeof(prefix), clash, ids, id);
    }
  }
  snprintf(info, sizeof(info), "%s%s", prefix, about);
  return info;
}
