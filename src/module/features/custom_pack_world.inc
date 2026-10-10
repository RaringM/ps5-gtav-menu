// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS && PS5 branch of the runtime pack lane.
//
// Pack archetype definitions (.ptyp via DLC_ITYP_REQUEST) and map data (.pmap activation).

// Archetype definitions: the DLC_ITYP_REQUEST
// mounter (type 174) never opens a file. It takes the DataFile filename's base name, finds that
// name's slot in the typ store (our archive registered the .ptyp member there) and requests it;
// the archetypes are registered into the model map when the .ptyp finishes streaming. No loose
// entry and no parser pump are involved.
static const struct {
  uintptr_t iface;
  uintptr_t vtable;
  uintptr_t load;
  uint8_t pin[32];
} kItypMounter = {0x53fb200,
                  0x3fa7e90,
                  0x1a46fa0,
                  {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x10,
                   0x01, 0x00, 0x00, 0x4c, 0x8b, 0x35, 0xab, 0x8a, 0x66, 0x02, 0xc5,
                   0xf8, 0x57, 0xc0, 0xb9, 0x6f, 0x72, 0x79, 0x3a, 0x48, 0x89}};
static const uint32_t kItypRequestType = 174;
// Per typ row (bit = row). uint64_t: GTAV_CUSTOM_PACK_TYP_CAP rows.
static uint64_t g_pack_typs_loaded;
static uint64_t g_pack_typs_failed;  // a `retail` row whose stock typ is not in the typ store
static_assert(GTAV_CUSTOM_PACK_TYP_CAP <= 64, "typ row masks are uint64_t");
// A stock (`retail`) typ registers all its archetypes (v_minigame: 657); one is requested at a
// time, each only with this much archetype-pool headroom.
static const uint32_t kRetailTypHeadroom = 1024;

// Archetype-definition (ptyp) store and its find-slot (vt+0x48, same shape as the map store's).
// A hardware run crashed a Subrender thread right after the one-press load reloaded a
// map initialised while its archetypes were only requested: a map row now waits for every typ row
// to be resident (status 1).
static const uintptr_t kTypStore = 0x6210a10;
static const uintptr_t kTypStoreVtable = 0x4093b08;
static const uintptr_t kTypFindSlot = 0x353bdd0;
static const uint8_t kTypFindSlotBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x89, 0xf7,
    0x49, 0x89, 0xf7, 0xff, 0x15, 0x7f, 0x42, 0x74, 0x02, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x89, 0xc6};

// Typ store slot and def of the typ named `file` (<name>.ptyp; find-slot hashes the name: main
// thread only). Fills the base name, slot, def, streaming status and (optional) the streaming
// info's first word (0 with no file behind the slot); returns nullptr, or why the slot cannot be
// used.
static const char* pack_typ_named_slot_def(const char* file, char* name, size_t name_size,
                                           int32_t* slot, uintptr_t* def,
                                           uint32_t* status = nullptr,
                                           uint32_t* handle = nullptr) {
  typedef int32_t (*FindSlotFn)(uintptr_t, const char*);
  if (*(const volatile uintptr_t*)kTypStore != kTypStoreVtable ||
      ((const volatile uintptr_t*)kTypStoreVtable)[0x48 / 8] != kTypFindSlot ||
      memcmp((const void*)kTypFindSlot, kTypFindSlotBytes, sizeof(kTypFindSlotBytes)) != 0)
    return "pack typ store anchors mismatch";
  snprintf(name, name_size, "%s", file);
  if (char* dot = strrchr(name, '.')) *dot = '\0';
  const int32_t typ_slot = ((FindSlotFn)kTypFindSlot)(kTypStore, name);
  const uintptr_t typ_defs = *(const volatile uintptr_t*)(kTypStore + 0x40);
  const uintptr_t typ_flags = *(const volatile uintptr_t*)(kTypStore + 0x48);
  const uint32_t typ_stride = *(const volatile uint32_t*)(kTypStore + 0x54);
  if (typ_slot < 0 || (uint32_t)typ_slot >= *(const volatile uint32_t*)(kTypStore + 0x50) ||
      !typ_defs || !typ_flags || typ_stride < 0x14u ||
      *(const volatile int8_t*)(typ_flags + (uintptr_t)(uint32_t)typ_slot) < 0)
    return "pack map dependency typ slot not found";
  *slot = typ_slot;
  *def = typ_defs + (uintptr_t)(uint32_t)typ_slot * typ_stride;
  if (!pack_readable(*def, 0x14)) return "pack map dependency typ def unreadable";
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  const uint32_t base = *(const volatile uint32_t*)(kTypStore + 8);
  const uintptr_t info = infos ? infos + ((uintptr_t)base + (uint32_t)typ_slot) * 8u : 0;
  if (status) *status = info ? *(const volatile uint32_t*)(info + 4u) : UINT32_MAX;
  if (handle) *handle = info ? *(const volatile uint32_t*)info : 0u;
  return nullptr;
}

static const char* pack_typ_slot_def(uint32_t row, char* name, size_t name_size, int32_t* slot,
                                     uintptr_t* def, uint32_t* status = nullptr) {
  return pack_typ_named_slot_def(g_pack.typs[row], name, name_size, slot, def, status);
}

// A world-lane event that must survive the status ring (a hardware run lost the interior events
// to it): the same text as a status event and as a "GTAVMenu pack ..." klog line.
static void pack_world_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void pack_world_note(const char* format, ...) {
  char text[160];  // the klog / pack-notes copy keeps the whole line; the status event truncates
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, text);
  char line[sizeof(text) + 16];
  snprintf(line, sizeof(line), "GTAVMenu %s", text);
  gtav_worker_klog(line);
}

// A typ's ITYP dependency on a stock typ (`typdep`, the typ->typ dependency
// binding). The manifest binder (0x1998d60 live) gives a retail typ its itypDependencies at boot;
// for a typ with none yet it writes typ def +0x12 (u16 count) = 1 and +0x18 = the dependency's typ
// slot (inline for one) and sets the dependency's +0x10 |= 8, and it skips a dependency whose +0x10
// has 0x14 (permanent or DLC-requested: resident anyway). This mirrors that one-entry case. The
// typ store's GetDependencies (vt+0xe0, 0x351df30 live) returns the slot when the typ is requested
// (RequestObject 0x355aa20 live, which then requests the dependency itself, loads it first and
// holds it with a dependent count), and the entity creator (0x350f410 live) accepts an archetype of
// a map dependency's own typ dependency: an MLO-only clone's room entities, which are the stock
// typ's archetypes. Main thread, before the typ's own request. Returns nullptr when bound or not
// needed, "retry: " + reason to wait, or why it cannot be bound.
static const char* pack_bind_typ_dependency(uint32_t row, uint32_t archetypes_free) {
  if (!g_pack.typ_deps[row][0]) return nullptr;
  char name[GTAV_CUSTOM_PACK_NAME_MAX], dep_name[GTAV_CUSTOM_PACK_NAME_MAX];
  int32_t slot = -1, dep_slot = -1;
  uintptr_t def = 0, dep_def = 0;
  uint32_t status = UINT32_MAX, dep_status = UINT32_MAX, dep_handle = 0;
  if (const char* refusal = pack_typ_slot_def(row, name, sizeof(name), &slot, &def, &status))
    return refusal;
  if (pack_typ_named_slot_def(g_pack.typ_deps[row], dep_name, sizeof(dep_name), &dep_slot, &dep_def,
                              &dep_status, &dep_handle))
    return "typ dependency not in the typ store";
  if (*(const volatile uint32_t*)(kTypStore + 0x54) < 0x1cu || !pack_readable(def, 0x1c) ||
      !pack_readable(dep_def, 0x1c))
    return "typ def layout unexpected";
  const uint32_t count = *(const volatile uint16_t*)(def + 0x12);
  const uint32_t dep_flags = *(const volatile uint16_t*)(dep_def + 0x10);
  const uint32_t dep_deps = *(const volatile uint16_t*)(dep_def + 0x12);
  const char* verdict = "bound";
  if (count == 1u && *(const volatile uint32_t*)(def + 0x18) == (uint32_t)dep_slot) {
    verdict = "already bound";
  } else if (count != 0u) {
    return "typ already has dependencies";
  } else if (dep_flags & 0x14u) {
    verdict = "not bound (permanent or requested)";
  } else if ((status & 3u) != 0u) {
    return "typ already requested; its dependency cannot be bound (restart GTA)";
  } else if (!dep_handle && !(dep_status & 0x2000000u)) {
    return "typ dependency has no streaming file";  // RequestObject would refuse the typ
  } else if ((dep_status & 3u) != 1u && archetypes_free < kRetailTypHeadroom) {
    return "retry: archetype pool headroom gate refused for a typ dependency";
  } else {
    *(volatile uint32_t*)(def + 0x18) = (uint32_t)dep_slot;
    *(volatile uint16_t*)(def + 0x12) = 1u;
    *(volatile uint16_t*)(dep_def + 0x10) = (uint16_t)(dep_flags | 8u);
  }
  // dep_deps: the stock typ's own (manifest) dependencies; the engine streams them too, but the
  // entity creator looks one level down only, so their archetypes would not populate a room.
  pack_world_note("pack typ %u %s dep %s slot=%d flags=0x%x status=0x%x dep_deps=%u %s",
                  (unsigned)row, name, dep_name, (int)dep_slot, (unsigned)dep_flags,
                  (unsigned)dep_status, (unsigned)dep_deps, verdict);
  return nullptr;
}

