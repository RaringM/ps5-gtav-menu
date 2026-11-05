// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS branch of the runtime pack lane: the engine lane or the engine-less shell.
//
// Installed packs and the active selection (Custom Packs page). Worker thread only, plain libc IO
// on the shared /data pack root, never engine devices. The engine-less shell skips the store
// census and the store-headroom check (they read the engine's stores).
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
#define GTAV_PACK_INSTALLED_MAX 128
static int pack_load_failed_clean();  // custom_pack_autoload.inc
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static int pack_append_possible();  // custom_pack_autoload.inc
#endif

// Worker ticks between rescans of the pack root while something asks for the page rows.
#define GTAV_PACK_SCAN_TICKS 120u

static char g_pack_installed[GTAV_PACK_INSTALLED_MAX][GTAV_CUSTOM_PACK_ID_MAX];
// The installed index file's text (one id plus '\n' per line), read by the scan and by the
// uninstall's rewrite; both run on the worker thread, never nested, so they share it (~8 KiB).
static char g_pack_index_text[GTAV_PACK_INSTALLED_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
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
  char* index = g_pack_index_text;
  pack_root_path(path, sizeof(path), nullptr, "/installed");
  const int64_t index_size = pack_select_read(path, index, sizeof(g_pack_index_text));
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
  PACK_CLASH_STORE = 5,  // the packs (bits) need a game store too full to load them (what)
};
struct PackClash {
  int kind;
  int pack;   // index in the checked ids of the pack that does not merge
  int other;  // PACK_CLASH_NAME: index of the earlier pack holding the same name, else -1
  uint32_t packs;  // PACK_CLASH_STORE: bit i = ids[i] needs the store
  // "map gm_x.pmap", "too many maps"; PACK_CLASH_STORE: the store gate's refusal
  // ("pack metadata store headroom gate refused free=1 need=8")
  char what[GTAV_FEATURE_MESSAGE_LEN];
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

#if GTAV_MENU_CUSTOM_PACK_ENGINE
// Model and file tables the data rows add to, after the member stores in the selection check
// (custom_pack_checks.c kStores names them in this order).
enum {
  PACK_TABLE_VEHICLE = 0,  // vehicle model infos (VEHICLE_METADATA <InitDatas>)
  PACK_TABLE_PED,          // ped model infos (PED_METADATA <InitDatas>)
  PACK_TABLE_WMODEL,       // weapon model infos (WEAPON_METADATA <InitDatas>)
  PACK_TABLE_WCOMP,        // weapon component infos (WEAPONCOMPONENTSINFO <Infos>, 470 fixed)
  PACK_TABLE_WINFO,        // weapon-info file blobs (one per WEAPONINFO row)
  PACK_TABLE_COUNT,
};
static const char* const kPackTableNames[PACK_TABLE_COUNT] = {"vehicle", "pedmodel", "wmodel",
                                                              "wcomp", "winfo"};
static const char* const kPackTableShort[PACK_TABLE_COUNT] = {"vehicle", "ped", "weapon model",
                                                              "component", "weapon file"};
static const char* const kPackStoreShort[PACK_STORE_COUNT] = {
    "frag", "drawable", "txd", "typ", "map", "collision", "ped meta", "ped dwd", "particle"};

// What one selected pack adds: per member store (its archives' counted members plus its rows'
// by-name adds) and per table. `unknown`: an archive could not be counted (keyed or unreadable), so
// its stores take the old fixed allowance.
struct PackNeeds {
  char id[GTAV_CUSTOM_PACK_ID_MAX];
  uint32_t stores[PACK_STORE_COUNT];
  uint32_t tables[PACK_TABLE_COUNT];
  int unknown;
};
// The last selection check's packs (worker thread only): Manage Packs footers name their needs.
static PackNeeds g_pack_needs[GTAV_CUSTOM_PACK_ACTIVE_MAX];
static int g_pack_needs_count;

// Console-file counts cached by archive / data file identity (size + SHA-256 prefix), so the
// page's rescans do not re-read the pack files (worker thread only). One slot per archive and data
// row of a full selection, so a check never evicts its own entries; only definite results are kept
// (a read that failed is retried on the next check).
struct PackCountCache {
  uint64_t size;
  uint32_t tag;  // 0: an archive table; else the data row's list name (pack_count_tag)
  uint8_t sha[8];
  uint8_t valid;
  uint8_t known;
  uint32_t bound;  // data files: the list bound
  GtavCustomPackTocCounts counts;
};
static PackCountCache g_pack_count_cache[GTAV_CUSTOM_PACK_ARCHIVE_CAP + GTAV_CUSTOM_PACK_DATA_CAP];
static uint32_t g_pack_count_cache_next;

// The same bytes counted as an archive and as a data row, or as rows of different lists, are
// separate entries: the key includes what was counted.
static uint32_t pack_count_tag(const char* list) {
  uint32_t hash = 2166136261u;  // FNV-1a 32
  for (const char* c = list; *c; ++c) hash = (hash ^ (uint8_t)*c) * 16777619u;
  return hash | 1u;
}

static PackCountCache* pack_count_cached(uint64_t size, const uint8_t* sha, uint32_t tag) {
  for (PackCountCache& entry : g_pack_count_cache)
    if (entry.valid && entry.size == size && entry.tag == tag &&
        memcmp(entry.sha, sha, sizeof(entry.sha)) == 0)
      return &entry;
  return nullptr;
}

static PackCountCache* pack_count_slot(uint64_t size, const uint8_t* sha, uint32_t tag) {
  PackCountCache& entry =
      g_pack_count_cache[g_pack_count_cache_next++ % (sizeof(g_pack_count_cache) /
                                                      sizeof(g_pack_count_cache[0]))];
  memset(&entry, 0, sizeof(entry));
  entry.size = size;
  entry.tag = tag;
  memcpy(entry.sha, sha, sizeof(entry.sha));
  entry.valid = 1;
  return &entry;
}

// <pack root>/<id>/resources/<file> in `path`; 0 when it does not fit.
static int pack_select_file_path(const char* id, const char* file, char* path, size_t size) {
  char dir[GTAV_PACK_PATH_MAX];
  pack_root_path(dir, sizeof(dir), id, "/resources");
  const int n = snprintf(path, size, "%s/%s", dir, file);
  return dir[0] && n > 0 && (size_t)n < size;
}

// Read `size` bytes of <pack root>/<id>/resources/<file> at `offset` into `out` (worker, plain libc
// IO on the shared /data). Returns 1 when every byte was read.
static int pack_select_read_part(const char* id, const char* file, uint64_t offset, void* out,
                                 uint64_t size) {
  char path[GTAV_PACK_PATH_MAX];
  if (!pack_select_file_path(id, file, path, sizeof(path))) return 0;
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  const int ok =
      fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, (size_t)size, f) == (size_t)size;
  fclose(f);
  return ok;
}