static uint32_t load_pack_typ(uint32_t param) {
  typedef bool (*LoadDataFileFn)(void*, const void*);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.typ_count) return unavailable(action, "pack typ row out of range");
  if (g_pack_typs_loaded & (1ull << param)) return unavailable(action, "pack typ row already loaded");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!pack_archives_resident())
    return unavailable(action, "pack archive is not resident; run pack-load-archive first");
  const uintptr_t iface = ((const volatile uintptr_t*)kDataFileInterfaces)[kItypRequestType];
  if (iface != kItypMounter.iface || !pack_readable(iface, 8) ||
      *(const volatile uintptr_t*)iface != kItypMounter.vtable ||
      ((const volatile uintptr_t*)kItypMounter.vtable)[2] != kItypMounter.load ||
      memcmp((const void*)kItypMounter.load, kItypMounter.pin, sizeof(kItypMounter.pin)) != 0)
    return unavailable(action, "pack typ mounter anchors mismatch");
  const uint32_t archetypes_free = *(const volatile uint32_t*)kArchetypePoolFree;
  if (archetypes_free < 64u) return unavailable(action, "archetype pool headroom gate refused");

  if (g_pack.typ_retail[param]) {
    // A stock typ (an interior's itypDependencies): the same mounter finds its retail slot by name.
    // Requested one at a time, after the previous stock row is resident, with headroom for a
    // whole prop typ.
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    int32_t slot = -1;
    uintptr_t def = 0;
    uint32_t status = UINT32_MAX;
    for (uint32_t r = 0; r < param; ++r) {
      if (!g_pack.typ_retail[r] || !(g_pack_typs_loaded & (1ull << r))) continue;
      int32_t s = -1;
      uintptr_t d = 0;
      uint32_t st = UINT32_MAX;
      if (!pack_typ_slot_def(r, name, sizeof(name), &s, &d, &st) && (st & 3u) != 1u)
        return unavailable(action, "stock typ waits for the previous stock typ; retry");
    }
    if (const char* refusal = pack_typ_slot_def(param, name, sizeof(name), &slot, &def, &status)) {
      g_pack_typs_failed |= 1ull << param;
      pack_world_note("pack typ %u %s stock typ refused: %s", (unsigned)param, g_pack.typs[param],
                      refusal);
      return failed(action, "stock typ not in the typ store");
    }
    if ((status & 3u) != 1u && archetypes_free < kRetailTypHeadroom)
      return unavailable(action, "archetype pool headroom gate refused for a stock typ");
    pack_world_note("pack typ %u %s stock slot=%d flags=0x%x status=0x%x archetypes_free=%u",
                    (unsigned)param, name, (int)slot, (unsigned)*(const volatile uint32_t*)(def + 0x10),
                    (unsigned)status, (unsigned)archetypes_free);
  }
  // A pack typ's stock dependency (`typdep`) is bound before the request reads it.
  if (const char* problem = pack_bind_typ_dependency(param, archetypes_free)) {
    if (strncmp(problem, "retry: ", 7) == 0) return unavailable(action, problem + 7);
    g_pack_typs_failed |= 1ull << param;
    pack_world_note("pack typ %u %s dependency refused: %s", (unsigned)param, g_pack.typs[param],
                    problem);
    return failed(action, problem);
  }

  // An interior's typ keeps its loaded data: the typ load frees it unless the def's
  // flags carry 0x200 without 4 (0x311e0f0 normalized), which only the manifest binder sets, for
  // INTERIOR_DATA dependencies. Set it before the request, like a retail interior typ from boot.
  int keep = -1;  // -1: not an interior typ, 0: could not set, 1: set
  for (int r = 0; r < g_pack.map_count; ++r)
    if (g_pack.map_interior[r] && g_pack.map_dep_typ[r] == param + 1u) keep = 0;
  if (keep == 0) {
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    int32_t slot = -1;
    uintptr_t def = 0;
    uint32_t status = UINT32_MAX;
    const char* refusal = pack_typ_slot_def(param, name, sizeof(name), &slot, &def, &status);
    const uint32_t flags = refusal ? 0u : *(const volatile uint32_t*)(def + 0x10);
    if (!refusal && (status & 3u) == 0u && !(flags & 4u)) {
      *(volatile uint8_t*)(def + 0x11) |= 2u;  // +0x10 |= 0x200
      keep = 1;
    }
    pack_world_note("pack typ %u interior keep=%d slot=%d flags=0x%x status=0x%x%s%s",
                    (unsigned)param, keep, (int)slot, (unsigned)flags, (unsigned)status,
                    refusal ? " " : "", refusal ? refusal : "");
  }
  g_pack_typs_loaded |= 1ull << param;
  memset(g_pack_datafile, 0, sizeof(g_pack_datafile));
  snprintf((char*)g_pack_datafile, 0x80, "%s", g_pack.typs[param]);
  *(uint32_t*)(g_pack_datafile + 0x90) = kItypRequestType;
  gtav_worker_klog("GTAVMenu pack typ request before");
  const bool loaded = ((LoadDataFileFn)kItypMounter.load)((void*)iface, g_pack_datafile);
  gtav_worker_klog("GTAVMenu pack typ request returned");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack typ %u %s requested returned %d archetypes_free=%u",
           (unsigned)param, g_pack.typs[param], (int)loaded, (unsigned)archetypes_free);
  return ok(action, message);
}