// Archive `i` of the merged selection, counted from its console file into `out` (a plaintext
// table only; keyed tables need the game's key, which the registration gate applies). The held
// g_pack_scratch is the read buffer (the merge no longer needs it). Returns 1 when counted. A read
// that fails is not cached: the next check reads the file again.
static int pack_select_toc(int i, GtavCustomPackTocCounts* out) {
  const char* archive = nullptr;
  uint64_t size = 0;
  const uint8_t* sha = nullptr;
  pack_archive_ref(i, &archive, &size, &sha);
  if (const PackCountCache* hit = pack_count_cached(size, sha, 0u)) {
    *out = hit->counts;
    return hit->known;
  }
  const char* id = g_pack.source_ids[pack_archive_source(i)];
  uint32_t header[4] = {};
  if (!pack_select_read_part(id, archive, 0, header, sizeof(header))) return 0;
  GtavCustomPackTocCounts counts;
  memset(&counts, 0, sizeof(counts));
  int known = 0;
  const uint64_t bytes = (uint64_t)header[1] * 16u + header[2];
  if (header[0] == kRpfMagic && header[1] >= 2u && (header[2] >> 28) == 0u &&
      header[3] == GTAV_RPF_TAG_OPEN && 16u + bytes <= size && bytes <= sizeof(g_pack_scratch)) {
    uint8_t* buffer = (uint8_t*)&g_pack_scratch;
    if (!pack_select_read_part(id, archive, 16, buffer, bytes)) return 0;
    known = gtav_custom_pack_toc_counts(buffer, (size_t)bytes, header[1], &counts) == 0;
  }
  PackCountCache* entry = pack_count_slot(size, sha, 0u);
  entry->known = (uint8_t)known;
  entry->counts = counts;
  *out = counts;
  return known;
}