// Map data. The game activates archive-new
// map-data slots in 0x311c1a0 (normalized): request, LOAD_ALL_OBJECTS_NOW, learn the extents, mark
// initialised, set the box-streamer state word, then drop the slot so the box streamer owns it.
// The worker never calls the blocking load. It mirrors 0x311c1a0 per slot (section 7.2):
// load_pack_map makes the bounds-only request (flag 6); finish_pack_map, retried, then (a) once
// loaded with filled box bounds removes the slot (RemoveObject 0x31502c0 normalized), marks it
// initialised, sets the box-streamer state and requests it again keep-resident (flag 7) -- the
// post-load routine 0x3118980 creates entities only for an initialised slot (on hardware, a slot
// initialised after its only load had no entities) -- and (b) confirms the new contents' entity
// count, clearing the IPL-disabled bit like REQUEST_IPL when it is set.
static const uintptr_t kMapStore = 0x61a9aa0;
static const uintptr_t kMapStoreVtable = 0x4093970;
static const uintptr_t kMapFindSlot = 0x353b9e0;  // store vt+0x48 (store, name) -> slot or -1
static const uintptr_t kMapEnableIpl = 0x3515530;  // (slot, name hash, 0), REQUEST_IPL's call
static const uintptr_t kRemoveObject = 0x35502c0;  // (global index) -> bool
static const uint8_t kRemoveObjectBytes[20] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                               0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                                               0x81, 0xec, 0x48, 0x0c, 0x00, 0x00};
static const uint8_t kMapFindSlotBytes[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                              0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x89, 0xf7};
static const uint8_t kMapEnableIplBytes[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x53, 0x50, 0x83, 0xff, 0xff, 0x0f, 0x84, 0xd6};
// 1 when typ row `row` is resident, 0 while pending, -1 when its slot cannot be found or read.
// Main thread only (the find-slot hashes the name and reads the store's map).
static int pack_typ_resident(uint32_t row) {
  typedef int32_t (*FindSlotFn)(uintptr_t, const char*);
  if (row >= (uint32_t)g_pack.typ_count || *(const volatile uintptr_t*)kTypStore != kTypStoreVtable ||
      ((const volatile uintptr_t*)kTypStoreVtable)[0x48 / 8] != kTypFindSlot ||
      memcmp((const void*)kTypFindSlot, kTypFindSlotBytes, sizeof(kTypFindSlotBytes)) != 0)
    return -1;
  char name[GTAV_CUSTOM_PACK_NAME_MAX];
  snprintf(name, sizeof(name), "%s", g_pack.typs[row]);
  if (char* dot = strrchr(name, '.')) *dot = '\0';
  const int32_t slot = ((FindSlotFn)kTypFindSlot)(kTypStore, name);
  const uint32_t size = *(const volatile uint32_t*)(kTypStore + 0x50);
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  if (slot < 0 || (uint32_t)slot >= size || !infos) return -1;
  const uint32_t base = *(const volatile uint32_t*)(kTypStore + 8);
  const uint32_t status = *(const volatile uint32_t*)(infos + ((uintptr_t)base + (uint32_t)slot) * 8u + 4u);
  return (status & 3u) == 1u ? 1 : 0;
}

// Every typ row requested and resident.
static int pack_typs_resident() {
  for (int i = 0; i < g_pack.typ_count; ++i)
    if (!(g_pack_typs_loaded & (1ull << i)) || pack_typ_resident((uint32_t)i) != 1) return 0;
  return 1;
}

static uint32_t g_pack_maps_requested;  // per map row (bit = row): GTAV_CUSTOM_PACK_MAP_CAP rows
static_assert(GTAV_CUSTOM_PACK_MAP_CAP <= 32, "map row masks are uint32_t");
static volatile int g_pack_map_waiting_typs;  // last map request refused only for typ residency
static uint32_t g_pack_maps_finished;  // activated (teleport row ready)
static uint32_t g_pack_maps_failed;    // stopped for good: never retried, fails the one-press load
static int32_t g_pack_map_slots[GTAV_CUSTOM_PACK_MAP_CAP];
static uint8_t g_pack_map_reloaded[GTAV_CUSTOM_PACK_MAP_CAP];

// Map-data store views: defs {.., u32 flags @+0x20, u32 content @+0x24} of `def size` bytes,
// the module's streaming base, box bounds {min vec4, max vec4} per slot and u16 state words.
static int pack_map_store_ok() {
  return *(const volatile uintptr_t*)kMapStore == kMapStoreVtable &&
         ((const volatile uintptr_t*)kMapStoreVtable)[0x48 / 8] == kMapFindSlot &&
         memcmp((const void*)kMapFindSlot, kMapFindSlotBytes, sizeof(kMapFindSlotBytes)) == 0 &&
         memcmp((const void*)kMapEnableIpl, kMapEnableIplBytes, sizeof(kMapEnableIplBytes)) == 0 &&
         memcmp((const void*)kRequestObject, kRequestObjectBytes, sizeof(kRequestObjectBytes)) == 0 &&
         memcmp((const void*)kRemoveObject, kRemoveObjectBytes, sizeof(kRemoveObjectBytes)) == 0 &&
         *(const volatile uint32_t*)(kMapStore + 0x54) == 0x28u;
}

static uintptr_t pack_map_def(int32_t slot) {
  const uintptr_t defs = *(const volatile uintptr_t*)(kMapStore + 0x40);
  return defs ? defs + (uintptr_t)(uint32_t)slot * 0x28u : 0;
}

static uint32_t pack_map_status(int32_t slot) {
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  const uint32_t base = *(const volatile uint32_t*)(kMapStore + 8);
  return infos ? *(const volatile uint32_t*)(infos + ((uintptr_t)base + (uint32_t)slot) * 8u + 4u)
               : UINT32_MAX;
}

static void pack_map_basename(uint32_t row, char* name, size_t size) {
  snprintf(name, size, "%s", g_pack.maps[row]);
  char* dot = strrchr(name, '.');
  if (dot) *dot = '\0';
}

// Static collision bounds. The archive
// registration already added each .pbn member as a slot of the static bounds store; unlike map data
// the store has no "initialised" gate, so a keep-resident request (flag 7) is all it takes: on load
// set-contents (0x3521980 live) stores the bound, computes the slot box from a composite root's
// children and inserts one physics instance per child. Only a composite root (type 10) collides
// (make_static_pbn.py writes one); the first call requests, later calls confirm.
static const uintptr_t kBoundsStore = 0x61a9740;
static const uintptr_t kBoundsStoreVtable = 0x4093ff0;
static const uintptr_t kBoundsFindSlot = 0x353d320;  // vt+0x48 (store, name) -> slot or -1
static const uint8_t kBoundsFindSlotBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x89, 0xf7, 0xff, 0x15, 0x06, 0x2d,
    0x74, 0x02, 0x48, 0x8b, 0xb3, 0x80, 0x00, 0x00, 0x00, 0x89, 0xc1, 0x31, 0xd2, 0xf7, 0xb3, 0x88};
static uint32_t g_pack_bounds_requested;  // per bounds row (bit = row): GTAV_CUSTOM_PACK_BOUNDS_CAP
static_assert(GTAV_CUSTOM_PACK_BOUNDS_CAP <= 32, "bounds row masks are uint32_t");
static uint32_t g_pack_bounds_finished;  // loaded and validated
static uint32_t g_pack_bounds_failed;    // loaded but not a usable composite; never retried
static int32_t g_pack_bounds_slots[GTAV_CUSTOM_PACK_BOUNDS_CAP];
static uint32_t g_pack_bounds_records[GTAV_CUSTOM_PACK_BOUNDS_CAP];  // store +0x1d8 before

static int pack_bounds_store_ok() {
  return *(const volatile uintptr_t*)kBoundsStore == kBoundsStoreVtable &&
         ((const volatile uintptr_t*)kBoundsStoreVtable)[0x48 / 8] == kBoundsFindSlot &&
         memcmp((const void*)kBoundsFindSlot, kBoundsFindSlotBytes, sizeof(kBoundsFindSlotBytes)) ==
             0 &&
         memcmp((const void*)kRequestObject, kRequestObjectBytes, sizeof(kRequestObjectBytes)) == 0 &&
         *(const volatile uint32_t*)(kBoundsStore + 0x54) == 0x10u;
}