// The list bound (custom_pack_checks.c) of a whole data file, read in g_pack_scratch-sized pieces
// through the streaming parser (data files may be up to GTAV_CUSTOM_PACK_DATA_BYTES_MAX, far more
// than the buffer; the result equals the one-buffer bound the row gate computes). Returns 1 with
// *bound set (UINT32_MAX: not plain XML), or 0 when the file could not be read as `size` bytes.
static GtavCustomPackXmlStream g_pack_select_xml;
static int pack_select_file_bound(const char* id, const char* file, uint64_t size,
                                  const char* list, uint32_t* bound) {
  char path[GTAV_PACK_PATH_MAX];
  if (!pack_select_file_path(id, file, path, sizeof(path))) return 0;
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  char* buffer = (char*)&g_pack_scratch;
  gtav_custom_pack_xml_stream_init(&g_pack_select_xml, list, 1u);
  uint64_t done = 0;
  while (done < size) {
    const size_t want =
        size - done < sizeof(g_pack_scratch) ? (size_t)(size - done) : sizeof(g_pack_scratch);
    if (fread(buffer, 1, want, f) != want) break;
    gtav_custom_pack_xml_stream_feed(&g_pack_select_xml, buffer, want);
    done += want;
  }
  const int longer = done == size && fgetc(f) != EOF;  // not the file the descriptor names
  fclose(f);
  if (done != size || longer) return 0;
  *bound = gtav_custom_pack_xml_stream_finish(&g_pack_select_xml);
  return 1;
}

// Data row `row`'s list bound (`list` element children) from its console file, at least
// `fallback`. *unknown is set when the bound is an estimate: the file could not be read (not
// cached: read again on the next check) or is not plain XML (the row gate refuses it when it
// loads).
static uint32_t pack_select_row_bound(int row, const char* list, uint32_t fallback, int* unknown) {
  const GtavCustomPackData& data = g_pack.data[row];
  const uint32_t tag = pack_count_tag(list);
  if (const PackCountCache* hit = pack_count_cached(data.size, data.sha256, tag)) {
    if (!hit->known) *unknown = 1;
    return hit->bound;
  }
  uint32_t bound = UINT32_MAX;
  const char* id = g_pack.source_ids[g_pack.data_source[row]];
  if (!pack_select_file_bound(id, data.file, data.size, list, &bound)) {
    *unknown = 1;
    return fallback;
  }
  PackCountCache* entry = pack_count_slot(data.size, data.sha256, tag);
  entry->known = bound != UINT32_MAX;
  entry->bound = entry->known && bound >= fallback ? bound : fallback;
  if (!entry->known) *unknown = 1;
  return entry->bound;
}

// Per-pack needs of the merged selection (g_pack as pack_select_clash_locked just merged it) into
// g_pack_needs, in source order.
static void pack_select_needs() {
  memset(g_pack_needs, 0, sizeof(g_pack_needs));
  g_pack_needs_count = g_pack.source_count < (int)GTAV_CUSTOM_PACK_ACTIVE_MAX
                           ? g_pack.source_count
                           : (int)GTAV_CUSTOM_PACK_ACTIVE_MAX;
  for (int s = 0; s < g_pack_needs_count; ++s)
    snprintf(g_pack_needs[s].id, sizeof(g_pack_needs[s].id), "%s", g_pack.source_ids[s]);
  uint32_t ped_sources = 0, ptfx_sources = 0;
  for (int i = 0; i < g_pack.data_count && i < (int)GTAV_CUSTOM_PACK_DATA_CAP; ++i) {
    const int s = g_pack.data_source[i];
    if (s >= g_pack_needs_count) continue;
    PackNeeds& needs = g_pack_needs[s];
    switch (g_pack.data[i].type) {
      case GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA: {
        const uint32_t models = pack_select_row_bound(i, "InitDatas", 1u, &needs.unknown);
        needs.tables[PACK_TABLE_VEHICLE] += models;
        needs.stores[PACK_STORE_TXD] += models;
        break;
      }
      case GTAV_CUSTOM_PACK_DATA_PED_METADATA: {
        // Each ped adds by name its own and its props' dwd and txd, and its pmt in the
        // MetaDataStore (an archive .pmt of that name is found, not added: an over-count).
        const uint32_t models = pack_select_row_bound(i, "InitDatas", 1u, &needs.unknown);
        needs.tables[PACK_TABLE_PED] += models;
        needs.stores[PACK_STORE_TXD] += 2u * models;
        needs.stores[PACK_STORE_DWD] += 2u * models;
        needs.stores[PACK_STORE_METADATA] += models;
        ped_sources |= 1u << s;
        break;
      }
      case GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL:
        // Adds nothing by name: its .pmt is an archive member (counted below), and a row whose
        // .pmt is not registered is refused (pack_apparel_gate) before anything parses.
        ped_sources |= 1u << s;
        break;
      case GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA:
        needs.tables[PACK_TABLE_WMODEL] +=
            pack_select_row_bound(i, "InitDatas", 1u, &needs.unknown);
        break;
      case GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO:
        needs.tables[PACK_TABLE_WCOMP] += pack_select_row_bound(i, "Infos", 1u, &needs.unknown);
        break;
      case GTAV_CUSTOM_PACK_DATA_WEAPONINFO:
        needs.tables[PACK_TABLE_WINFO] += 1u;
        break;
      default:
        break;
    }
  }
  for (int i = 0; i < g_pack.spawn_count && i < (int)GTAV_CUSTOM_PACK_SPAWN_CAP; ++i)
    if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_PTFX &&
        g_pack.spawn_source[i] < GTAV_CUSTOM_PACK_ACTIVE_MAX)
      ptfx_sources |= 1u << g_pack.spawn_source[i];
  const int archives = 1 + g_pack.extra_archive_count;
  for (int i = 0; i < archives && i < (int)GTAV_CUSTOM_PACK_ARCHIVE_CAP; ++i) {
    const int s = pack_archive_source(i);
    if (s >= g_pack_needs_count) continue;
    PackNeeds& needs = g_pack_needs[s];
    // Every member counts, an overlay's too (custom_rpf.inc pack_store_adds: registration checks
    // an overlay's entry count, not its member names, before LoadImage).
    GtavCustomPackTocCounts counts;
    if (pack_select_toc(i, &counts)) {
      for (int k = 0; k < PACK_STORE_COUNT; ++k) needs.stores[k] += counts.members[k];
      continue;
    }
    if (g_pack.archive_overlay[i]) {
      // A keyed overlay holds exactly its override rows (registration refuses one with more
      // entries, then counts its staged table): its rows' members by extension until then.
      for (int r = 0; r < g_pack.override_count && r < (int)GTAV_CUSTOM_PACK_OVERRIDE_CAP; ++r) {
        if (g_pack.overrides[r].archive != i) continue;
        const char* member = g_pack.overrides[r].member;
        const int kind = gtav_custom_pack_member_store(member, strlen(member));
        if (kind >= 0) needs.stores[kind]++;
      }
      continue;
    }
    needs.unknown = 1;
    for (int k = 0; k < PACK_STORE_COUNT; ++k) {
      const int ped_store = k == PACK_STORE_METADATA || k == PACK_STORE_DWD;
      if ((k == PACK_STORE_PTFX && !(ptfx_sources & (1u << s))) ||
          (ped_store && !(ped_sources & (1u << s))))
        continue;
      needs.stores[k] += kPackStoreUnknownAdds[k];
    }
  }
}

// Free slots of table `t` in *free_slots, and its capacity in *size. Returns 0, or -1 when the
// table cannot be read (worker, read-only; the row gates stay the backstop).
static int pack_table_headroom(int t, uint32_t* free_slots, uint32_t* size) {
  uint32_t count = 0, capacity = 0;
  int read = -1;
  switch (t) {
    case PACK_TABLE_VEHICLE:
      read = pack_vehicle_model_store(&count, &capacity);
      break;
#ifdef GTAV_LANE_kPedModelFactory
    case PACK_TABLE_PED:
      read = pack_model_factory_block(kPedModelFactory, kPedModelFactoryVtable, &count, &capacity);
      break;
#endif
#ifdef GTAV_LANE_kWeaponModelFactory
    case PACK_TABLE_WMODEL:
      read = pack_model_factory_block(kWeaponModelFactory, kWeaponModelFactoryVtable, &count,
                                      &capacity);
      break;
#endif
#ifdef GTAV_LANE_kComponentInfos
    case PACK_TABLE_WCOMP: {
      const int32_t components =
          pack_readable(kComponentInfoCount, 4) ? pack_component_count() : -1;
      if (components >= 0) {
        count = (uint32_t)components;
        capacity = kComponentInfoMax;
        read = 0;
      }
      break;
    }
#endif
#ifdef GTAV_LANE_kWeaponInfoBlobs
    case PACK_TABLE_WINFO:
      if (pack_readable(kWeaponInfoBlobs, 0x10)) {
        count = *(const volatile uint16_t*)(kWeaponInfoBlobs + 8);
        capacity = *(const volatile uint16_t*)(kWeaponInfoBlobs + 0xa);
        read = count <= capacity ? 0 : -1;
      }
      break;
#endif
    default:
      break;
  }
  if (read != 0) return -1;
  *free_slots = capacity - count;
  *size = capacity;
  return 0;
}