static uint32_t pack_bounds_status(int32_t slot) {
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  const uint32_t base = *(const volatile uint32_t*)(kBoundsStore + 8);
  return infos ? *(const volatile uint32_t*)(infos + ((uintptr_t)base + (uint32_t)slot) * 8u + 4u)
               : UINT32_MAX;
}

static uint32_t load_pack_bounds(uint32_t param) {
  typedef int32_t (*FindSlotFn)(uintptr_t, const char*);
  typedef void (*RequestObjectFn)(uint32_t, uint32_t);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.bounds_count) return unavailable(action, "pack bounds row out of range");
  if ((g_pack_bounds_finished | g_pack_bounds_failed) & (1u << param))
    return unavailable(action, "pack bounds row already loaded");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!pack_archives_resident())
    return unavailable(action, "pack archive is not resident; run pack-load-archive first");
  if (!pack_bounds_store_ok()) return unavailable(action, "pack bounds store anchors mismatch");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  if (!(g_pack_bounds_requested & (1u << param))) {
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_pack.bounds[param]);
    if (char* dot = strrchr(name, '.')) *dot = '\0';
    const int32_t slot = ((FindSlotFn)kBoundsFindSlot)(kBoundsStore, name);
    const uint32_t size = *(const volatile uint32_t*)(kBoundsStore + 0x50);
    if (slot < 0 || (uint32_t)slot >= size) return unavailable(action, "pack bounds slot not found");
    const uintptr_t defs = *(const volatile uintptr_t*)(kBoundsStore + 0x40);
    const uintptr_t flags = *(const volatile uintptr_t*)(kBoundsStore + 0x48);
    const uintptr_t def = defs ? defs + (uintptr_t)(uint32_t)slot * 0x10u : 0;
    if (!def || !flags || !pack_readable(def, 0x10) ||
        *(const volatile int8_t*)(flags + (uintptr_t)(uint32_t)slot) < 0)
      return unavailable(action, "pack bounds slot unreadable or unused");
    if (*(const volatile uintptr_t*)def != 0 || (pack_bounds_status(slot) & 3u) != 0u)
      return unavailable(action, "pack bounds slot is not a fresh archive slot");
    g_pack_bounds_slots[param] = slot;
    g_pack_bounds_records[param] = *(const volatile uint32_t*)(kBoundsStore + 0x1d8);
    g_pack_bounds_requested |= 1u << param;
    gtav_worker_klog("GTAVMenu pack bounds request before");
    ((RequestObjectFn)kRequestObject)(*(const volatile uint32_t*)(kBoundsStore + 8) + (uint32_t)slot,
                                      7u);
    gtav_worker_klog("GTAVMenu pack bounds request returned");
    snprintf(message, sizeof(message), "pack bounds %u slot=%d requested status=0x%x; retry",
             (unsigned)param, (int)slot, (unsigned)pack_bounds_status(slot));
    return ok(action, message);
  }
  // Confirm: resident, a composite root with children, the slot box filled, one more instance list.
  const int32_t slot = g_pack_bounds_slots[param];
  const uint32_t status = pack_bounds_status(slot);
  if ((status & 3u) != 1u) {
    snprintf(message, sizeof(message), "pack bounds load pending status=0x%x; retry", (unsigned)status);
    return unavailable(action, message);
  }
  const uintptr_t defs = *(const volatile uintptr_t*)(kBoundsStore + 0x40);
  const uintptr_t bound = defs ? *(const volatile uintptr_t*)(defs + (uintptr_t)(uint32_t)slot * 0x10u) : 0;
  const uintptr_t boxes = *(const volatile uintptr_t*)(kBoundsStore + 0xd0);
  if (!bound || !pack_readable(bound, 0xa4) || !boxes ||
      !pack_readable(boxes + (uintptr_t)(uint32_t)slot * 0x20u, 0x20))
    return unavailable(action, "pack bounds contents pending; retry");
  const uint8_t type = *(const volatile uint8_t*)(bound + 0x10);
  const uint32_t children = type == 10u ? *(const volatile uint16_t*)(bound + 0xa2) : 0u;
  const volatile float* box = (const volatile float*)(boxes + (uintptr_t)(uint32_t)slot * 0x20u);
  const int box_ok = box[0] < box[4] && box[1] < box[5] && box[2] < box[6];
  const uint32_t records = *(const volatile uint32_t*)(kBoundsStore + 0x1d8);
  const int valid = type == 10u && children && box_ok && records > g_pack_bounds_records[param];
  if (valid)
    g_pack_bounds_finished |= 1u << param;
  else
    g_pack_bounds_failed |= 1u << param;
  snprintf(message, sizeof(message),
           "pack bounds %u loaded type=%u children=%u box=%d instances %u->%u", (unsigned)param,
           (unsigned)type, (unsigned)children, box_ok, (unsigned)g_pack_bounds_records[param],
           (unsigned)records);
  pack_world_note("%s", message);  // the row's result in pack-notes too (the status ring overflows)
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack bounds %u box (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)", (unsigned)param,
                     (double)box[0], (double)box[1], (double)box[2], (double)box[4], (double)box[5],
                     (double)box[6]);
  return valid ? ok(action, message) : failed(action, message);
}

// Interiors and map typ dependencies.
// The game binds a map's ITYP dependency from the archive manifest (0x1998d60 live), which the
// changeset path alone runs; a `mapdep` row does the same for one dependency right before the
// bounds-only load: map def +0x18 = typ slot (u32, inline for a count of 1), +0x26 = 1, typ def
// +0x10 |= 8. The bounds-only load then creates an interior proxy per CMloInstanceDef (contentFlags
// 8), and the interior bounds binder (0x1999500 live) attaches the MLO's .pbn to it.
static const uintptr_t kInteriorProxyPool = 0x5753cb0;  // pool*: +8 data, +0x10 flags, +0x18 size
static const uintptr_t kInteriorBoundsBinder = 0x1999500;  // (changeset group, 0 = all)
static const uint8_t kInteriorBoundsBinderBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x1d, 0x45, 0x65, 0x71, 0x02, 0x48, 0x8b, 0x03, 0x48, 0x89};
static uint8_t g_pack_map_bound[GTAV_CUSTOM_PACK_MAP_CAP];  // interior binder ran for this row
// After the binder the game rebuilds two box-streamer trees (changeset executor 0x1597961..,
// startup 0x15c4fb5.., normalized): the map store's (vt+0x38), then the bounds store's (vt+0x18),
// which re-indexes every bounds slot with a box. REQUEST_COLLISION_AT_COORD and proximity loading
// search that tree, so without the rebuild the binder's new alias slot is never streamed in
// (seen on hardware: doors only, no floor).
static const uintptr_t kMapBoxStreamer = 0x61a9b50;
static const uintptr_t kMapBoxStreamerVtable = 0x4094938;
static const uintptr_t kMapBoxRebuild = 0x3511aa0;  // vt+0x38
static const uint8_t kMapBoxRebuildBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x18, 0x48, 0x8b, 0x05, 0xa8, 0xdf, 0xb9, 0x00, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x00, 0x48, 0x89};
static const uintptr_t kBoundsBoxStreamer = 0x61a97f0;
static const uintptr_t kBoundsBoxStreamerVtable = 0x40948f8;
static const uintptr_t kBoundsBoxRebuild = 0x3510660;  // vt+0x18
static const uint8_t kBoundsBoxRebuildBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x18, 0x4c, 0x8b, 0x25, 0xe8, 0xf3, 0xb9, 0x00, 0x48, 0x89, 0xfb, 0x49, 0x8b, 0x04, 0x24, 0x48};
// The last bound interior, for the proximity diagnostics (pack_interior_diag_tick).
static volatile int32_t g_pack_interior_proxy = -1;
static volatile int32_t g_pack_interior_alias = -1;
static volatile int32_t g_pack_interior_source = -1;  // the pack .pbn's bounds slot