// The merged selection against the game's free slots (worker, read-only, before Load): every
// member store with the exact need (members + by-name adds + margin, gtav_custom_pack_store_need)
// and every model / file table with its rows' adds. The first short one is the clash: its packs are
// the ones adding to it. Unreadable stores and tables are skipped (the registration and row gates
// stay the backstop). Returns PACK_CLASH_STORE (out filled) or PACK_CLASH_NONE.
// Packs before `first_source` are loaded already (an additive load, custom_pack_autoload.inc
// start_pack_append): the game's free slots count them, so only the packs from there on add.
static int pack_select_store_clash(PackClash* out, int first_source = 0) {
#if defined(__ORBIS__)
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack preflight counts begin packs=%d",
                     g_pack.source_count);
#endif
  pack_select_needs();
#if defined(__ORBIS__)
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack preflight stores begin");
#endif
  for (int k = 0; k < PACK_STORE_COUNT + PACK_TABLE_COUNT; ++k) {
    const int table = k >= PACK_STORE_COUNT;
    uint32_t adds = 0, sources = 0;
    for (int s = first_source; s < g_pack_needs_count; ++s) {
      const uint32_t add = table ? g_pack_needs[s].tables[k - PACK_STORE_COUNT]
                                 : g_pack_needs[s].stores[k];
      adds += add;
      if (add) sources |= 1u << s;
    }
    if (!adds) continue;
    uint32_t free_slots = 0, size = 0;
    uint32_t need = adds;
    const char* name = nullptr;
    if (table) {
      if (pack_table_headroom(k - PACK_STORE_COUNT, &free_slots, &size) != 0) continue;
      name = kPackTableNames[k - PACK_STORE_COUNT];
    } else {
      const PackStoreGate& gate = kPackStoreGates[k];
      if (!pack_readable(gate.store, 0x64) || pack_store_headroom(gate, &free_slots, &size) != 0)
        continue;
      need = gtav_custom_pack_store_need(k, adds);
      name = gate.name;
    }
    if (free_slots >= need) continue;
    out->kind = PACK_CLASH_STORE;
    out->packs = sources;
    out->pack = __builtin_ctz(sources);
    out->other = -1;
    gtav_custom_pack_store_refusal(out->what, sizeof(out->what), name, free_slots, need);
    return out->kind;
  }
  return PACK_CLASH_NONE;
}

// "Needs 12 frag, 31 txd, 1 vehicle. " for pack `id` of the last selection check, or "" when it
// was not part of it (Manage Packs footer).
static void pack_needs_text(const char* id, char* out, size_t size) {
  if (!size) return;
  out[0] = '\0';
  for (int s = 0; s < g_pack_needs_count; ++s) {
    if (strcmp(g_pack_needs[s].id, id) != 0) continue;
    const PackNeeds& needs = g_pack_needs[s];
    size_t length = 0;
    for (int k = 0; k < PACK_STORE_COUNT + PACK_TABLE_COUNT && length < size; ++k) {
      const int table = k >= PACK_STORE_COUNT;
      const uint32_t add = table ? needs.tables[k - PACK_STORE_COUNT] : needs.stores[k];
      if (!add) continue;
      const int n = snprintf(out + length, size - length, "%s%u %s", length ? ", " : "Needs ",
                             (unsigned)add,
                             table ? kPackTableShort[k - PACK_STORE_COUNT] : kPackStoreShort[k]);
      if (n < 0) break;
      length += (size_t)n;
    }
    if (length && length < size)
      snprintf(out + length, size - length, "%s. ", needs.unknown ? " (estimated)" : "");
    return;
  }
}

// The game's free slots for custom packs (the load row's footer and the pack-notes census):
// "Free: frag 284, ..., vehicle 3, ped 5, weapon model 58, component 5, weapon file 6".
// Worker, read-only; an unreadable store or table is left out. When the counts changed, at most
// every 600 worker ticks (the world's streaming moves them constantly), pack-notes gets them as two
// lines with the gate names ("pack census stores frag=284 ...",
// "pack census tables vehicle=3 ...").
static const char* pack_select_census() {
  static char text[256];
  static char noted[256];
  static uint32_t noted_tick, computed_tick;
  static int noted_once, computed_once;
  // The footer is drawn every frame: read the stores and tables once per rescan period.
  if (computed_once && g_pack_worker_ticks - computed_tick < GTAV_PACK_SCAN_TICKS) return text;
  computed_once = 1;
  computed_tick = g_pack_worker_ticks;
  char lines[2][160];
  size_t line_length[2];
  line_length[0] = (size_t)snprintf(lines[0], sizeof(lines[0]), "pack census stores");
  line_length[1] = (size_t)snprintf(lines[1], sizeof(lines[1]), "pack census tables");
  size_t length = (size_t)snprintf(text, sizeof(text), "Free:");
  for (int k = 0; k < PACK_STORE_COUNT + PACK_TABLE_COUNT && length < sizeof(text); ++k) {
    uint32_t free_slots = 0, size = 0;
    const int table = k >= PACK_STORE_COUNT;
    if (table) {
      if (pack_table_headroom(k - PACK_STORE_COUNT, &free_slots, &size) != 0) continue;
    } else {
      const PackStoreGate& gate = kPackStoreGates[k];
      if (!pack_readable(gate.store, 0x64) || pack_store_headroom(gate, &free_slots, &size) != 0)
        continue;
    }
    const int n =
        snprintf(text + length, sizeof(text) - length, "%s %s %u", length > 5 ? "," : "",
                 table ? kPackTableShort[k - PACK_STORE_COUNT] : kPackStoreShort[k],
                 (unsigned)free_slots);
    if (n < 0) break;
    length += (size_t)n;
    size_t& at = line_length[table];
    if (at < sizeof(lines[table]))
      at += (size_t)snprintf(lines[table] + at, sizeof(lines[table]) - at, " %s=%u",
                             table ? kPackTableNames[k - PACK_STORE_COUNT]
                                   : kPackStoreGates[k].name,
                             (unsigned)free_slots);
  }
  if (strcmp(text, noted) != 0 && (!noted_once || g_pack_worker_ticks - noted_tick >= 600u)) {
    snprintf(noted, sizeof(noted), "%s", text);
    noted_tick = g_pack_worker_ticks;
    noted_once = 1;
    pack_world_note("%s", lines[0]);
    pack_world_note("%s", lines[1]);
  }
  return text;
}
#endif

// Parse and merge every pack of `ids` the way the load will, each descriptor parsed into the
// shared g_pack_scratch (custom_rpf.inc), which the caller holds, and merged into g_pack, which no
// one uses before the load has parsed the selection (pack_load_descriptor merges into it while it
// holds the same scratch, and marks it parsed before releasing that). On a repeated name the
// earlier pack holding it is found by re-checking the failing pack against each earlier one alone
// (the merged set is not needed any more). A selection that merges is then checked against the
// game's store headroom (pack_select_store_clash). Returns the PackClash kind (*out filled).
static int pack_select_clash_locked(
    const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX], int count,
    PackClash* out) {
  GtavCustomPack& set = g_pack;
  GtavCustomPack& one = g_pack_scratch;
  memset(out, 0, sizeof(*out));
  out->other = -1;
#if GTAV_MENU_CUSTOM_PACK_ENGINE
  g_pack_needs_count = 0;  // filled by pack_select_store_clash for a selection that merges
#endif
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
#if GTAV_MENU_CUSTOM_PACK_ENGINE
  return live ? PACK_CLASH_NONE : pack_select_store_clash(out);
#else
  return PACK_CLASH_NONE;  // no store census without the engine
#endif
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

// A store clash involves pack `id`: it is one of the packs that need the full store.
static int pack_store_clash_names(
    const PackClash& clash, const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
    const char* id) {
  if (clash.kind != PACK_CLASH_STORE) return 0;
  for (uint32_t i = 0; i < GTAV_CUSTOM_PACK_ACTIVE_MAX; ++i)
    if ((clash.packs & (1u << i)) && strcmp(ids[i], id) == 0) return 1;
  return 0;
}

// The packs of `mask` (bit i: ids[i]) for a store clash's fix: "a, b" when every id fits `size`,
// else their count with `noun` ("2 ped packs"; Manage Packs marks each of them).
static void pack_clash_ids(uint32_t mask,
                           const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
                           const char* noun, char* out, size_t size) {
  if (!size) return;
  mask &= (1u << GTAV_CUSTOM_PACK_ACTIVE_MAX) - 1u;
  size_t length = 0;
  for (uint32_t i = 0; i < GTAV_CUSTOM_PACK_ACTIVE_MAX; ++i)
    if (mask & (1u << i)) length += (length ? 2u : 0u) + strlen(ids[i]);
  if (length < size) {
    length = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < GTAV_CUSTOM_PACK_ACTIVE_MAX; ++i)
      if (mask & (1u << i))
        length += (size_t)snprintf(out + length, size - length, "%s%s", length ? ", " : "", ids[i]);
    return;
  }
  const int count = __builtin_popcount(mask);
  snprintf(out, size, "%d %s%s", count, noun, count == 1 ? "" : "s");
}