static int pack_box_trees_ok() {
  return *(const volatile uintptr_t*)kMapBoxStreamer == kMapBoxStreamerVtable &&
         *(const volatile uintptr_t*)kBoundsBoxStreamer == kBoundsBoxStreamerVtable &&
         ((const volatile uintptr_t*)kMapBoxStreamerVtable)[0x38 / 8] == kMapBoxRebuild &&
         ((const volatile uintptr_t*)kBoundsBoxStreamerVtable)[0x18 / 8] == kBoundsBoxRebuild &&
         memcmp((const void*)kMapBoxRebuild, kMapBoxRebuildBytes, sizeof(kMapBoxRebuildBytes)) == 0 &&
         memcmp((const void*)kBoundsBoxRebuild, kBoundsBoxRebuildBytes,
                sizeof(kBoundsBoxRebuildBytes)) == 0;
}

// The interior proxy pool, read-only. Returns 0 and fills the fields when it is usable.
static int pack_proxy_pool(uintptr_t* data, uintptr_t* flags, int32_t* size, int32_t* element) {
  const uintptr_t pool = *(const volatile uintptr_t*)kInteriorProxyPool;
  if (!pool || !pack_readable(pool, 0x2c)) return -1;
  *data = *(const volatile uintptr_t*)(pool + 8);
  *flags = *(const volatile uintptr_t*)(pool + 0x10);
  *size = *(const volatile int32_t*)(pool + 0x18);
  *element = *(const volatile int32_t*)(pool + 0x1c);
  if (*size <= 0 || *size > 4096 || *element < 0xd0 || !pack_readable(*flags, (size_t)*size) ||
      !pack_readable(*data, (size_t)*size * (size_t)*element))
    return -1;
  const uint32_t used = *(const volatile uint32_t*)(pool + 0x28) & 0x3fffffffu;
  return *(const volatile int32_t*)(pool + 0x20) != -1 && used < (uint32_t)*size ? 0 : -2;
}

// Bind map row `row`'s typ dependency into map def `def` (before its first load). Returns nullptr
// when bound or when the row has none, "retry: " + reason to wait, or why it cannot be bound.
// A stock dependency (`mapdep ... retail`) is a typ no row requests: the map's GetDependencies
// (vt+0xe0, 0x351c580 live) returns it once the slot is initialised, so the keep-resident reload
// streams it before the map, like a retail map's manifest dependency. The binder skips one with
// +0x10 & 0x14 (permanent or DLC-requested): its archetypes create entities without the binding.
static const char* pack_bind_map_dependency(uint32_t row, uintptr_t def) {
  const uint32_t dep = g_pack.map_dep_typ[row];
  const char* stock = g_pack.map_dep_retail[row] ? g_pack.map_dep_name[row] : "";
  // A mapdep naming another pack's typ is bound by the merge; unbound, that pack is not active.
  if (!dep && !stock[0])
    return g_pack.map_dep_name[row][0] ? "pack map dependency typ is in no active pack" : nullptr;
  char name[GTAV_CUSTOM_PACK_NAME_MAX];
  int32_t typ_slot = -1;
  uintptr_t typ_def = 0;
  uint32_t status = UINT32_MAX, handle = 0;
  if (const char* refusal =
          dep ? pack_typ_slot_def(dep - 1u, name, sizeof(name), &typ_slot, &typ_def)
              : pack_typ_named_slot_def(stock, name, sizeof(name), &typ_slot, &typ_def, &status,
                                        &handle))
    return refusal;
  if (*(const volatile uint16_t*)(def + 0x26) != 0u) return "pack map already has dependencies";
  if (!dep) {
    const uint32_t flags = *(const volatile uint16_t*)(typ_def + 0x10);
    const uint32_t archetypes_free = *(const volatile uint32_t*)kArchetypePoolFree;
    if (flags & 0x14u) {
      pack_world_note("pack map %u stock typ %s slot=%d flags=0x%x status=0x%x not bound (resident)",
                      (unsigned)row, name, (int)typ_slot, (unsigned)flags, (unsigned)status);
      return nullptr;
    }
    if (!handle && !(status & 0x2000000u)) return "map dependency typ has no streaming file";
    if ((status & 3u) != 1u && archetypes_free < kRetailTypHeadroom)
      return "retry: archetype pool headroom gate refused for a map's stock typ";
  }
  if (g_pack.map_interior[row]) {
    // Verified by a hardware crash: the MLO archetype keeps
    // raw pointers into the typ's loaded data (rooms, portals, entities), so that data must still
    // be there; load_pack_typ asked the store to keep it (+0x10 |= 0x200).
    const uintptr_t contents = *(const volatile uintptr_t*)typ_def;
    if (!contents || !pack_readable(contents, 0x18) || !*(const volatile uintptr_t*)(contents + 0x10))
      return "interior typ meta freed (keep bit missing); restart GTA and load again";
  }
  if (g_pack.map_interior[row]) {
    uintptr_t data = 0, flags = 0;
    int32_t size = 0, element = 0;
    const int pool = pack_proxy_pool(&data, &flags, &size, &element);
    if (pool != 0)
      return pool == -2 ? "interior proxy pool full" : "interior proxy pool unreadable";
  }
  *(volatile uint32_t*)(def + 0x18) = (uint32_t)typ_slot;
  *(volatile uint16_t*)(def + 0x26) = 1u;
  *(volatile uint8_t*)(typ_def + 0x10) |= 8u;
  pack_world_note("pack map %u bound typ %s slot=%d interior=%u%s", (unsigned)row, name,
                  (int)typ_slot, (unsigned)g_pack.map_interior[row], dep ? "" : " stock=1");
  return nullptr;
}

// Interior row, bounds-only load done: find the proxy the load created for map `slot` and, when the
// pack ships the MLO's .pbn (a bounds row named like the MLO archetype, already resident), run the
// game's interior bounds binder once. Returns nullptr to continue, "retry:" + reason to wait, or
// the failure.
static const char* pack_finish_interior(uint32_t row, int32_t slot) {
  typedef void (*BinderFn)(uint32_t);
  uintptr_t data = 0, flags = 0;
  int32_t size = 0, element = 0;
  if (pack_proxy_pool(&data, &flags, &size, &element) == -1) return "interior proxy pool unreadable";
  uintptr_t proxy = 0;
  int32_t index = -1;
  for (int32_t i = 0; i < size && !proxy; ++i) {
    if (*(const volatile int8_t*)(flags + (uintptr_t)i) < 0) continue;
    const uintptr_t entry = data + (uintptr_t)i * (uintptr_t)element;
    if (*(const volatile int32_t*)(entry + 8) == slot) {
      proxy = entry;
      index = i;
    }
  }
  if (!proxy) return "no interior proxy (contentFlags 8 / dependency)";
  const uint32_t mlo = *(const volatile uint32_t*)(proxy + 0xc4);
  pack_world_note("pack map %u interior proxy=%d mlo=0x%08x at (%.1f,%.1f,%.1f) bounds=%d",
                  (unsigned)row, (int)index, (unsigned)mlo,
                  (double)*(const volatile float*)(proxy + 0x50),
                  (double)*(const volatile float*)(proxy + 0x54),
                  (double)*(const volatile float*)(proxy + 0x58),
                  (int)*(const volatile int32_t*)(proxy + 0xc));
  int bounds_row = -1;
  for (int b = 0; b < g_pack.bounds_count && bounds_row < 0; ++b) {
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    snprintf(name, sizeof(name), "%s", g_pack.bounds[b]);
    if (char* dot = strrchr(name, '.')) *dot = '\0';
    if (pack_joaat(name) == mlo) bounds_row = b;
  }
  if (bounds_row < 0 || g_pack_map_bound[row]) return nullptr;  // no collision shipped, or done
  if (!(g_pack_bounds_finished & (1u << bounds_row)))
    return "retry: interior bounds not resident yet (load the bounds row first)";
  if (memcmp((const void*)kInteriorBoundsBinder, kInteriorBoundsBinderBytes,
             sizeof(kInteriorBoundsBinderBytes)) != 0)
    return "interior bounds binder anchors mismatch";
  if (!pack_box_trees_ok()) return "interior box streamer anchors mismatch";
  g_pack_map_bound[row] = 1;
  gtav_worker_klog("GTAVMenu pack interior bind before");
  ((BinderFn)kInteriorBoundsBinder)(0u);
  gtav_worker_klog("GTAVMenu pack interior bind returned");
  const int32_t alias = *(const volatile int32_t*)(proxy + 0xc);
  pack_world_note("pack map %u interior bounds alias=%d", (unsigned)row, (int)alias);
  if (alias < 0) return "interior bounds binder attached nothing";
  typedef void (*RebuildFn)(uintptr_t);
  gtav_worker_klog("GTAVMenu pack interior trees before");
  ((RebuildFn)kMapBoxRebuild)(kMapBoxStreamer);
  ((RebuildFn)kBoundsBoxRebuild)(kBoundsBoxStreamer);
  gtav_worker_klog("GTAVMenu pack interior trees returned");
  const uintptr_t boxes = *(const volatile uintptr_t*)(kBoundsStore + 0xd0);
  const uintptr_t at = boxes + (uintptr_t)(uint32_t)alias * 0x20u;
  const volatile float* box = boxes && pack_readable(at, 0x20) ? (const volatile float*)at : nullptr;
  const int box_ok = box && box[0] < box[4] && box[1] < box[5] && box[2] < box[6];
  pack_world_note("pack map %u interior trees rebuilt alias=%d box=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
                  (unsigned)row, (int)alias, box ? (double)box[0] : 0.0, box ? (double)box[1] : 0.0,
                  box ? (double)box[2] : 0.0, box ? (double)box[4] : 0.0, box ? (double)box[5] : 0.0,
                  box ? (double)box[6] : 0.0);
  g_pack_interior_proxy = index;
  g_pack_interior_alias = alias;
  g_pack_interior_source = g_pack_bounds_slots[bounds_row];
  return box_ok ? nullptr : "interior alias box empty after the tree rebuild";
}

// Whether the last bound interior's collision (its alias bounds slot) is resident; 1 when no
// interior is bound. Holds an interior teleport's freeze (on hardware the alias streamed in ~5 s
// after the teleport released the player).
static int pack_interior_collision_ready() {
  const int32_t alias = g_pack_interior_alias;
  return alias < 0 || (pack_bounds_status(alias) & 3u) == 1u;
}

// The interior's streaming wait (on hardware the proxy stays in state 1 while modules ask for 2
// and 3). State 1 advances only when every entry of the request array at inst +0x198 has loaded
// (0x159c44d -> 0x15a4510 normalized): entries {u32 slot, u16 module << 8 | flags} (module 0x7f:
// unused), count i32 at +0x200 (<= 64); the module table [0x62271c8] gives the store, whose +8 is
// its streaming base. Fills `line` (when non-null) with the count, the pending count and up to six
// pending module/slot/status triples, the module as its store address (e.g. 0x61a6cd8 drawables,
// 0x6210a10 typs); returns the pending count (-1: no readable request array) and a hash of the
// pending set in `*set`. Read-only. kStreamingModules (0x62271c8) is custom_rpf.inc's.
static int pack_interior_wait_scan(uintptr_t inst, uint32_t* set, char* line, size_t line_size) {
  *set = 0;
  if (line && line_size) line[0] = '\0';
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  const uintptr_t modules = *(const volatile uintptr_t*)kStreamingModules;
  if (!inst || !infos || !modules || !pack_readable(inst + 0xc8, 0xd8)) return -1;
  const uintptr_t req = *(const volatile uintptr_t*)(inst + 0x198);
  const unsigned inst_flags = *(const volatile uint16_t*)(inst + 0xc8);
  if (!req || !pack_readable(req, 0x204)) {
    if (line)
      snprintf(line, line_size, "GTAVMenu pack interior wait inst_flags=%04x req=%p", inst_flags,
               (void*)req);
    return -1;
  }
  const int32_t count = *(const volatile int32_t*)(req + 0x200);
  int pending = 0;
  uint32_t hash = 2166136261u;  // FNV-1a over the pending {module, slot} pairs
  int used = line ? snprintf(line, line_size, "GTAVMenu pack interior wait inst_flags=%04x n=%d",
                             inst_flags, (int)count)
                  : 0;
  for (int32_t i = 0; i < count && i < 64; ++i) {
    const uint32_t slot = *(const volatile uint32_t*)(req + (uintptr_t)i * 8u);
    const unsigned bits = *(const volatile uint16_t*)(req + (uintptr_t)i * 8u + 4u);
    if (slot == UINT32_MAX || (bits & 0x7f00u) == 0x7f00u) continue;
    const uintptr_t module = *(const volatile uintptr_t*)(modules + (uintptr_t)(bits >> 8 & 0x7fu) * 8u);
    if (!module || !pack_readable(module + 8, 4)) continue;
    const uint32_t global = *(const volatile uint32_t*)(module + 8) + slot;
    const uint32_t status = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u + 4u);
    if ((status & 3u) == 1u) continue;
    hash = (hash ^ (bits >> 8 & 0x7fu)) * 16777619u;
    hash = (hash ^ slot) * 16777619u;
    if (++pending <= 6 && line && used > 0 && (size_t)used < line_size)
      used += snprintf(line + used, line_size - (size_t)used, " %p:%u:%x", (void*)module,
                       (unsigned)slot, (unsigned)status);
  }
  if (line && used > 0 && (size_t)used < line_size)
    snprintf(line + used, line_size - (size_t)used, " pending=%d", pending);
  *set = hash;
  return pending;
}

// What the interior diagnostics compare between samples; a difference logs a line.
struct PackInteriorDiagKey {
  uint64_t req;
  uintptr_t inst;
  uint32_t pending_set;
  int32_t pending;
  uint16_t flags;
  uint16_t rooms;
  uint16_t alias_state;
  uint16_t inst_flags;
  uint8_t state;
  uint8_t alias_status;
  uint8_t source_status;
  uint8_t in_range;
};

static int pack_interior_diag_key_equal(const PackInteriorDiagKey& a, const PackInteriorDiagKey& b) {
  return a.req == b.req && a.inst == b.inst && a.pending_set == b.pending_set &&
         a.pending == b.pending && a.flags == b.flags && a.rooms == b.rooms &&
         a.alias_state == b.alias_state && a.inst_flags == b.inst_flags && a.state == b.state &&
         a.alias_status == b.alias_status && a.source_status == b.source_status &&
         a.in_range == b.in_range;
}