// The packs a store or table refusal names, by its scope ("2 ped packs").
static const char* pack_store_noun(int scope) {
  switch (scope) {
    case GTAV_CUSTOM_PACK_STORE_PED:
      return "ped pack";
    case GTAV_CUSTOM_PACK_STORE_PTFX:
      return "particle pack";
    case GTAV_CUSTOM_PACK_STORE_VEHICLE:
      return "vehicle pack";
    case GTAV_CUSTOM_PACK_STORE_WEAPON:
      return "weapon pack";
    default:
      return "pack";
  }
}

// A store clash's text: "<Store> slots full (<n> free, need <m>)" (custom_pack_checks.c) and the
// fix: deselect the packs that need the store, or (`adding`) the pack was not selected. A toast
// passes the 76 bytes left beside its status glyph, the load row's footer its whole line.
static void pack_store_clash_text(
    const PackClash& clash, const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
    int adding, char* out, size_t size) {
  char detail[GTAV_CUSTOM_PACK_STORE_DETAIL_MAX + 1];
  int scope = GTAV_CUSTOM_PACK_STORE_PED;
  if (!gtav_custom_pack_store_reason(clash.what, detail, sizeof(detail), &scope))
    snprintf(detail, sizeof(detail), "Game slots full");
  if (adding) {
    snprintf(out, size, "%s: not selected", detail);
    return;
  }
  const int prefix = snprintf(out, size, "%s: deselect ", detail);
  if (prefix > 0 && (size_t)prefix < size)
    pack_clash_ids(clash.packs, ids, pack_store_noun(scope), out + prefix, size - (size_t)prefix);
}