// Game thread, every frame (work on every 15th): event-driven diagnostics for the last bound
// interior while the player is within 60 m of it. A sample reads the proxy state/flags/module
// requests, the instance's rooms and flags, the alias and source bounds status and the pending set
// of the instance's streaming wait. A klog line pair (diag + wait) is written when any of these
// changed (at most one per 30 frames, one per 300 frames after 60 change lines so a flapping field
// cannot spend the budget; a change inside the gap is logged by a later sample), on
// entering/leaving the range, and as a heartbeat every 600 frames (~10 s) while nothing changes;
// at most 240 pairs per process (a hardware run lost its state change to the old 20-line cap).
// Read-only; the ground probe runs only for a logged line.
__attribute__((unused)) static void pack_interior_diag_tick() {
  static const uint32_t kSampleFrames = 15u;
  static const uint32_t kChangeGapFrames = 30u;
  static const uint32_t kHeartbeatFrames = 600u;
  static const uint32_t kMaxLines = 240u;
  static uint32_t frames;
  static uint32_t last_logged;
  static uint32_t lines;
  static uint32_t changes;
  static int logged_once;
  static PackInteriorDiagKey last;
  const int32_t index = g_pack_interior_proxy;
  if (index < 0 || lines > kMaxLines || ++frames % kSampleFrames != 0u) return;
  if (!g_n.get_entity_coords || !g_n.get_ground_z_for_3d_coord) return;
  if (lines == kMaxLines) {
    ++lines;
    gtav_worker_klog("GTAVMenu pack interior diag cap reached (240 lines); no further samples");
    return;
  }
  uintptr_t data = 0, flags = 0;
  int32_t size = 0, element = 0;
  if (pack_proxy_pool(&data, &flags, &size, &element) == -1 || index >= size) return;
  const uintptr_t proxy = data + (uintptr_t)index * (uintptr_t)element;
  const Vec3 player = invoke_vector(g_n.get_entity_coords, native_player_ped_id(), 1);
  const float dx = player.x - *(const volatile float*)(proxy + 0x50);
  const float dy = player.y - *(const volatile float*)(proxy + 0x54);
  const float dz = player.z - *(const volatile float*)(proxy + 0x58);
  PackInteriorDiagKey key = {};
  key.in_range = dx * dx + dy * dy + dz * dz <= 3600.0f ? 1u : 0u;
  if (!key.in_range && !last.in_range) return;  // out of range, and logged as such (or never in)
  memcpy(&key.req, (const void*)(proxy + 0x16), sizeof(key.req));
  key.inst = *(const volatile uintptr_t*)(proxy + 0x28);
  key.rooms = key.inst && pack_readable(key.inst + 0x130, 2)
                  ? *(const volatile uint16_t*)(key.inst + 0x130)
                  : 0u;
  key.inst_flags = key.inst && pack_readable(key.inst + 0xc8, 2)
                       ? *(const volatile uint16_t*)(key.inst + 0xc8)
                       : 0u;
  key.state = *(const volatile uint8_t*)(proxy + 0x1e);
  key.flags = *(const volatile uint16_t*)(proxy + 0x14);
  const int32_t alias = g_pack_interior_alias;
  const int32_t source = g_pack_interior_source;
  const uintptr_t def = *(const volatile uintptr_t*)(kBoundsStore + 0x40) + (uintptr_t)(uint32_t)alias * 0x10u;
  const uintptr_t state = *(const volatile uintptr_t*)(kBoundsStore + 0xe0) + (uintptr_t)(uint32_t)alias * 2u;
  const uintptr_t bound = pack_readable(def, 8) ? *(const volatile uintptr_t*)def : 0;
  key.alias_state = pack_readable(state, 2) ? *(const volatile uint16_t*)state : 0xffffu;
  key.alias_status = (uint8_t)(pack_bounds_status(alias) & 3u);
  key.source_status = source >= 0 ? (uint8_t)(pack_bounds_status(source) & 3u) : 9u;
  key.pending = pack_interior_wait_scan(key.inst, &key.pending_set, nullptr, 0);
  const uint32_t since = frames - last_logged;
  const uint32_t gap = changes < 60u ? kChangeGapFrames : kChangeGapFrames * 10u;
  const int changed = !logged_once || !pack_interior_diag_key_equal(key, last);
  const char* reason = nullptr;
  if (changed && key.in_range != last.in_range)
    reason = key.in_range ? "enter" : "leave";
  else if (changed && since >= gap)
    reason = "change";
  else if (!changed && since >= kHeartbeatFrames)
    reason = "beat";
  if (!reason) return;
  last = key;
  last_logged = frames;
  logged_once = 1;
  if (reason[0] == 'c') ++changes;
  ++lines;
  float ground = 0.0f;
  const int grounded = invoke_return<int>(g_n.get_ground_z_for_3d_coord, player.x, player.y, 1500.0f,
                                          (uint64_t)(uintptr_t)&ground, 0, 0);
  // Heartbeats carry a different tag so they stay klog-only: the pack-notes mirror (klog.c) keeps
  // "GTAVMenu pack ..." lines, and 10 s beats pushed a hardware run's load lines out of its ring.
  const int beat = reason[0] == 'b';
  const char* tag = beat ? "GTAVMenu (beat)" : "GTAVMenu pack";
  char line[416];
  snprintf(line, sizeof(line),
           "%s interior diag %s #%u/%u t=%us proxy=%d state=%02x flags=%04x req=%016llx "
           "inst=%p rooms=%u alias=%d alias_status=%u alias_bound=%p alias_state=%04x src_status=%u "
           "bounds_insts=%u player=(%.1f,%.1f,%.1f) groundz=%.2f/%d",
           tag, reason, (unsigned)lines, (unsigned)changes, (unsigned)(frames / 60u), (int)index,
           (unsigned)key.state, (unsigned)key.flags, (unsigned long long)key.req, (void*)key.inst,
           (unsigned)key.rooms, (int)alias, (unsigned)key.alias_status, (void*)bound,
           (unsigned)key.alias_state, (unsigned)key.source_status,
           (unsigned)*(const volatile uint32_t*)(kBoundsStore + 0x1d8), (double)player.x,
           (double)player.y, (double)player.z, (double)ground, grounded);
  gtav_worker_klog(line);
  if (!key.in_range) return;
  uint32_t set = 0;
  char wait[384];
  pack_interior_wait_scan(key.inst, &set, wait, sizeof(wait));
  if (!wait[0]) return;
  if (beat && strncmp(wait, "GTAVMenu pack ", 14) == 0) {
    char tagged[sizeof(wait) + 8];
    snprintf(tagged, sizeof(tagged), "GTAVMenu (beat) %s", wait + 14);
    gtav_worker_klog(tagged);
  } else {
    gtav_worker_klog(wait);
  }
}

static uint32_t load_pack_map(uint32_t param) {
  typedef int32_t (*FindSlotFn)(uintptr_t, const char*);
  typedef void (*RequestObjectFn)(uint32_t, uint32_t);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.map_count) return unavailable(action, "pack map row out of range");
  if (g_pack_maps_requested & (1u << param)) return unavailable(action, "pack map row already requested");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!pack_archives_resident())
    return unavailable(action, "pack archive is not resident; run pack-load-archive first");
  if (!pack_map_store_ok()) return unavailable(action, "pack map store anchors mismatch");
  const int typs_ready = pack_typs_resident();
  g_pack_map_waiting_typs = !typs_ready;
  if (!typs_ready) return unavailable(action, "pack typs not resident yet; retry");
  char name[GTAV_CUSTOM_PACK_NAME_MAX];
  pack_map_basename(param, name, sizeof(name));
  const int32_t slot = ((FindSlotFn)kMapFindSlot)(kMapStore, name);
  const uint32_t size = *(const volatile uint32_t*)(kMapStore + 0x50);
  if (slot < 0 || (uint32_t)slot >= size) return unavailable(action, "pack map slot not found");
  const uintptr_t def = pack_map_def(slot);
  const uintptr_t flags = *(const volatile uintptr_t*)(kMapStore + 0x48);
  if (!def || !pack_readable(def, 0x28) || !flags ||
      *(const volatile int8_t*)(flags + (uintptr_t)(uint32_t)slot) < 0)
    return unavailable(action, "pack map slot unreadable or unused");
  const uint32_t def_flags = *(const volatile uint32_t*)(def + 0x20);
  const uint32_t status = pack_map_status(slot);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack map %s slot=%d def=0x%x status=0x%x", name,
                     (int)slot, (unsigned)def_flags, (unsigned)status);
  if (!(def_flags & 0x2000u) || (def_flags & 0xc00u) == 0x800u || (status & 3u) != 0u)
    return unavailable(action, "pack map slot is not a fresh archive slot");
  if (const char* refusal = pack_bind_map_dependency(param, def)) {
    if (strncmp(refusal, "retry: ", 7) == 0) return unavailable(action, refusal + 7);
    g_pack_maps_failed |= 1u << param;  // the typ will not change: never retried
    return failed(action, refusal);
  }
  g_pack_maps_requested |= 1u << param;
  g_pack_map_slots[param] = slot;
  gtav_worker_klog("GTAVMenu pack map request before");
  ((RequestObjectFn)kRequestObject)(*(const volatile uint32_t*)(kMapStore + 8) + (uint32_t)slot, 6u);
  gtav_worker_klog("GTAVMenu pack map request returned");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack map %u slot=%d requested status=0x%x", (unsigned)param,
           (int)slot, (unsigned)pack_map_status(slot));
  return ok(action, message);
}