// Toast for a refused selection: names the pack to deselect (`adding`: the selected pack the new
// one clashes with; otherwise the later of the two selected ones) and the clash. Fits the toast's
// 80 bytes with its status glyph; nullptr when nothing clashes.
static const char* pack_clash_text(
    const PackClash& clash, const char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX],
    int adding) {
  static char text[GTAV_FEATURE_MESSAGE_LEN];
  switch (clash.kind) {
    case PACK_CLASH_STORE:
      pack_store_clash_text(clash, ids, adding, text, sizeof(text) - 4u);  // "[!] " glyph
      return text;
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

// Rescan step: check the selection before any load (one descriptor read per selected pack, up to
// GTAV_CUSTOM_PACK_ACTIVE_MAX, plus its archive tables and data files once each: the counts are
// cached), keep the last result while a load holds the buffer, and log each new result once
// (pack-notes).
static void pack_select_check_selection() {
  ++g_pack_scan_serial;
  PackClash clash = {};
  clash.other = -1;
#if defined(__ORBIS__)
  if (g_pack_selection_valid && g_pack_selected_count > 0 && pack_select_before_load()) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack preflight descriptors begin selected=%d",
                       g_pack_selected_count);
    const int kind = pack_select_clash(g_pack_selected, g_pack_selected_count, &clash);
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack preflight end kind=%d", kind);
    if (kind == PACK_CLASH_BUSY) return;
  }
#else
  if (g_pack_selection_valid && g_pack_selected_count > 0 && pack_select_before_load() &&
      pack_select_clash(g_pack_selected, g_pack_selected_count, &clash) == PACK_CLASH_BUSY)
    return;
#endif
  const int changed = clash.kind != g_pack_selection_clash.kind ||
                      clash.pack != g_pack_selection_clash.pack ||
                      clash.other != g_pack_selection_clash.other ||
                      clash.packs != g_pack_selection_clash.packs ||
                      strcmp(clash.what, g_pack_selection_clash.what) != 0;
  g_pack_selection_clash = clash;
  if (!changed) return;
  if (clash.kind == PACK_CLASH_NONE)
    pack_world_note("pack selection ok selected=%d", g_pack_selected_count);
  else if (clash.kind == PACK_CLASH_STORE)
    pack_world_note("pack selection blocked: %s packs=0x%x", clash.what, (unsigned)clash.packs);
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

// Worker-direct action: add or remove installed pack `param` (installed order) from the
// selection. The list heading (GTAV_PACK_MENU_LIST_ROW) is no pack: a host command naming it is
// refused.
// Refusals name the pack and the fix and fit a toast (GTAV_FEATURE_MESSAGE_LEN with the status
// glyph); the klog gets the clash with both ids.
static uint32_t toggle_pack_active(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (param == GTAV_PACK_MENU_LIST_ROW) return unavailable(action, "Select a pack row below");
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
    // A full store refuses only a pack that needs it; one the selection already has blocks Load,
    // not adding an unrelated pack.
    if (pack_select_clash(ids, count, &clash) != PACK_CLASH_NONE &&
        (clash.kind != PACK_CLASH_STORE || pack_store_clash_names(clash, ids, id))) {
      // what: up to a store refusal's 73 characters, so its free=/need= counts reach pack-notes.
      pack_world_note("pack select refused id=%.40s kind=%d other=%.40s what=%.73s", id, clash.kind,
                      clash.other >= 0 ? ids[clash.other] : "-", clash.what);
      return unavailable(action, pack_clash_text(clash, ids, 1));
    }
  }
  if (pack_select_write(ids, count) != 0)
    return failed(action, "Could not save the selection; retry");
  pack_select_scan(1);
  // A load refused before anything reached the game does not count: the next Load starts over.
  // After a finished load, Load adds the newly selected packs to this game.
  int loaded_here = g_rpf_register_attempted || (g_pack_parsed && !pack_load_failed_clean());
#if GTAV_MENU_CUSTOM_PACK_ENGINE
  if (pack_append_possible()) loaded_here = 0;
#endif
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
  char* index = g_pack_index_text;
  static char body[GTAV_PACK_INSTALLED_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1];
  char path[GTAV_PACK_PATH_MAX];
  char temp[GTAV_PACK_PATH_MAX];
  *listed = 0;
  pack_root_path(path, sizeof(path), nullptr, "/installed");
  pack_root_path(temp, sizeof(temp), nullptr, "/installed.tmp");
  if (!path[0] || !temp[0]) return -1;
  const int64_t size = pack_select_read(path, index, sizeof(g_pack_index_text));
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

#if defined(__ORBIS__)
// The archive copies a PS4 load staged for pack `id` (GTAV_PS4_CACHE_ROOT/<id>, custom_rpf.inc
// pack_cache_path): derived files its next load stages again. The game streams resource pages from
// them, so only a pack this game never loaded may lose them. Returns the number of files removed.
static int pack_prune_cache(const char* id) {
  char dir[GTAV_PACK_PATH_MAX];
  if (snprintf(dir, sizeof(dir), "%s/%s", GTAV_PS4_CACHE_ROOT, id) >= (int)sizeof(dir)) return 0;
  int removed = 0;
  // Removing entries while listing can skip some, so list again until a pass removes nothing.
  for (int pass = 0; pass < 4; ++pass) {
    DIR* listing = opendir(dir);
    if (!listing) break;
    int this_pass = 0;
    while (struct dirent* entry = readdir(listing)) {
      if (entry->d_name[0] == '.') continue;
      char path[GTAV_PACK_PATH_MAX];
      if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) continue;
      if (unlink(path) == 0) ++this_pass;
    }
    closedir(listing);
    removed += this_pass;
    if (this_pass == 0) break;
  }
  return removed;
}
#endif

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
#if defined(__ORBIS__)
  if (!loaded) pack_world_note("pack uninstall cache id=%.36s removed=%d", id, pack_prune_cache(id));
#endif
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
  else if (pack_store_clash_names(clash, ids, id)) {
    char detail[GTAV_CUSTOM_PACK_STORE_DETAIL_MAX + 1];
    if (gtav_custom_pack_store_reason(clash.what, detail, sizeof(detail), nullptr))
      snprintf(out, size, "Load blocked: %s. ", detail);
  }
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
  char needs[112] = "";
#if GTAV_MENU_CUSTOM_PACK_ENGINE
  if (pack_select_before_load()) pack_needs_text(id, needs, sizeof(needs));
#endif
  snprintf(info, sizeof(info), "%s%s%s", prefix, needs, about);
  return info;
}