static uint32_t finish_pack_map(uint32_t param) {
  typedef void (*EnableIplFn)(int32_t, uint32_t, uint32_t);
  typedef bool (*RemoveObjectFn)(uint32_t);
  typedef void (*RequestObjectFn)(uint32_t, uint32_t);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP;
  if (param >= (uint32_t)g_pack.map_count || !(g_pack_maps_requested & (1u << param)))
    return unavailable(action, "pack map row not requested");
  if ((g_pack_maps_finished | g_pack_maps_failed) & (1u << param))
    return unavailable(action, "pack map row already finished");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!pack_map_store_ok()) return unavailable(action, "pack map store anchors mismatch");
  const int32_t slot = g_pack_map_slots[param];
  const uint32_t global = *(const volatile uint32_t*)(kMapStore + 8) + (uint32_t)slot;
  const uint32_t status = pack_map_status(slot);
  if ((status & 3u) != 1u) {
    char message[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(message, sizeof(message), "pack map load pending status=0x%x; retry", (unsigned)status);
    return unavailable(action, message);
  }
  const uintptr_t def = pack_map_def(slot);
  if (!def || !pack_readable(def, 0x28)) return unavailable(action, "pack map def unreadable");
  char name[GTAV_CUSTOM_PACK_NAME_MAX];
  pack_map_basename(param, name, sizeof(name));

  if (g_pack_map_reloaded[param]) {
    // (b) The reload ran with the slot initialised: the contents now carry the entities.
    const uintptr_t contents = *(const volatile uintptr_t*)def;
    if (!contents || !pack_readable(contents, 0x20)) return unavailable(action, "pack map contents pending; retry");
    const uint32_t entities = *(const volatile uint32_t*)(contents + 0x18);
    const uintptr_t array = *(const volatile uintptr_t*)(contents + 0x10);
    if (!entities || !array) return unavailable(action, "pack map entities pending; retry");
    const uintptr_t states = *(const volatile uintptr_t*)(kMapStore + 0xe0);
    int enabled = 0;
    if (states && pack_readable(states + (uintptr_t)(uint32_t)slot * 2u, 2) &&
        (*(const volatile uint16_t*)(states + (uintptr_t)(uint32_t)slot * 2u) & 0x4u)) {
      ((EnableIplFn)kMapEnableIpl)(slot, pack_joaat(name), 0u);
      enabled = 1;
    }
    float x = 0, y = 0, z = 0;
    const uintptr_t first = pack_readable(array, 8) ? *(const volatile uintptr_t*)array : 0;
    if (first && pack_readable(first + 0x90, 12)) {
      x = *(const volatile float*)(first + 0x90);
      y = *(const volatile float*)(first + 0x94);
      z = *(const volatile float*)(first + 0x98);
    }
    g_pack_maps_finished |= 1u << param;
    char message[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(message, sizeof(message), "pack map %u entities=%u at (%.1f,%.1f,%.1f) ipl=%d",
             (unsigned)param, (unsigned)entities, (double)x, (double)y, (double)z, enabled);
    pack_world_note("%s", message);  // the run's pass line, kept in pack-notes
    return ok(action, message);
  }

  // (a) The bounds-only load finished: check the extents landed, then remove and reload it
  // initialised.
  const uintptr_t bounds = *(const volatile uintptr_t*)(kMapStore + 0xd0);
  const uintptr_t states = *(const volatile uintptr_t*)(kMapStore + 0xe0);
  if (!bounds || !states || !pack_readable(bounds + (uintptr_t)(uint32_t)slot * 0x20u, 0x20) ||
      !pack_readable(states + (uintptr_t)(uint32_t)slot * 2u, 2))
    return unavailable(action, "pack map bounds/state unreadable");
  const volatile float* box = (const volatile float*)(bounds + (uintptr_t)(uint32_t)slot * 0x20u);
  const int bounds_ok = box[0] < box[4] && box[1] < box[5] && box[2] < box[6];
  // Kept in pack-notes: which of the map's extents the store took as the slot box.
  pack_world_note("pack map %u bounds (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) ok=%d", (unsigned)param,
                  (double)box[0], (double)box[1], (double)box[2], (double)box[4], (double)box[5],
                  (double)box[6], bounds_ok);
  if (!bounds_ok) return unavailable(action, "pack map bounds not filled; the store refused the file");
  if ((status & 0xfffcu) != 0u) return unavailable(action, "pack map slot has other requirement flags");
  const uint32_t def_flags = *(const volatile uint32_t*)(def + 0x20);
  if ((def_flags & 0xc00u) == 0x800u) return unavailable(action, "pack map slot already initialised");
  if (g_pack.map_interior[param]) {
    if (const char* problem = pack_finish_interior(param, slot)) {
      if (strncmp(problem, "retry: ", 7) == 0) return unavailable(action, problem + 7);
      g_pack_maps_failed |= 1u << param;  // never retry a half-done interior
      return failed(action, problem);
    }
  }
  gtav_worker_klog("GTAVMenu pack map remove before");
  const bool removed = ((RemoveObjectFn)kRemoveObject)(global);
  gtav_worker_klog("GTAVMenu pack map remove returned");
  if (!removed || (pack_map_status(slot) & 3u) != 0u) {
    g_pack_maps_failed |= 1u << param;  // never retry a half-done reload
    return failed(action, "pack map bounds load could not be removed");
  }
  const uint32_t content = *(const volatile uint32_t*)(def + 0x24);
  const uint16_t mask = (content & 2u)       ? 0x140u
                        : (content & 0x184u) ? 0x180u
                        : (content & 0x600u) ? 0x100u
                                             : 0xc0u;
  volatile uint16_t* state = (volatile uint16_t*)(states + (uintptr_t)(uint32_t)slot * 2u);
  const uint16_t old_state = *state;
  *(volatile uint32_t*)(def + 0x20) = (def_flags & ~(0xc00u | (3u << 17))) | 0x800u;
  *state = (uint16_t)((old_state & 0x3fu) | mask);
  g_pack_map_reloaded[param] = 1;
  gtav_worker_klog("GTAVMenu pack map reload before");
  ((RequestObjectFn)kRequestObject)(global, 7u);
  gtav_worker_klog("GTAVMenu pack map reload returned");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack map %u initialised state 0x%x->0x%x; reload pending; retry",
           (unsigned)param, (unsigned)old_state, (unsigned)*state);
  return unavailable(action, message);
}
