// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_DEVICE && CUSTOM_STREAM && PS5 branch of the runtime pack lane.
//
// Pack data files: pinned LoadDataFile mounters, loose-entry fill and the parser pump.

// Data-file mounters: sm_Interfaces[type] is a live
// object whose vtable slot 2 is LoadDataFile(iface, const DataFile*), the same tail call the game's
// changeset executor makes (0x159b633 normalized). The vehicle loaders read only the DataFile's
// filename (+0), an absolute path read synchronously through GetDevice. Mounters only add new
// names: an existing handling or model name is skipped, never replaced.
static const uintptr_t kDataFileInterfaces = 0x598a350;  // void*[208]
static const uint32_t kDataFileInterfaceCount = 208;
// Route: queued rows leave exactly one parser request for pump_pack_data; sync rows (peds.meta,
// weaponarchetypes.meta) parse inside LoadDataFile.
// `names` is the XML element whose text lists the row's new model or weapon names (nullptr: none
// counted), used for headroom gates and the post-load check; with `anchor`, only the first such
// element after each anchor counts (a CWeaponInfo's own <Name>, not its components').
static const struct {
  uint32_t type;
  uintptr_t iface;
  uintptr_t vtable;
  uintptr_t load;
  uint8_t sync;
  const char* names;
  const char* anchor;
  uint8_t pin[32];
} kDataFileMounters[] = {
    {GTAV_CUSTOM_PACK_DATA_HANDLING, 0x5584be8, 0x4032b48, 0x27b6150, 0, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x05, 0xf8, 0x98, 0x8f, 0x01, 0x48, 0x89, 0xb5, 0x60, 0xff, 0xff, 0xff, 0x48}},
    {GTAV_CUSTOM_PACK_DATA_CARCOLS, 0x45a4790, 0x3f90ec0, 0x14138e0, 0, "kitName", nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x65, 0xc1, 0xc9, 0x02, 0x48, 0x89, 0xb5, 0x58, 0xff}},
    {GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA, 0x45a4780, 0x3f90e20, 0x1413370, 0, "modelName", nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xde, 0xc6, 0xc9, 0x02, 0x48, 0x8d, 0x0d, 0xc7, 0xda, 0xb7, 0x02, 0x48, 0x8d, 0x5d, 0xb0, 0x48, 0x89, 0xf7}},
    {GTAV_CUSTOM_PACK_DATA_VEHICLE_VARIATION, 0x45a4788, 0x3f90e90, 0x14137d0, 0, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x78, 0x4c, 0x8b, 0x35, 0x7c, 0xc2, 0xc9, 0x02, 0x48, 0x8d, 0x5d, 0xb0, 0x4c, 0x8d, 0x7d, 0x80, 0x48, 0x8d, 0x15, 0xca}},
    // Vehicle layouts: same queue-and-pump path; load before vehicles.meta.
    {GTAV_CUSTOM_PACK_DATA_VEHICLE_LAYOUTS, 0x5618338, 0x4032fe0, 0x27b6dd0, 0, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x78, 0x4c, 0x8b, 0x35, 0x7c, 0x8c, 0x8f, 0x01, 0x48, 0x8d, 0x5d, 0xb0, 0x4c, 0x8d, 0x7d, 0x80, 0x48, 0x8d, 0x15, 0xe9}},
    // Shared ped mounter (types 20/21/51/53/55/71), dispatching on DataFile+0x90.
    {GTAV_CUSTOM_PACK_DATA_PED_METADATA, 0x45a41c0, 0x3f90df0, 0x1412590, 1, "Name", nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x3d, 0xb5, 0xd4, 0xc9, 0x02, 0x49, 0x8b, 0x07, 0x48, 0x89}},
    // Weapon models (sync; 0x15ba760 normalized adds each InitData by name, no name or capacity
    // check). Rows load in descriptor order: list this row before the weapons.meta using it.
    {GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA, 0x53fb1f8, 0x3fa7e60, 0x1a46f40, 1, "modelName", nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf7, 0xba, 0x00, 0xf0, 0x00, 0x00, 0x31, 0xf6, 0xe8, 0x0d, 0x38, 0xf7, 0xff, 0xb0, 0x01, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}},
    {GTAV_CUSTOM_PACK_DATA_WEAPONINFO, 0x578e5f8, 0x403cce8, 0x296bac0, 0, "Name", "<Item type=\"CWeaponInfo\">",
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x35, 0x85, 0x3f, 0x74, 0x01, 0x49, 0x89, 0xf7, 0x49, 0x8b}},
    // Weapon components (queued): one <Name> per `<Item type="...Info">` (the Infos), not the
    // `...Data">` items (reload/swap data, named and referenced by ref= only).
    {GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO, 0x578d360, 0x403cc48, 0x296b770, 0, "Name", "Info\">",
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0xde, 0x42, 0x74, 0x01, 0x48, 0x8d, 0x5d, 0xb0, 0x48, 0x89, 0xf7, 0x48, 0x8d, 0x0d, 0xe8, 0x14, 0x6d, 0x01}},
    {GTAV_CUSTOM_PACK_DATA_WEAPON_ANIMATIONS, 0x578dc50, 0x403ccb8, 0x296ba00, 0, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x35, 0x4e, 0x40, 0x74, 0x01, 0x48, 0x8d, 0x5d, 0xb0, 0x48, 0x8d, 0x0d, 0xc3, 0x15, 0x6d, 0x01, 0x48, 0x89, 0xda}},
    // Clothing for an existing ped: the shared apparel mounter (134/137/149/150, `mov ecx,-134` +
    // df+0x90) queues one parse; pack_apparel_gate pins the 137 case and its post-parse step.
    {GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL, 0x53fa8f8, 0x3fa7e00, 0x1a448e0, 0, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x2d, 0x65, 0xb1, 0x66, 0x02, 0xb9, 0x7a, 0xff, 0xff, 0xff}},
    // Timecycle modifiers (sync; 0x31678d0 normalized appends every <modifier name=...>). Names
    // come from pack_tc_modifier_names (an attribute, not element text), so `names` is nullptr.
    {GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD, 0x54fbd20, 0x4017ec0, 0x24fe640, 1, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x10, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x35, 0x0b, 0x14, 0xbb, 0x01, 0x49, 0x8b, 0x06, 0x48, 0x89, 0x45, 0xe8, 0x83, 0xbe, 0x90, 0x00}},
    // Audio game-data chunk (sync; the audio metadata mounter shared by types 138-141/143,
    // pack_audio_gate pins the rest). No XML names: pack_audio_verify checks the chunk itself.
    {GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA, 0x435a0b0, 0x3f51198, 0x98e840, 1, nullptr, nullptr,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x05, 0x08, 0x12, 0x72, 0x03, 0x49, 0x89, 0xfe, 0x48, 0x89, 0xf7, 0x48, 0x8b}},
};
// vehicles.meta stores: CVehicleModelInfo records come from the archetype factory's block for
// mapTypeDef 0xf000 ({data, u16 count, u16 capacity}; the handler increments count without a
// capacity check), and each new model takes one streamed-archetype pool slot (registration is
// silently skipped when none is free).
static const uintptr_t kVehicleModelFactory = 0x45a36d8;  // buckets +8, u16 bucket count +0x10
static const uintptr_t kVehicleModelFactoryVtable = 0x3f8ffe8;
static const uint32_t kVehicleModelMapTypeDef = 0xf000;
static const uintptr_t kArchetypePoolFree = 0x6177260;  // u32
static const uintptr_t kHandlingCount = 0x5584c10;      // u16 count of the handling atArray
static int pack_readable(uintptr_t address, size_t bytes) {
  GtavLooseVqInfo info = {};
  return loose_query(address, &info) && (info.protection & 1) &&
         address + bytes <= (uintptr_t)info.end;
}

// Vehicle variation globals: the carcols
// merge grows the mod-kit, lights and sirens arrays by reallocating them, and vehicle mod render
// data caches raw pointers into the kit arrays. Every pump logs the three {data, count, capacity}
// triples before and after, so a move is visible in each run.
static const uintptr_t kVehicleVariationGlobals = 0x5a8b380;  // CVehicleModelInfoVarGlobal*
struct PackVarArrays {
  uintptr_t kits, lights, sirens;
  uint16_t kit_count, kit_capacity, light_count, light_capacity, siren_count, siren_capacity;
};

static int pack_var_arrays(PackVarArrays* out) {
  memset(out, 0, sizeof(*out));
  const uintptr_t vg = pack_readable(kVehicleVariationGlobals, 8)
                           ? *(const volatile uintptr_t*)kVehicleVariationGlobals
                           : 0;
  if (!vg || !pack_readable(vg + 0x38, 0x2c)) return -1;
  out->lights = *(const volatile uintptr_t*)(vg + 0x38);
  out->light_count = *(const volatile uint16_t*)(vg + 0x40);
  out->light_capacity = *(const volatile uint16_t*)(vg + 0x42);
  out->sirens = *(const volatile uintptr_t*)(vg + 0x48);
  out->siren_count = *(const volatile uint16_t*)(vg + 0x50);
  out->siren_capacity = *(const volatile uint16_t*)(vg + 0x52);
  out->kits = *(const volatile uintptr_t*)(vg + 0x58);
  out->kit_count = *(const volatile uint16_t*)(vg + 0x60);
  out->kit_capacity = *(const volatile uint16_t*)(vg + 0x62);
  return 0;
}

static void pack_log_var_arrays(const char* when, int32_t row, const PackVarArrays* a) {
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack data %d %s kits=%p %u/%u lights=%p %u/%u sirens=%p %u/%u", (int)row,
                     when, (void*)a->kits, (unsigned)a->kit_count, (unsigned)a->kit_capacity,
                     (void*)a->lights, (unsigned)a->light_count, (unsigned)a->light_capacity,
                     (void*)a->sirens, (unsigned)a->siren_count, (unsigned)a->siren_capacity);
}

// Per data row (bit = row): requested, and parsed below. uint64_t: GTAV_CUSTOM_PACK_DATA_CAP rows.
static uint64_t g_pack_data_loaded;
static_assert(GTAV_CUSTOM_PACK_DATA_CAP <= 64, "data row masks are uint64_t");
static uint8_t g_pack_datafile[0x100];

// Verified on hardware: LoadDataFile only queues a
// parser request and an asynchronous read of the file through the loose collection.
//  A. The loose entry's size is filled only when the device reports a file time; the memory
//     device's GetFileTime returns 0, so a memory: name reads 0 bytes. Pre-register the name and
//     store what GetEntry would store for a timed plain file: q0 0, size at +8, time sentinel 1.
//  B. The parse runs only inside the parser pump, which the game's changeset loop calls after each
//     data-file action. The worker pumps from a separate action once exactly our request is
//     pending and its read has completed, so the pump never waits on an unfinished read.
static const uintptr_t kRegisterLoose = 0x2f18730;  // (path, u32* size, u32) -> (coll<<16)|idx
static const uintptr_t kParserPump = 0x2f5ef30;     // (parser)
static const uintptr_t kLockMutex = 0x3790430;      // PLT stubs (mutex*)
static const uintptr_t kUnlockMutex = 0x3790440;
static const uintptr_t kLooseLock = 0x5c815d0;
static const uintptr_t kLooseCollectionPtr = 0x5c85b10;
static const uintptr_t kPackLooseVtable = 0x4071208;
static const uintptr_t kParserPtr = 0x6258910;
static const uintptr_t kParserPumpGuard = 0x5eaf308;  // u8 re-entrancy byte
static const uint8_t kRegisterLooseBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x18, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x1d, 0x15, 0x73, 0x19, 0x01, 0x49, 0x89, 0xfe, 0x48, 0x8d};
static const uint8_t kParserPumpBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0xe8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x35, 0x15, 0x0b, 0x15, 0x01, 0x49, 0x8b, 0x06, 0x48, 0x89};
static const uint8_t kLockMutexBytes[11] = {0xff, 0x25, 0xe2, 0xf7, 0x91, 0x00,
                                            0x68, 0x09, 0x00, 0x00, 0x00};
static const uint8_t kUnlockMutexBytes[11] = {0xff, 0x25, 0xda, 0xf7, 0x91, 0x00,
                                              0x68, 0x0a, 0x00, 0x00, 0x00};
static int32_t g_pack_pending_row = -1;
static uintptr_t g_pack_pending_request;

static int pack_pump_anchors_ok() {
  return memcmp((const void*)kRegisterLoose, kRegisterLooseBytes, sizeof(kRegisterLooseBytes)) ==
             0 &&
         memcmp((const void*)kParserPump, kParserPumpBytes, sizeof(kParserPumpBytes)) == 0 &&
         memcmp((const void*)kLockMutex, kLockMutexBytes, sizeof(kLockMutexBytes)) == 0 &&
         memcmp((const void*)kUnlockMutex, kUnlockMutexBytes, sizeof(kUnlockMutexBytes)) == 0;
}

// The parser's pending-request list: {request** data @+0x70, u16 count @+0x78}, current @+0x80.
static uintptr_t pack_parser() {
  const uintptr_t parser = *(const volatile uintptr_t*)kParserPtr;
  return parser && pack_readable(parser, 0x98) ? parser : 0;
}

// Pre-register `name` in the loose collection and give its entry the real size (defect A).
static const char* pack_fill_loose_entry(const char* name, uint64_t size) {
  typedef uint32_t (*RegisterLooseFn)(const char*, uint32_t*, uint32_t);
  typedef void (*MutexFn)(void*);
  const uintptr_t coll = *(const volatile uintptr_t*)kLooseCollectionPtr;
  if (!coll || !pack_readable(coll, 0x7b8) ||
      *(const volatile uintptr_t*)coll != kPackLooseVtable)
    return "loose collection anchors mismatch";
  uint32_t registered_size = 0;
  const uint32_t handle = ((RegisterLooseFn)kRegisterLoose)(name, &registered_size, 0);
  const uint32_t index = handle & 0xffffu;
  if (handle == UINT32_MAX || (handle >> 16) != 0 || index == 0 ||
      index >= *(const volatile uint32_t*)(coll + 0x7b0))
    return "loose registration refused";
  const char* refusal = nullptr;
  ((MutexFn)kLockMutex)((void*)kLooseLock);
  const uintptr_t chunk = *(const volatile uintptr_t*)(coll + 0x5b0 + (uintptr_t)(index >> 10) * 8u);
  const uintptr_t entry = chunk ? chunk + (uintptr_t)(index & 0x3ffu) * 0x20u : 0;
  const char* entry_path =
      entry && pack_readable(entry, 0x20) ? *(const char* const volatile*)(entry + 0x18) : nullptr;
  if (!entry_path || !pack_readable((uintptr_t)entry_path, strlen(name) + 1) ||
      strcmp(entry_path, name) != 0) {
    refusal = "loose entry does not hold the data name";
  } else if (*(const volatile uint64_t*)entry != 0 ||
             *(const volatile uint64_t*)(entry + 0x10) != 0) {
    refusal = "loose entry already filled";
  } else {
    *(volatile uint64_t*)entry = 0;
    *(volatile uint32_t*)(entry + 8) = (uint32_t)size;
    *(volatile uint64_t*)(entry + 0x10) = 1;
  }
  ((MutexFn)kUnlockMutex)((void*)kLooseLock);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack data loose idx=0x%x size=%u filled=%d",
                     (unsigned)index, (unsigned)size, refusal == nullptr);
  return refusal;
}

// Count and capacity of a model-info factory's block for mapTypeDef 0xf000, walking its buckets
// defensively (the factory's own GetCount/GetCapacity loop forever on a missing key). Returns 0 on
// success. The ped and weapon-model factories share the vehicle factory's shape.
static const uintptr_t kPedModelFactory = 0x45a36f0;
static const uintptr_t kPedModelFactoryVtable = 0x3f90088;
static const uintptr_t kWeaponModelFactory = 0x45a36c0;
static const uintptr_t kWeaponModelFactoryVtable = 0x3f8ff48;

static int pack_model_factory_block(uintptr_t factory, uintptr_t vtable, uint32_t* count,
                                    uint32_t* capacity, uintptr_t* models = nullptr) {
  if (!pack_readable(factory, 0x18) || *(const volatile uintptr_t*)factory != vtable) return -1;
  const uintptr_t buckets = *(const volatile uintptr_t*)(factory + 8);
  const uint32_t bucket_count = *(const volatile uint16_t*)(factory + 0x10);
  if (!bucket_count || bucket_count > 4096u || !pack_readable(buckets, bucket_count * 8u)) return -1;
  for (uint32_t b = 0; b < bucket_count; ++b) {
    uintptr_t node = ((const volatile uintptr_t*)buckets)[b];
    for (int depth = 0; node && depth < 256; ++depth) {
      if (!pack_readable(node, 0x18)) return -1;
      if (*(const volatile uint32_t*)node == kVehicleModelMapTypeDef) {
        const uintptr_t array = *(const volatile uintptr_t*)(node + 8);
        if (!pack_readable(array, 0x10)) return -1;
        *count = *(const volatile uint16_t*)(array + 8);
        *capacity = *(const volatile uint16_t*)(array + 0xa);
        if (models) *models = *(const volatile uintptr_t*)array;  // element stride per factory
        return *count <= *capacity ? 0 : -1;
      }
      node = *(const volatile uintptr_t*)(node + 0x10);
    }
  }
  return -1;
}

static int pack_vehicle_model_store(uint32_t* count, uint32_t* capacity) {
  return pack_model_factory_block(kVehicleModelFactory, kVehicleModelFactoryVtable, count,
                                  capacity);
}

// Archetype (model) map: buckets* @+0, u16 bucket count @+8; node {u32 hash, u16 index @+4,
// next* @+8}. What IS_MODEL_IN_CDIMAGE resolves.
static const uintptr_t kArchetypeMap = 0x6166ae8;

static int pack_model_known(uint32_t hash) {
  const uintptr_t buckets = *(const volatile uintptr_t*)kArchetypeMap;
  const uint32_t bucket_count = *(const volatile uint16_t*)(kArchetypeMap + 8);
  if (!buckets || !bucket_count || !pack_readable(buckets, bucket_count * 8u)) return -1;
  uintptr_t node = ((const volatile uintptr_t*)buckets)[hash % bucket_count];
  for (int depth = 0; node && depth < 256; ++depth) {
    if (!pack_readable(node, 0x10)) return -1;
    if (*(const volatile uint32_t*)node == hash) return 1;
    node = *(const volatile uintptr_t*)(node + 8);
  }
  return 0;
}

// Weapon infos: one blob record per weapons.meta
// ({data, u16 count, u16 capacity}; the loader increments count with no capacity check) and the
// by-hash info array the pump's PostLoad rebuilds and sorts on info+0x10.
static const uintptr_t kWeaponInfoBlobs = 0x5a8a000;
static const uintptr_t kWeaponInfoByHash = 0x5a89ff0;  // {CItemInfo**, u16 count, u16 capacity}

static int pack_weapon_known(uint32_t hash, uintptr_t* found = nullptr) {
  const uintptr_t infos = *(const volatile uintptr_t*)kWeaponInfoByHash;
  const uint32_t count = *(const volatile uint16_t*)(kWeaponInfoByHash + 8);
  if (!infos || !pack_readable(infos, (size_t)count * 8u)) return -1;
  uint32_t lo = 0, hi = count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2u;
    const uintptr_t info = ((const volatile uintptr_t*)infos)[mid];
    if (!info || !pack_readable(info + 0x10, 4)) return -1;
    const uint32_t key = *(const volatile uint32_t*)(info + 0x10);
    if (key == hash) {
      if (found) *found = info;
      return 1;
    }
    if (key < hash) lo = mid + 1u;
    else hi = mid;
  }
  return 0;
}

// Weapon component infos: each components file
// appends one 0xed8-byte blob (Infos: CWeaponComponentInfo*[470] at +0x10, s32 count +0xec0) to the
// heap-grown blob list {data, u16 count, u16 capacity}. The parse's completion functor (vtable
// slot 2) refills the global CWeaponComponentInfo*[470] from every blob with NO capacity check,
// then qsorts it on info+0x10 (name hash, unsigned); lookups binary-search it. Past 470 the refill
// would run over the count and the blob list behind the array, so the gate keeps count + n <= 470.
static const uintptr_t kComponentInfos = 0x5a8b3f0;      // CWeaponComponentInfo*[470]
static const uintptr_t kComponentInfoCount = 0x5a8c2a0;  // s32, right after the array
static const uintptr_t kComponentBlobs = 0x5a8c2a8;      // {blob**, u16 count, u16 capacity}
static const uint32_t kComponentInfoMax = 470;
static const uintptr_t kComponentFunctorVtable = 0x403cc78;  // the 79 load's functor (its pin)
static const uintptr_t kComponentRefill = 0x296b820;         // its slot 2
// mov dword [count], 0; movzx eax, word [blob count]; test; jz; push rbx; lea rsi, [array]; xor
static const uint8_t kComponentRefillBytes[36] = {
    0xc7, 0x05, 0x76, 0x0a, 0x12, 0x03, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xb7,
    0x05, 0x7f, 0x0a, 0x12, 0x03, 0x48, 0x85, 0xc0, 0x0f, 0x84, 0x0a, 0x01,
    0x00, 0x00, 0x53, 0x48, 0x8d, 0x35, 0xae, 0xfb, 0x11, 0x03, 0x31, 0xd2};

static int pack_component_anchors_ok() {
  return pack_readable(kComponentFunctorVtable, 0x18) &&
         ((const volatile uintptr_t*)kComponentFunctorVtable)[2] == kComponentRefill &&
         memcmp((const void*)kComponentRefill, kComponentRefillBytes,
                sizeof(kComponentRefillBytes)) == 0;
}

// Component infos in the sorted array, or -1 when the count is out of range.
static int32_t pack_component_count() {
  const int32_t count = *(const volatile int32_t*)kComponentInfoCount;
  return count >= 0 && (uint32_t)count <= kComponentInfoMax ? count : -1;
}

static int pack_component_known(uint32_t hash, uintptr_t* found = nullptr) {
  const int32_t count = pack_component_count();
  if (count < 0) return -1;
  uint32_t lo = 0, hi = (uint32_t)count;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2u;
    const uintptr_t info = ((const volatile uintptr_t*)kComponentInfos)[mid];
    if (!info || !pack_readable(info + 0x10, 8)) return -1;
    const uint32_t key = *(const volatile uint32_t*)(info + 0x10);
    if (key == hash) {
      if (found) *found = info;
      return 1;
    }
    if (key < hash) lo = mid + 1u;
    else hi = mid;
  }
  return 0;
}

// Data files are parsed asynchronously: LoadDataFile queues a parse job and a streamer read of the
// file, routed by device class exactly like resources. A /data path reads through the APR queue and
// parses garbage (on hardware: nothing added), so each verified file is copied into never-freed
// memory and named through the memory device, the route register_pack already gates. Returns
// nullptr on success.
static uint8_t* g_pack_data_memory[GTAV_CUSTOM_PACK_DATA_CAP];
static const char* pack_stage_data_file(uint32_t row, const char* path, char* name,
                                        size_t name_size) {
  const GtavCustomPackData* data = &g_pack.data[row];
  const uint64_t bytes = (data->size + GTAV_PACK_ARCHIVE_SLACK + 0xffffull) & ~0xffffull;
  if (!g_pack_data_memory[row]) {
    if (const char* refusal = pack_map_direct(bytes, &g_pack_data_memory[row])) return refusal;
  }
  uint8_t* memory = g_pack_data_memory[row];
  memset(memory, 0, (size_t)bytes);
  if (pack_read_local(path, memory, data->size, data->size) != (int64_t)data->size ||
      !pack_sha256_matches(memory, data->size, data->sha256))
    return "pack data file size/hash gate refused";
  // The audio metadata mounter takes strrchr(name, '/') + 1 without a NULL check: audio rows carry a
  // directory (pack_audio_gate).
  const int length = snprintf(name, name_size, "memory:$0x%llx,%llu,0:%s%s",
                              (unsigned long long)(uintptr_t)memory,
                              (unsigned long long)data->size,
                              data->type == GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA ? "audio/" : "",
                              data->file);
  if (length <= 0 || (size_t)length >= name_size) return "pack data memory name too long";
  void* device = ((PackGetDeviceFn)kFiDeviceGetDevice)(name, true);
  if ((uintptr_t)device != kFiMemoryDevice || *(const uintptr_t*)device != kFiMemoryVtable ||
      ((PackGetFileSizeFn)(*(const uintptr_t**)device)[kFiSlotGetFileSize])(device, name) !=
          data->size)
    return "pack data memory device gate refused";
  return nullptr;
}

// joaat of the text of every <tag>...</tag> in staged data row `row` (XML), at most `max`.
// Returns the element count (which may exceed `max`).
#define GTAV_PACK_DATA_NAMES_MAX 64
static uint32_t pack_data_names(uint32_t row, const char* tag, const char* anchor, uint32_t* hashes,
                                uint32_t max) {
  const char* text = (const char*)g_pack_data_memory[row];
  const size_t size = (size_t)g_pack.data[row].size;
  const size_t anchor_length = anchor ? strlen(anchor) : 0;
  char open[48];
  const int open_length = snprintf(open, sizeof(open), "<%s>", tag);
  uint32_t count = 0;
  int armed = !anchor;  // with an anchor: count one element per anchor
  if (!text || open_length <= 0 || (size_t)open_length >= sizeof(open)) return 0;
  for (size_t i = 0; i + (size_t)open_length < size; ++i) {
    if (anchor && i + anchor_length <= size && memcmp(text + i, anchor, anchor_length) == 0) {
      armed = 1;
      i += anchor_length - 1u;
      continue;
    }
    if (!armed || memcmp(text + i, open, (size_t)open_length) != 0) continue;
    if (anchor) armed = 0;
    size_t at = i + (size_t)open_length;
    char value[GTAV_CUSTOM_PACK_NAME_MAX];
    size_t n = 0;
    while (at < size && text[at] != '<' && n + 1u < sizeof(value)) value[n++] = text[at++];
    value[n] = '\0';
    if (count < max) hashes[count] = pack_joaat(value);
    ++count;
    i = at;
  }
  return count;
}

// Upper bound on the entries the engine's parser can append from the `list` array (InitDatas,
// Infos, Kits) of staged row `row`, or UINT32_MAX when the file is refused. The name scanner
// (pack_data_names) matches one spelling of one element only; this counts every element child of
// the array structurally (gtav_custom_pack_xml_list_bound: any child name, self-closing children,
// single quotes, whitespace in tags; markup inside comments/CDATA counts too). A capacity check on
// an engine array without its own bound must hold for this count, not only for the names.
static uint32_t pack_data_list_bound(uint32_t row, const char* list) {
  return gtav_custom_pack_xml_list_bound((const char*)g_pack_data_memory[row],
                                         (size_t)g_pack.data[row].size, list);
}

// The same bound one level deeper: every wheel of every carcols <Wheels> type array.
static uint32_t pack_data_grandchild_bound(uint32_t row, const char* list) {
  return gtav_custom_pack_xml_grandchild_bound((const char*)g_pack_data_memory[row],
                                               (size_t)g_pack.data[row].size, list);
}

// Timecycle modifiers (TIMECYCLEMOD_FILE 28).
// LoadDataFile parses the XML on this thread (0x31678d0 normalized). Per <modifier name="X"> it
// appends a 0x28-byte modifier to the modifier array: entries* +0 and u16 count +8 of 12-byte
// {var, f32, f32}, name hash +0x10 (atStringHash, the lowercase joaat), userFlags +0x20. It also
// appends {u32 hash, u32, T*} to the name map. Both grow by 16 with no capacity limit and no
// duplicate check. 0x31680b0 then sorts the map, sets the sorted byte and resizes the
// per-modifier weight array to the modifier count. Vars are parsed only while the tc-vars-ready
// byte is 1; otherwise every new modifier has 0 entries.
static const uintptr_t kTcModifiers = 0x577de20;    // T*[], u16 count +8, u16 capacity +0xa
static const uintptr_t kTcMapSorted = 0x577de30;    // u8, 1 once the name map is sorted
static const uintptr_t kTcModifierMap = 0x577de38;  // {u32 hash, u32, T*}[], u16 count +8
static const uintptr_t kTcWeights = 0x577dde8;      // f32[] per modifier, u16 count +8
static const uintptr_t kTcVarsReady = 0x6210c60;    // u8, 1 once the 429 tc var names exist
// SET_TIMECYCLE_MODIFIER (live 0x1ab67f0) stores the resolved modifier index here (-1: none, set
// by CLEAR_TIMECYCLE_MODIFIER 0x1ab68e0); GET_TIMECYCLE_MODIFIER_INDEX (0x1ab6910) returns it.
static const uintptr_t kTcCurrentIndex = 0x57805b0;  // s32

typedef struct {
  uintptr_t modifiers, map;
  uint32_t count, capacity, map_count, sorted, weights, vars_ready;
} PackTcState;

// pack_readable with the last readable region cached, for walks over many heap objects.
static int pack_readable_cached(uintptr_t address, size_t bytes, GtavLooseVqInfo* cache) {
  if (cache->end && address >= (uintptr_t)cache->start && address + bytes <= (uintptr_t)cache->end)
    return 1;
  if (!loose_query(address, cache) || !(cache->protection & 1)) {
    memset(cache, 0, sizeof(*cache));
    return 0;
  }
  return address + bytes <= (uintptr_t)cache->end;
}

// Reads the modifier array, name map and weight array headers. -1 when any is unreadable.
static int pack_tc_state(PackTcState* s) {
  memset(s, 0, sizeof(*s));
  if (!pack_readable(kTcModifiers, 0x28) || !pack_readable(kTcWeights, 0x10) ||
      !pack_readable(kTcVarsReady, 1))
    return -1;
  s->modifiers = *(const volatile uintptr_t*)kTcModifiers;
  s->count = *(const volatile uint16_t*)(kTcModifiers + 8);
  s->capacity = *(const volatile uint16_t*)(kTcModifiers + 0xa);
  s->sorted = *(const volatile uint8_t*)kTcMapSorted;
  s->map = *(const volatile uintptr_t*)kTcModifierMap;
  s->map_count = *(const volatile uint16_t*)(kTcModifierMap + 8);
  s->weights = *(const volatile uint16_t*)(kTcWeights + 8);
  s->vars_ready = *(const volatile uint8_t*)kTcVarsReady;
  if (s->count > s->capacity || (s->count && !pack_readable(s->modifiers, (size_t)s->count * 8u)) ||
      (s->map_count && !pack_readable(s->map, (size_t)s->map_count * 16u)))
    return -1;
  return 0;
}

// 1 when `hash` is in the name map (linear: no reliance on the sorted order).
static int pack_tc_named(const PackTcState* s, uint32_t hash) {
  for (uint32_t i = 0; i < s->map_count; ++i)
    if (*(const volatile uint32_t*)(s->map + (uintptr_t)i * 16u) == hash) return 1;
  return 0;
}

// Index of the modifier named `hash` in the modifier array (the index SET_TIMECYCLE_MODIFIER
// stores), or -1. `mods` receives its parsed var count.
static int32_t pack_tc_index(const PackTcState* s, uint32_t hash, uint32_t* mods) {
  GtavLooseVqInfo cache = {};
  for (uint32_t i = 0; i < s->count; ++i) {
    const uintptr_t m = *(const volatile uintptr_t*)(s->modifiers + (uintptr_t)i * 8u);
    if (!m || !pack_readable_cached(m, 0x28, &cache) ||
        *(const volatile uint32_t*)(m + 0x10) != hash)
      continue;
    if (mods) *mods = *(const volatile uint16_t*)(m + 8);
    return (int32_t)i;
  }
  return -1;
}

// joaat of the name of every <modifier ...> element in staged row `row`, at most `max`. The engine
// counts every <modifier> element, so one whose first attribute is not name="..." (1..63
// characters) still counts, with hash 0, which the gate refuses. Returns the element count.
// Parser differential: a spelling this scan misses cannot overrun anything (the modifier array,
// name map and weights are heap-grown); pack_tc_verify then fails the row (count != before + n).
static uint32_t pack_tc_modifier_names(uint32_t row, uint32_t* hashes, uint32_t max) {
  const char* text = (const char*)g_pack_data_memory[row];
  const size_t size = (size_t)g_pack.data[row].size;
  static const char kOpen[] = "<modifier";
  static const char kName[] = " name=\"";
  const size_t open_length = sizeof(kOpen) - 1u, name_length = sizeof(kName) - 1u;
  uint32_t count = 0;
  for (size_t i = 0; text && i + open_length < size; ++i) {
    if (memcmp(text + i, kOpen, open_length) != 0) continue;
    const char next = text[i + open_length];
    if (next != ' ' && next != '\t' && next != '\r' && next != '\n' && next != '>' && next != '/')
      continue;  // e.g. <modifiers>
    size_t at = i + open_length;
    uint32_t hash = 0;
    if (at + name_length <= size && memcmp(text + at, kName, name_length) == 0) {
      at += name_length;
      char value[GTAV_CUSTOM_PACK_NAME_MAX];
      size_t n = 0;
      while (at < size && text[at] != '"' && text[at] != '<' && n + 1u < sizeof(value))
        value[n++] = text[at++];
      value[n] = '\0';
      if (n && at < size && text[at] == '"') hash = pack_joaat(value);
    }
    if (count < max) hashes[count] = hash;
    ++count;
    i = at;
  }
  return count;
}

// TIMECYCLEMOD_FILE gate: tc vars initialised, 1..GTAV_PACK_DATA_NAMES_MAX well-formed modifier
// names, none repeated in the file and none already in the live name map. A duplicate name would
// make lookups ambiguous because the map sort is unstable. Returns nullptr when the row may load.
static const char* pack_tc_gate(uint32_t n, const uint32_t* hashes) {
  PackTcState s;
  const int readable = pack_tc_state(&s) == 0;
  char line[160];  // Keep the gate's final fields in klog/pack-notes, beyond the status slot.
  snprintf(line, sizeof(line),
           "GTAVMenu pack data tc names=%u modifiers=%u/%u map=%u sorted=%u weights=%u "
           "vars_ready=%u readable=%d",
           (unsigned)n, (unsigned)s.count, (unsigned)s.capacity, (unsigned)s.map_count,
           (unsigned)s.sorted, (unsigned)s.weights, (unsigned)s.vars_ready, readable);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  if (!readable) return "timecycle modifier arrays unreadable";
  if (s.vars_ready != 1u) return "timecycle vars not initialised yet (load in Story mode)";
  if (!n || n > GTAV_PACK_DATA_NAMES_MAX) return "timecycle file names no modifier, or too many";
  if (s.map_count != s.count || s.sorted != 1u) return "timecycle name map out of step";
  for (uint32_t i = 0; i < n; ++i) {
    if (!hashes[i]) return "every <modifier needs name=\"...\" first (1-63 chars)";
    for (uint32_t j = 0; j < i; ++j)
      if (hashes[j] == hashes[i]) return "modifier name repeated in the file";
    if (pack_tc_named(&s, hashes[i])) return "modifier name already exists (retail or a pack)";
  }
  return nullptr;
}

// After the sync parse: the modifier count grew by exactly `n`, the map and weights followed and
// are sorted, and every new name resolves to a modifier with parsed vars. One klog line per row
// and per new modifier (index = what SET_TIMECYCLE_MODIFIER will store for it).
static int pack_tc_verify(uint32_t row, uint32_t n, const uint32_t* hashes, uint32_t before) {
  PackTcState s;
  const int readable = pack_tc_state(&s) == 0;
  uint32_t known = 0, usable = 0;
  char line[160];  // Includes known/usable even with five-digit modifier counts.
  for (uint32_t i = 0; readable && i < n && i < GTAV_PACK_DATA_NAMES_MAX; ++i) {
    known += pack_tc_named(&s, hashes[i]);
    uint32_t mods = 0;
    const int32_t index = pack_tc_index(&s, hashes[i], &mods);
    usable += index >= 0 && mods > 0u;
    if (i >= 8u) continue;
    snprintf(line, sizeof(line), "GTAVMenu pack data %u tc modifier 0x%08x index=%d mods=%u",
             (unsigned)row, (unsigned)hashes[i], (int)index, (unsigned)mods);
    gtav_worker_klog(line);
  }
  snprintf(line, sizeof(line),
           "GTAVMenu pack data %u sync type=28 modifiers %u->%u/%u map=%u sorted=%u weights=%u "
           "known=%u/%u usable=%u",
           (unsigned)row, (unsigned)before, (unsigned)s.count, (unsigned)s.capacity,
           (unsigned)s.map_count, (unsigned)s.sorted, (unsigned)s.weights, (unsigned)known,
           (unsigned)n, (unsigned)usable);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  return readable && s.count == before + n && s.map_count == s.count && s.sorted == 1u &&
         s.weights == s.count && known == n && usable == n;
}

// CARCOLS growth gate. The merge appends kits in place while count + new <= capacity; otherwise it
// deep-copies every kit into a larger array and frees each old kit's visible-mod (0x68) and
// link-mod (0x20) arrays, while vehicle mod render data (pool 0x45a57d0) holds raw pointers into
// them that Subrender threads keep drawing from (a Subrender crash on hardware). Refuse a growing
// load while any live render data points into those arrays (any tuned car streamed in, parked ones
// too): the one-press load waits on that refusal.
// The kit array is heap-grown (no overrun), but the in-place/grow decision must not trust the
// kitName scanner: a kit it missed would take the grow path unchecked, so `new_kits` is the larger
// of the names and the structural count of <Kits> children.
// Wheels: 13 per-type arrays at vg+0x68+t*0x10 ({0x38-byte items, u16 count, u16 capacity}, key =
// wheelName hash at +0x20, first wins). A full array is copied into a block 16 larger and the old
// block freed (0x1023e90), while render data caches item pointers at gfx +0xb48 (front, 0xff4f10)
// and +0xb50 (rear, 0xff4f66). A type grows only when count + new > capacity, `new` bounded by
// every grandchild of <Wheels>; each type that could grow must have no live gfx pointer into it.
// Unpinned code refuses wheels as before.
static const uintptr_t kModRenderDataPool = 0x45a57d0;
#define PACK_WHEEL_TYPES 13u
static const uintptr_t kWheelMergeCode = 0x1423d7a;
static const uint8_t kWheelMergeCodeBytes[] = {
    0x48, 0x83, 0xf8, 0x0d, 0x0f, 0x84, 0x58, 0x0c, 0x00, 0x00, 0x49, 0x89, 0xc7, 0x48,
    0x89, 0x85, 0x40, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x85, 0x70, 0xff, 0xff, 0xff, 0x49,
    0xc1, 0xe7, 0x04, 0x42, 0x0f, 0xb7, 0x44, 0x38, 0x70, 0x66, 0x85, 0xc0, 0x74, 0xcc};
static const uintptr_t kWheelGrowCode = 0x1423e90;
static const uint8_t kWheelGrowCodeBytes[] = {0x66, 0x44, 0x3b, 0x73, 0x0a, 0x0f,
                                              0x85, 0x96, 0x05, 0x00, 0x00};
static const uintptr_t kWheelGfxFrontCode = 0x13f4f03;
static const uint8_t kWheelGfxFrontCodeBytes[] = {0x4c, 0x6b, 0xf2, 0x38, 0x48, 0xc1, 0xe7,
                                                  0x04, 0x4c, 0x03, 0x74, 0x3e, 0x68, 0x4d,
                                                  0x89, 0xb2, 0x48, 0x0b, 0x00, 0x00};
static const uintptr_t kWheelGfxRearCode = 0x13f4f59;
static const uint8_t kWheelGfxRearCodeBytes[] = {0x48, 0xc1, 0xe0, 0x04, 0x48, 0x6b, 0xc9,
                                                 0x38, 0x48, 0x03, 0x4c, 0x02, 0x68, 0x49,
                                                 0x89, 0x8a, 0x50, 0x0b, 0x00, 0x00};

struct PackWheelArrays {
  uintptr_t data[PACK_WHEEL_TYPES];
  uint16_t count[PACK_WHEEL_TYPES], capacity[PACK_WHEEL_TYPES];
};

static int pack_wheel_arrays(PackWheelArrays* out) {
  memset(out, 0, sizeof(*out));
  const uintptr_t vg = pack_readable(kVehicleVariationGlobals, 8)
                           ? *(const volatile uintptr_t*)kVehicleVariationGlobals
                           : 0;
  if (!vg || !pack_readable(vg + 0x68, PACK_WHEEL_TYPES * 0x10u)) return -1;
  for (uint32_t t = 0; t < PACK_WHEEL_TYPES; ++t) {
    out->data[t] = *(const volatile uintptr_t*)(vg + 0x68 + t * 0x10u);
    out->count[t] = *(const volatile uint16_t*)(vg + 0x70 + t * 0x10u);
    out->capacity[t] = *(const volatile uint16_t*)(vg + 0x72 + t * 0x10u);
  }
  return 0;
}

static int pack_wheel_code_ok() {
  return memcmp((const void*)kWheelMergeCode, kWheelMergeCodeBytes, sizeof(kWheelMergeCodeBytes)) ==
             0 &&
         memcmp((const void*)kWheelGrowCode, kWheelGrowCodeBytes, sizeof(kWheelGrowCodeBytes)) ==
             0 &&
         memcmp((const void*)kWheelGfxFrontCode, kWheelGfxFrontCodeBytes,
                sizeof(kWheelGfxFrontCodeBytes)) == 0 &&
         memcmp((const void*)kWheelGfxRearCode, kWheelGfxRearCodeBytes,
                sizeof(kWheelGfxRearCodeBytes)) == 0;
}

// The mod render data pool (kModRenderDataPool): {slot base, flags (live while >= 0), slots,
// element size}. -1 when unreadable.
struct PackModRenderPool {
  uintptr_t base, flags;
  int32_t slots, element;
};
static int pack_mod_render_pool(PackModRenderPool* p) {
  const uintptr_t pool = pack_readable(kModRenderDataPool, 8)
                             ? *(const volatile uintptr_t*)kModRenderDataPool
                             : 0;
  if (!pool || !pack_readable(pool, 0x2c)) return -1;
  p->base = *(const volatile uintptr_t*)(pool + 8);
  p->flags = *(const volatile uintptr_t*)(pool + 0x10);
  p->slots = *(const volatile int32_t*)(pool + 0x18);
  p->element = *(const volatile int32_t*)(pool + 0x1c);
  return p->slots < 0 || p->slots > 4096 || p->element <= 0xbaf ||
                 (p->slots && (!pack_readable(p->flags, (size_t)p->slots) ||
                               !pack_readable(p->base, (size_t)p->slots * (size_t)p->element)))
             ? -1
             : 0;
}

// Mod `mod` (0..52) of render data `gfx`: 0..0x32 kit visible/link mod pointers, 0x33/0x34 the
// front/rear wheel item pointers.
static uintptr_t pack_mod_render_ptr(uintptr_t gfx, int mod) {
  const uintptr_t at = mod < 0x24   ? gfx + 0x9b0u + (uintptr_t)mod * 8u
                       : mod < 0x33 ? gfx + 0xad0u + (uintptr_t)(mod - 0x24) * 8u
                                    : gfx + 0xb48u + (uintptr_t)(mod - 0x33) * 8u;
  return *(const volatile uintptr_t*)at;
}

// Live render data slots holding any mod or wheel pointer (tuned cars streamed in, parked ones
// too); 0 when the pool is unreadable. The Load press warns with it (worker, read-only).
static uint32_t pack_tuned_cars() {
  PackModRenderPool p;
  if (pack_mod_render_pool(&p) != 0) return 0;
  uint32_t tuned = 0;
  for (int32_t i = 0; i < p.slots; ++i) {
    if (*(const volatile int8_t*)(p.flags + (uintptr_t)i) < 0) continue;
    const uintptr_t gfx = p.base + (uintptr_t)i * (uintptr_t)p.element;
    int mod = 0;
    while (mod < 53 && !pack_mod_render_ptr(gfx, mod)) ++mod;
    tuned += mod < 53;
  }
  return tuned;
}

// The gate's in-use refusal: the autoload waits on it (custom_pack_autoload.inc pack_al_gate_wait)
// instead of failing; nothing of the row has run. The last gate's hit and live slot counts.
static const char kPackCarcolsInUse[] = "carcols would free mod parts in use (tuned car nearby)";
static volatile uint32_t g_pack_carcols_hits, g_pack_carcols_gfx;

static const char* pack_carcols_gate(uint32_t row, uint32_t new_kits) {
  PackVarArrays vars;
  if (pack_var_arrays(&vars) != 0) return "vehicle variation globals unreadable";
  const uint32_t kits = pack_data_list_bound(row, "Kits");
  if (kits == UINT32_MAX) return "carcols file is not plain XML (structure refused)";
  const uint32_t new_wheels = pack_data_grandchild_bound(row, "Wheels");
  if (new_wheels == UINT32_MAX) return "carcols file is not plain XML (structure refused)";
  PackWheelArrays wheels;
  uint32_t wheel_grow = 0;  // bit t: wheel type t may be re-allocated by this row
  if (new_wheels) {
    if (!pack_wheel_code_ok() || pack_wheel_arrays(&wheels) != 0)
      return "carcols wheels are not supported";
    for (uint32_t t = 0; t < PACK_WHEEL_TYPES; ++t) {
      // An empty block (capacity 0) frees nothing when it grows.
      if (!wheels.capacity[t] || (uint32_t)wheels.count[t] + new_wheels <= wheels.capacity[t])
        continue;
      if (!wheels.data[t] || !pack_readable(wheels.data[t], (size_t)wheels.count[t] * 0x38u))
        return "carcols wheel array unreadable";
      wheel_grow |= 1u << t;
    }
  }
  if (kits > new_kits) new_kits = kits;
  const int kit_grow = (uint32_t)vars.kit_count + new_kits > vars.kit_capacity;
  if (!kit_grow && !wheel_grow) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                       "pack carcols gate kits=%u/%u new=%u wheels=%u grow=0",
                       (unsigned)vars.kit_count, (unsigned)vars.kit_capacity, (unsigned)new_kits,
                       (unsigned)new_wheels);
    return nullptr;
  }
  if (kit_grow && (!vars.kits || !pack_readable(vars.kits, (size_t)vars.kit_count * 0x70u)))
    return "carcols kit array unreadable";
  PackModRenderPool p;
  if (pack_mod_render_pool(&p) != 0) return "mod render data pool unreadable";
  uint32_t live = 0, pointers = 0, hits = 0;
  int32_t first_slot = -1, first_mod = -1;
  for (int32_t i = 0; i < p.slots; ++i) {
    if (*(const volatile int8_t*)(p.flags + (uintptr_t)i) < 0) continue;
    ++live;
    const uintptr_t gfx = p.base + (uintptr_t)i * (uintptr_t)p.element;
    for (int mod = 0; mod < 53; ++mod) {
      const uintptr_t pointer = pack_mod_render_ptr(gfx, mod);
      if (!pointer) continue;
      ++pointers;
      int hit = 0;
      for (uint32_t t = 0; mod >= 0x33 && t < PACK_WHEEL_TYPES && !hit; ++t)
        hit = (wheel_grow >> t & 1u) && pointer >= wheels.data[t] &&
              pointer < wheels.data[t] + (uintptr_t)wheels.capacity[t] * 0x38u;
      for (uint32_t k = 0; kit_grow && mod < 0x33 && k < vars.kit_count && !hit; ++k) {
        const uintptr_t kit = vars.kits + (uintptr_t)k * 0x70u;
        const uintptr_t vis = *(const volatile uintptr_t*)(kit + 0x10);
        const uintptr_t vis_end = vis + (uintptr_t)*(const volatile uint16_t*)(kit + 0x1a) * 0x68u;
        const uintptr_t link = *(const volatile uintptr_t*)(kit + 0x20);
        const uintptr_t link_end = link + (uintptr_t)*(const volatile uint16_t*)(kit + 0x2a) * 0x20u;
        hit = (pointer >= vis && pointer < vis_end) || (pointer >= link && pointer < link_end);
      }
      if (hit && !hits++) {
        first_slot = i;
        first_mod = mod;
      }
    }
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack carcols gate kits=%u/%u new=%u wheels=%u grow=%d/0x%x gfx=%u ptrs=%u "
                     "hits=%u first=%d/%d",
                     (unsigned)vars.kit_count, (unsigned)vars.kit_capacity, (unsigned)new_kits,
                     (unsigned)new_wheels, kit_grow, (unsigned)wheel_grow, (unsigned)live,
                     (unsigned)pointers, (unsigned)hits, (int)first_slot, (int)first_mod);
  g_pack_carcols_gfx = live;
  g_pack_carcols_hits = hits;
  return hits ? kPackCarcolsInUse : nullptr;
}

// SHOP_PED_APPAREL_META_FILE. The 137 case of the
// mounter (0x16449e0 normalized) returns true WITHOUT queueing when the DataFile name's hash is in
// the apparel manager's file list; otherwise it queues one parse. Its post-parse step (functor slot
// 2, 0x164b600) resolves pedName through the archetype map, returns early while that model is
// loaded, loading or requested (0x15b4280), and appends the MetaDataStore slot of fullDlcName to the
// model's variation-info slots (model +0xe0). It only looks names up, so the archive holding
// <fullDlcName>.pmt must be registered first. A streamed ped finds a DLC drawable at
// "<model name>_<dlcName>/<comp>_<nnn>_<u|r>" (0xead2f0), so fullDlcName must be pedName_dlcName.
// No count drives a capacity here: the slot append compares count with capacity (0x1a4c67a pin)
// and the post-parse repeats the streamed check itself, so a pedName spelling the scan misses
// refuses (count != 1) or fails pack_apparel_verify; nothing overruns.
struct PackApparelPin {
  uintptr_t at;
  uint8_t size;
  uint8_t bytes[56];
  const char* what;
};
// Live eboot text (execute-only) range, from the crash dumps' dynamic-library list.
static const uintptr_t kEbootTextStart = 0x400000;
static const uintptr_t kEbootTextEnd = 0x3798000;
static const PackApparelPin kApparelPins[] = {
    // 137 case: manager [0x54566c8] and functor vtable 0x3fa8598 (rip-relative), then the file
    // list walk {T** +0x70, u16 +0x78}, name hash at file +0x20.
    {0x1a449e0, 32, {0x48, 0x8b, 0x1d, 0xe1, 0x1c, 0xa1, 0x03, 0x48, 0x8d, 0x05, 0xaa, 0x3b, 0x56, 0x02, 0x4c, 0x89, 0xe7, 0x66, 0xc7, 0x83, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0xc7, 0x83, 0xb0, 0x00, 0x00}, "137 case"},
    {0x1a44a3c, 40, {0x0f, 0xb7, 0x4b, 0x78, 0x48, 0x85, 0xc9, 0x0f, 0x84, 0x2d, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x36, 0x31, 0xd2, 0x66, 0x90, 0x48, 0x8b, 0x3c, 0xd6, 0x39, 0x47, 0x20, 0x0f, 0x84, 0x11, 0x01, 0x00, 0x00, 0x48, 0xff, 0xc2, 0x48, 0x39, 0xd1, 0x75}, "file list"},
    // Post-parse: entry, model-info pool lookup (pool 0x6177250), streamed check, pmt slot lookup
    // (MetaDataStore +0x80/+0x88) and the append to model +0xe0.
    {0x1a4b600, 32, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0x1e, 0x48, 0x85, 0xdb, 0x0f, 0x84, 0xc9, 0x15, 0x00, 0x00, 0x4c, 0x8b, 0x6f}, "post-parse"},
    {0x1a4b7f2, 48, {0x0f, 0xb7, 0x40, 0x04, 0x3b, 0x05, 0x5c, 0xba, 0x72, 0x04, 0x0f, 0x8d, 0xe4, 0x13, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0x77, 0xba, 0x72, 0x04, 0x89, 0xc2, 0xc1, 0xea, 0x05, 0x8b, 0x0c, 0x91, 0x0f, 0xa3, 0xc1, 0x0f, 0x83, 0xcc, 0x13, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0x2f, 0xba, 0x72, 0x04, 0x48}, "model pool"},
    {0x1a4b845, 24, {0x3d, 0xff, 0xff, 0x00, 0x00, 0x0f, 0x84, 0x96, 0x13, 0x00, 0x00, 0x8d, 0xb8, 0x00, 0x00, 0xff, 0x0f, 0xe8, 0x25, 0x8a, 0xf6, 0xff, 0x84, 0xc0}, "streamed skip"},
    {0x1a4c600, 36, {0x48, 0x8b, 0x0d, 0xa9, 0x6c, 0x72, 0x04, 0x8b, 0x35, 0xab, 0x6c, 0x72, 0x04, 0x31, 0xd2, 0x44, 0x89, 0xc0, 0xf7, 0xf6, 0x8b, 0x14, 0x91, 0x8b, 0x7d, 0x94, 0x83, 0xfa, 0xff, 0x0f, 0x84, 0x6f, 0x03, 0x00, 0x00, 0x48}, "pmt lookup"},
    {0x1a4c67a, 21, {0x0f, 0xb7, 0x99, 0xe8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xb1, 0xe0, 0x00, 0x00, 0x00, 0x66, 0x3b, 0x99, 0xea, 0x00, 0x00, 0x00}, "slot append"},
    // Streamed check itself, mirrored by pack_model_streamed (module id 0x6210f30, modules
    // 0x62271c8, entries 0x6227018: state = entry +4 & 3).
    {0x19b4280, 52, {0x8b, 0x05, 0xaa, 0xcc, 0x85, 0x04, 0x48, 0x8b, 0x0d, 0x3b, 0x2f, 0x87, 0x04, 0xba, 0xff, 0xff, 0xff, 0xff, 0x48, 0x8b, 0x04, 0xc1, 0x0f, 0xb7, 0xcf, 0x81, 0xf9, 0xff, 0xff, 0x00, 0x00, 0x0f, 0x45, 0xd1, 0xb9, 0x0e, 0x00, 0x00, 0x00, 0x03, 0x50, 0x08, 0x48, 0x8b, 0x05, 0x67, 0x2d, 0x87, 0x04, 0x0f, 0xb6, 0x44}, "streamed check"},
};
static const uintptr_t kApparelFunctorVtable = 0x3fa8598;
static const uintptr_t kApparelPostParse = 0x1a4b600;
static const uintptr_t kApparelManagerPtr = 0x54566c8;
static const uintptr_t kMetaDataStore = 0x6173230;  // name map: nodes +0x78, heads +0x80, count +0x88
static const uintptr_t kModelInfoPool = 0x6177250;  // {T** +0, i32 size +8, stride +0x18, bits* +0x30}
static const uintptr_t kApparelModelModuleId = 0x6210f30;
static const uintptr_t kApparelStreamEntries = 0x6227018;  // kStreamingModules: custom_rpf.inc

// Text of the first <tag> element of staged XML row `row`; returns the element count.
static uint32_t pack_xml_text(uint32_t row, const char* tag, char* out, size_t out_size) {
  const char* text = (const char*)g_pack_data_memory[row];
  const size_t size = (size_t)g_pack.data[row].size;
  char open[48];
  const int open_length = snprintf(open, sizeof(open), "<%s>", tag);
  uint32_t count = 0;
  out[0] = '\0';
  if (!text || open_length <= 0 || (size_t)open_length >= sizeof(open)) return 0;
  for (size_t i = 0; i + (size_t)open_length < size; ++i) {
    if (memcmp(text + i, open, (size_t)open_length) != 0) continue;
    size_t at = i + (size_t)open_length, n = 0;
    while (at < size && text[at] != '<') {
      if (!count && n + 1u < out_size) out[n++] = text[at];
      ++at;
    }
    if (!count) out[n] = '\0';
    ++count;
    i = at;
  }
  return count;
}

// Slot of an asset store (MetaDataStore, drawable store: the same layout) registered under
// `hash`, or -1 (the post-parse lookup at 0x164c600; the wheel request at 0xfdcc10).
static int32_t pack_store_slot(uintptr_t s, uint32_t hash) {
  if (!pack_readable(s, 0x90)) return -1;
  const uintptr_t slots = *(const volatile uintptr_t*)(s + 0x40);
  const uintptr_t flags = *(const volatile uintptr_t*)(s + 0x48);
  const uint32_t size = *(const volatile uint32_t*)(s + 0x50);
  const uintptr_t nodes = *(const volatile uintptr_t*)(s + 0x78);
  const uintptr_t heads = *(const volatile uintptr_t*)(s + 0x80);
  const uint32_t buckets = *(const volatile uint32_t*)(s + 0x88);
  if (!slots || !flags || !nodes || !buckets || !pack_readable(heads, (size_t)buckets * 4u)) return -1;
  uint32_t node = ((const volatile uint32_t*)heads)[hash % buckets];
  for (int depth = 0; node != UINT32_MAX && depth < 256; ++depth) {
    const uintptr_t at = nodes + (uintptr_t)node * 12u;
    if (!pack_readable(at, 12)) return -1;
    if (*(const volatile uint32_t*)at == hash) {
      const int32_t slot = *(const volatile int32_t*)(at + 4);
      return slot >= 0 && (uint32_t)slot < size && pack_readable(flags + (uintptr_t)slot, 1) &&
                     *(const volatile int8_t*)(flags + (uintptr_t)slot) >= 0
                 ? slot
                 : -1;
    }
    node = *(const volatile uint32_t*)(at + 8);
  }
  return -1;
}

static int32_t pack_meta_slot(uint32_t hash) { return pack_store_slot(kMetaDataStore, hash); }

// Archetype index of `hash` (map node u16 +4) and its model info, or -1 / 0.
static int32_t pack_model_index(uint32_t hash, uintptr_t* model) {
  *model = 0;
  const uintptr_t buckets = *(const volatile uintptr_t*)kArchetypeMap;
  const uint32_t bucket_count = *(const volatile uint16_t*)(kArchetypeMap + 8);
  if (!buckets || !bucket_count || !pack_readable(buckets, bucket_count * 8u)) return -1;
  uintptr_t node = ((const volatile uintptr_t*)buckets)[hash % bucket_count];
  int32_t index = -1;
  for (int depth = 0; node && depth < 256 && index < 0; ++depth) {
    if (!pack_readable(node, 0x10)) return -1;
    if (*(const volatile uint32_t*)node == hash) index = *(const volatile uint16_t*)(node + 4);
    node = *(const volatile uintptr_t*)(node + 8);
  }
  const uintptr_t p = kModelInfoPool;
  if (index < 0 || index == 0xffff || !pack_readable(p, 0x38)) return -1;
  const uintptr_t base = *(const volatile uintptr_t*)p;
  const int32_t size = *(const volatile int32_t*)(p + 8);
  const uint64_t stride = *(const volatile uint64_t*)(p + 0x18);
  const uintptr_t bits = *(const volatile uintptr_t*)(p + 0x30);
  const uintptr_t word = bits + (uintptr_t)(index >> 5) * 4u;
  if (index >= size || !base || stride < 8 || stride > 0x1000 || !pack_readable(word, 4) ||
      !((*(const volatile uint32_t*)word >> (index & 31)) & 1u) ||
      !pack_readable(base + (uintptr_t)index * stride, 8))
    return -1;
  *model = *(const volatile uintptr_t*)(base + (uintptr_t)index * stride);
  return *model && pack_readable(*model, 0xf0) ? index : -1;
}

// 1 while model `index` is loaded, loading or requested (0x15b4280), 0 when not, -1 unreadable.
static int pack_model_streamed(int32_t index) {
  if (!pack_readable(kApparelModelModuleId, 4) || !pack_readable(kStreamingModules, 8) ||
      !pack_readable(kApparelStreamEntries, 8))
    return -1;
  const uint32_t module_id = *(const volatile uint32_t*)kApparelModelModuleId;
  const uintptr_t modules = *(const volatile uintptr_t*)kStreamingModules;
  if (!modules || module_id > 255u || !pack_readable(modules + module_id * 8u, 8)) return -1;
  const uintptr_t module = ((const volatile uintptr_t*)modules)[module_id];
  if (!module || !pack_readable(module + 8, 4)) return -1;
  const uint32_t entry = (uint32_t)index + *(const volatile uint32_t*)(module + 8);
  const uintptr_t entries = *(const volatile uintptr_t*)kApparelStreamEntries;
  if (!entries || !pack_readable(entries + (uintptr_t)entry * 8u + 4u, 1)) return -1;
  return (*(const volatile uint8_t*)(entries + (uintptr_t)entry * 8u + 4u) & 3u) != 0;
}

// The model's variation-info slots {s32* +0xe0, u16 count +0xe8}; 1 when `pmt` is among them.
static int pack_model_has_pmt(uintptr_t model, int32_t pmt, uint32_t* count) {
  *count = *(const volatile uint16_t*)(model + 0xe8);
  const uintptr_t data = *(const volatile uintptr_t*)(model + 0xe0);
  if (*count && !pack_readable(data, *count * 4u)) return -1;
  for (uint32_t i = 0; i < *count; ++i)
    if (((const volatile int32_t*)data)[i] == pmt) return 1;
  return 0;
}

static uintptr_t g_pack_apparel_model;  // queued 137 row: its ped model info, slots, pmt slot
static uint32_t g_pack_apparel_slots;
static int32_t g_pack_apparel_pmt = -1;

static const char* pack_apparel_gate(uint32_t row) {
  char line[192];
  // eboot code is mapped execute-only (crash dumps: "xotext 0x400000:0x3798000"), so the
  // vm-query read bit pack_readable checks is clear on it even though loads from it work (the
  // mounter pins memcmp it directly). A hardware run refused on exactly that; bound-check instead.
  for (const PackApparelPin& pin : kApparelPins) {
    if (pin.at < kEbootTextStart || pin.at + pin.size > kEbootTextEnd ||
        memcmp((const void*)pin.at, pin.bytes, pin.size) != 0) {
      snprintf(line, sizeof(line), "GTAVMenu pack data %u apparel pin mismatch: %s at %p",
               (unsigned)row, pin.what, (void*)pin.at);
      gtav_worker_klog(line);
      return "apparel anchors mismatch";
    }
  }
  if (!pack_readable(kApparelFunctorVtable, 0x18) ||
      ((const volatile uintptr_t*)kApparelFunctorVtable)[2] != kApparelPostParse)
    return "apparel post-parse functor mismatch";
  char ped[GTAV_CUSTOM_PACK_NAME_MAX], dlc[GTAV_CUSTOM_PACK_NAME_MAX], full[GTAV_CUSTOM_PACK_NAME_MAX];
  char expected[2 * GTAV_CUSTOM_PACK_NAME_MAX];
  if (pack_xml_text(row, "pedName", ped, sizeof(ped)) != 1 ||
      pack_xml_text(row, "dlcName", dlc, sizeof(dlc)) != 1 ||
      pack_xml_text(row, "fullDlcName", full, sizeof(full)) != 1 || !ped[0] || !dlc[0])
    return "apparel file needs one pedName, dlcName and fullDlcName (XML)";
  snprintf(expected, sizeof(expected), "%s_%s", ped, dlc);
  if (pack_joaat(expected) != pack_joaat(full)) return "apparel fullDlcName must be pedName_dlcName";
  // The mounter's own skip (returns true, queues nothing): the memory: name pack_stage_data_file built.
  char name[0x80];
  snprintf(name, sizeof(name), "memory:$0x%llx,%llu,0:%s",
           (unsigned long long)(uintptr_t)g_pack_data_memory[row],
           (unsigned long long)g_pack.data[row].size, g_pack.data[row].file);
  const uint32_t name_hash = pack_joaat(name);
  const uintptr_t manager =
      pack_readable(kApparelManagerPtr, 8) ? *(const volatile uintptr_t*)kApparelManagerPtr : 0;
  if (!manager || !pack_readable(manager + 0x70, 0x10)) return "apparel manager unreadable";
  const uintptr_t files = *(const volatile uintptr_t*)(manager + 0x70);
  const uint32_t file_count = *(const volatile uint16_t*)(manager + 0x78);
  if (file_count && !pack_readable(files, file_count * 8u)) return "apparel file list unreadable";
  for (uint32_t i = 0; i < file_count; ++i) {
    const uintptr_t file = ((const volatile uintptr_t*)files)[i];
    if (!file || !pack_readable(file + 0x20, 4)) return "apparel file list unreadable";
    if (*(const volatile uint32_t*)(file + 0x20) == name_hash) return "apparel file already loaded";
  }
  const int32_t pmt = pack_meta_slot(pack_joaat(full));
  uintptr_t model = 0;
  const int32_t index = pack_model_index(pack_joaat(ped), &model);
  const int streamed = index >= 0 ? pack_model_streamed(index) : -1;
  uint32_t slots = 0;
  const int has = model ? pack_model_has_pmt(model, pmt, &slots) : -1;
  snprintf(line, sizeof(line),
           "GTAVMenu pack data %u apparel gate ped=0x%08x idx=%d streamed=%d pmt=0x%08x slot=%d "
           "slots=%u has=%d files=%u",
           (unsigned)row, (unsigned)pack_joaat(ped), (int)index, streamed,
           (unsigned)pack_joaat(full), (int)pmt, (unsigned)slots, has, (unsigned)file_count);
  gtav_worker_klog(line);
  if (pmt < 0) return "apparel pmt is not registered (register the archive first)";
  if (index < 0) return "apparel pedName is not a known model";
  if (streamed != 0) return streamed < 0 ? "apparel ped streaming state unreadable"
                                         : "apparel ped model is in use; load before spawning it";
  if (has != 0) return has < 0 ? "apparel ped variation slots unreadable"
                               : "apparel pmt is already on the ped";
  g_pack_apparel_model = model;
  g_pack_apparel_slots = slots;
  g_pack_apparel_pmt = pmt;
  return nullptr;
}

// After the pump: the post-parse step appended exactly our pmt slot to the ped's variation slots.
static int pack_apparel_verify(int32_t row, char* message, size_t message_size) {
  uint32_t slots = 0;
  const int has = g_pack_apparel_model
                      ? pack_model_has_pmt(g_pack_apparel_model, g_pack_apparel_pmt, &slots)
                      : -1;
  const int added = has == 1 && slots == g_pack_apparel_slots + 1u;
  char line[192];
  snprintf(line, sizeof(line), "GTAVMenu pack data %d apparel slots %u->%u pmt=%d has=%d added=%d",
           (int)row, (unsigned)g_pack_apparel_slots, (unsigned)slots, (int)g_pack_apparel_pmt, has,
           added);
  gtav_worker_klog(line);
  snprintf(message, message_size, "pack data %d apparel slots %u->%u added=%d", (int)row,
           (unsigned)g_pack_apparel_slots, (unsigned)slots, added);
  return added;
}

// WEAPONCOMPONENTSINFO_FILE gate: the refill anchors hold, 1..GTAV_PACK_DATA_NAMES_MAX component
// names, none repeated in the file or already in the info array (the qsort comparator never
// returns 0, so a duplicate makes lookups ambiguous), and the array has room for all of them.
static const char* pack_component_gate(uint32_t row, uint32_t n, const uint32_t* hashes) {
  const int anchors = pack_component_anchors_ok();
  const int32_t count = pack_component_count();
  const uint32_t blobs = *(const volatile uint16_t*)(kComponentBlobs + 8);
  const uint32_t blob_capacity = *(const volatile uint16_t*)(kComponentBlobs + 0xa);
  // The refill writes a fixed CWeaponComponentInfo*[470] with no capacity check: bound it by every
  // child of <Infos> too (parser differential: an Info the name scanner missed must still fit).
  const uint32_t upper = pack_data_list_bound(row, "Infos");
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  snprintf(line, sizeof(line),
           "GTAVMenu pack data %u components names=%u items=%d infos=%d/%u blobs=%u/%u anchors=%d",
           (unsigned)row, (unsigned)n, (int)upper, (int)count, (unsigned)kComponentInfoMax,
           (unsigned)blobs, (unsigned)blob_capacity, anchors);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  if (!anchors) return "component array anchors mismatch";
  if (count < 0 || blobs > blob_capacity) return "component array unreadable";
  if (!n || n > GTAV_PACK_DATA_NAMES_MAX) return "components file names no component (XML only)";
  if ((uint32_t)count + n > kComponentInfoMax) return "component info array is full";
  if (upper == UINT32_MAX) return "components file is not plain XML (structure refused)";
  if (upper < n || (uint32_t)count + upper > kComponentInfoMax)
    return "component file items exceed the info array headroom";
  for (uint32_t i = 0; i < n; ++i) {
    if (pack_component_known(hashes[i]) != 0) return "component name already exists or unreadable";
    for (uint32_t j = 0; j < i; ++j)
      if (hashes[j] == hashes[i]) return "component name repeated in the file";
  }
  return nullptr;
}

// After the components pump: the refill must have added exactly the file's names, each resolving
// in the sorted array; klog each one's Model hash (info+0x14, what a give attaches) and whether
// that archetype is registered (a pack model comes from the type-72 row listed before this one).
static int pack_component_verify(int32_t row, int32_t before, const uint32_t* hashes, uint32_t n,
                                 char* message, size_t message_size) {
  uint32_t known = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uintptr_t info = 0;
    const int found = pack_component_known(hashes[i], &info) == 1;
    known += found;
    if (i >= 8u) continue;
    const uint32_t model =
        found && pack_readable(info + 0x14, 4) ? *(const volatile uint32_t*)(info + 0x14) : 0;
    char line[GTAV_FEATURE_MESSAGE_LEN + 16];
    snprintf(line, sizeof(line),
             "GTAVMenu pack data %d component 0x%08x found=%d model=0x%08x known=%d", (int)row,
             (unsigned)hashes[i], found, (unsigned)model, model ? pack_model_known(model) : -1);
    gtav_worker_klog(line);
  }
  const int32_t after = pack_component_count();
  snprintf(message, message_size, "pack data %d components %d->%d known=%u/%u", (int)row,
           (int)before, (int)after, (unsigned)known, (unsigned)n);
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  snprintf(line, sizeof(line), "GTAVMenu %s", message);
  gtav_worker_klog(line);
  return before >= 0 && after == before + (int32_t)n && known == n;
}

// Audio game data (AUDIO_GAMEDATA 140). The
// shared audio metadata mounter (live 0x98e840) takes the chunk name from the DataFile name: the
// basename after the last '/' (strrchr, result used with NO NULL check, so the staged name holds
// "audio/"), up to the first '_', at most 31 characters. Unless that name waits in the interface's
// pending-removal list (cancelled instead of loaded), it calls LoadMetadataChunk (live 0x2e912c0)
// with iface+8, the game-object manager, which under the manager mutex skips a name already loaded,
// opens "<name>151.rel" through fiStream on this thread (the memory device ignores the suffix),
// appends a 0x78-byte chunk to {chunks*, u16 count +8, u16 capacity +0xa} (grown by 16 when full),
// copies the data and tables and resolves the hash/pack references. Chunk: u32 name hash +0, data*
// +0x10, u32 data size +0x18, u32 objects +0x1c, index* {u32 offset, u32 hash} +0x20, buckets* +0x28
// (256 x {i32 first, i32 count} by hash low byte), u8 loaded +0x71. Format 4 (manager +0x50) never
// opens a .nametable. Objects are looked up newest chunk first (live 0x2e90730), so a vehicle created
// after the load finds a CarAudioSettings (class 3) its audioNameHash names (CarAudioEntity init,
// live 0xaa8030, reads model info +0x4e4 after the script override).
static const uintptr_t kAudioGameObjects = 0x57ef0c8;  // the game-object manager (iface+8)
static const uint32_t kAudioChunkStride = 0x78;
static const uint32_t kAudioRemovalSlots = 48;  // iface +0x10: char[0x20] names
static const uint32_t kAudioModelHash = 0x4e4;  // CVehicleModelInfo audioNameHash
static const PackApparelPin kAudioPins[] = {
    // strrchr(name, '/') + 1 with no NULL check, then strchr(.., '_'): the chunk name.
    {0x98e865, 41, {0x48, 0x89, 0x75, 0xa8, 0xbe, 0x2f, 0x00, 0x00, 0x00, 0xe8, 0x0d, 0x1d, 0xe0, 0x02, 0x49, 0x89, 0xc7, 0xbe, 0x5f, 0x00, 0x00, 0x00, 0x49, 0xff, 0xc7, 0x4c, 0x89, 0xff, 0xe8, 0x8a, 0x1d, 0xe0, 0x02, 0x4c, 0x29, 0xf8, 0xb9, 0x20, 0x00, 0x00, 0x00}, "chunk name"},
    // LoadMetadataChunk(iface+8, chunk, name, 0, 0), then the GAMEDATA (0x8c) step on 0x57ef0c8.
    {0x98ea6f, 52, {0x49, 0x8b, 0x7e, 0x08, 0x48, 0x8b, 0x55, 0xa8, 0x48, 0x8d, 0x75, 0xb0, 0x31, 0xc9, 0x45, 0x31, 0xc0, 0xe8, 0x3b, 0x28, 0x50, 0x02, 0x41, 0x89, 0xc7, 0x48, 0x8b, 0x5d, 0xa8, 0x81, 0xbb, 0x90, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x0f, 0x85, 0xd5, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x25, 0x06, 0xe6, 0x04}, "load call"},
    // The loaded-name skip: u16 count +8, chunks +0, stride 0x78, hash +0, loaded byte +0x71.
    {0x2e9138c, 56, {0x41, 0x0f, 0xb7, 0x44, 0x24, 0x08, 0x89, 0xcb, 0xc1, 0xe3, 0x0f, 0x01, 0xcb, 0x48, 0x85, 0xc0, 0x74, 0x30, 0x49, 0x8b, 0x0c, 0x24, 0xbe, 0xff, 0xff, 0xff, 0xff, 0x31, 0xd2, 0xeb, 0x11, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x48, 0xff, 0xc2, 0x48, 0x83, 0xc1, 0x78, 0x48, 0x39, 0xd0, 0x74, 0x17, 0x39, 0x19, 0x75, 0xf0, 0x80, 0x79, 0x71, 0x00}, "dedupe"},
    // GetObject: objects +0x1c, loaded +0x71, buckets +0x28 by hash low byte, index +0x20.
    {0x2e907b0, 48, {0x48, 0x6b, 0xd0, 0x78, 0x83, 0x7c, 0x11, 0x1c, 0x00, 0x74, 0xe5, 0x80, 0x7c, 0x11, 0x71, 0x00, 0x74, 0xde, 0x48, 0x8b, 0x7c, 0x11, 0x28, 0x44, 0x8b, 0x4c, 0xf7, 0x04, 0x45, 0x85, 0xc9, 0x7e, 0xcf, 0x4c, 0x63, 0x04, 0xf7, 0x48, 0x8b, 0x54, 0x11, 0x20, 0x41, 0xff, 0xc9, 0x45, 0x31, 0xd2}, "lookup buckets"},
    // ... binary search on the index entry's hash (+4), unsigned.
    {0x2e907fb, 26, {0x47, 0x8d, 0x1c, 0x11, 0x41, 0xd1, 0xfb, 0x49, 0x63, 0xfb, 0x4c, 0x01, 0xc7, 0x44, 0x39, 0x64, 0xfa, 0x04, 0x72, 0xe1, 0x0f, 0x86, 0x7a, 0xff, 0xff, 0xff}, "lookup search"},
};

typedef struct {
  uintptr_t chunks;
  uint32_t count, capacity, schema, format;
} PackAudioMgr;

static int pack_audio_mgr(PackAudioMgr* m) {
  memset(m, 0, sizeof(*m));
  if (!pack_readable(kAudioGameObjects, 0x58)) return -1;
  m->chunks = *(const volatile uintptr_t*)kAudioGameObjects;
  m->count = *(const volatile uint16_t*)(kAudioGameObjects + 8);
  m->capacity = *(const volatile uint16_t*)(kAudioGameObjects + 0xa);
  m->schema = *(const volatile uint32_t*)(kAudioGameObjects + 0x4c);
  m->format = *(const volatile uint32_t*)(kAudioGameObjects + 0x50);
  return m->count <= m->capacity &&
                 (!m->count || pack_readable(m->chunks, (size_t)m->count * kAudioChunkStride))
             ? 0
             : -1;
}

// Chunk index of name hash `hash` (loaded or not), or -1.
static int32_t pack_audio_chunk(const PackAudioMgr* m, uint32_t hash) {
  for (uint32_t c = 0; c < m->count; ++c)
    if (*(const volatile uint32_t*)(m->chunks + (uintptr_t)c * kAudioChunkStride) == hash)
      return (int32_t)c;
  return -1;
}

// The engine's GetObject, read-only: newest loaded chunk first, the hash's bucket, a binary search
// of the index. Returns the chunk index and the object's address and class byte, or -1.
static int32_t pack_audio_find(const PackAudioMgr* m, uint32_t hash, uintptr_t* object,
                               uint32_t* object_class) {
  for (int32_t c = (int32_t)m->count - 1; c >= 0; --c) {
    const uintptr_t chunk = m->chunks + (uintptr_t)c * kAudioChunkStride;
    const uint32_t objects = *(const volatile uint32_t*)(chunk + 0x1c);
    const uintptr_t data = *(const volatile uintptr_t*)(chunk + 0x10);
    const uint32_t size = *(const volatile uint32_t*)(chunk + 0x18);
    const uintptr_t index = *(const volatile uintptr_t*)(chunk + 0x20);
    const uintptr_t buckets = *(const volatile uintptr_t*)(chunk + 0x28);
    if (!objects || !*(const volatile uint8_t*)(chunk + 0x71) || !buckets ||
        !pack_readable(buckets + (uintptr_t)(hash & 0xffu) * 8u, 8))
      continue;
    const int32_t first = *(const volatile int32_t*)(buckets + (uintptr_t)(hash & 0xffu) * 8u);
    const int32_t n = *(const volatile int32_t*)(buckets + (uintptr_t)(hash & 0xffu) * 8u + 4u);
    if (n <= 0 || first < 0 || (uint32_t)first + (uint32_t)n > objects ||
        !pack_readable(index + (uintptr_t)first * 8u, (size_t)n * 8u))
      continue;
    int32_t lo = 0, hi = n - 1;
    while (lo <= hi) {
      const int32_t mid = (lo + hi) / 2;
      const uintptr_t entry = index + (uintptr_t)(first + mid) * 8u;
      const uint32_t key = *(const volatile uint32_t*)(entry + 4);
      if (key < hash) {
        lo = mid + 1;
      } else if (key > hash) {
        hi = mid - 1;
      } else {
        const uint32_t offset = *(const volatile uint32_t*)entry;
        if (offset > size || size - offset < 4 || !pack_readable(data + offset, 4)) return -1;
        *object = data + offset;
        *object_class = *(const volatile uint8_t*)(data + offset);
        return c;
      }
    }
  }
  return -1;
}

// Chunk name of an AUDIO_GAMEDATA row (<chunk>_game.rel, checked by the descriptor parser).
static void pack_audio_chunk_name(uint32_t row, char* out, size_t out_size) {
  const char* file = g_pack.data[row].file;
  size_t n = 0;
  while (file[n] && file[n] != '_' && n + 1u < out_size) {
    out[n] = file[n];
    ++n;
  }
  out[n] = '\0';
}

static uint32_t pack_audio_u32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static GtavAudioRelInfo g_pack_audio_info;  // the row being loaded (one data row at a time)

// AUDIO_GAMEDATA gate: pinned code, the manager's schema 151 / format 4, a chunk the loader can take
// (gtav_custom_pack_audio_rel_check), a chunk name no chunk has and the mounter does not wait to
// remove. Objects a loaded chunk already defines are allowed (the new chunk wins: an override) and
// counted as shadow=.
static const char* pack_audio_gate(uint32_t row) {
  char line[192], chunk[GTAV_CUSTOM_PACK_AUDIO_CHUNK_MAX + 1];
  for (const PackApparelPin& pin : kAudioPins) {
    if (pin.at < kEbootTextStart || pin.at + pin.size > kEbootTextEnd ||
        memcmp((const void*)pin.at, pin.bytes, pin.size) != 0) {
      snprintf(line, sizeof(line), "GTAVMenu pack data %u audio pin mismatch: %s at %p",
               (unsigned)row, pin.what, (void*)pin.at);
      gtav_worker_klog(line);
      return "audio anchors mismatch";
    }
  }
  const uintptr_t iface = ((const volatile uintptr_t*)kDataFileInterfaces)[g_pack.data[row].type];
  if (!pack_readable(iface, 0x6d4) || *(const volatile uintptr_t*)(iface + 8) != kAudioGameObjects)
    return "audio anchors mismatch (manager)";
  PackAudioMgr m;
  if (pack_audio_mgr(&m) != 0) return "audio metadata manager unreadable";
  if (m.schema != 151u || m.format != 4u) return "audio metadata format unexpected";
  const uint8_t* rel = g_pack_data_memory[row];
  if (const char* refusal =
          gtav_custom_pack_audio_rel_check(rel, (size_t)g_pack.data[row].size, &g_pack_audio_info))
    return refusal;
  pack_audio_chunk_name(row, chunk, sizeof(chunk));
  const uint32_t chunk_hash = pack_joaat(chunk);
  if (pack_audio_chunk(&m, chunk_hash) >= 0) return "audio chunk name already loaded";
  for (uint32_t i = 0; i < kAudioRemovalSlots; ++i) {
    const char* name = (const char*)(iface + 0x10u + i * 0x20u);
    if (name[0] && memchr(name, 0, 0x20) && pack_joaat(name) == chunk_hash)
      return "audio chunk is being removed";
  }
  uint32_t shadow = 0, cars = 0;
  for (uint32_t i = 0; i < g_pack_audio_info.objects; ++i) {
    const uint8_t* entry = rel + g_pack_audio_info.index_offset + 12u * i;
    uintptr_t object = 0;
    uint32_t object_class = 0;
    shadow += pack_audio_find(&m, pack_audio_u32(entry), &object, &object_class) >= 0;
    cars += rel[8u + pack_audio_u32(entry + 4)] == 3u;
  }
  snprintf(line, sizeof(line),
           "GTAVMenu pack data %u audio gate chunk=%s 0x%08x chunks=%u/%u objects=%u cars=%u "
           "shadow=%u refs=%u/%u",
           (unsigned)row, chunk, (unsigned)chunk_hash, (unsigned)m.count, (unsigned)m.capacity,
           (unsigned)g_pack_audio_info.objects, (unsigned)cars, (unsigned)shadow,
           (unsigned)g_pack_audio_info.hash_refs, (unsigned)g_pack_audio_info.pack_refs);
  gtav_worker_klog(line);
  return nullptr;
}

// After the sync load: exactly one chunk was appended, under our name, loaded, holding the file's
// objects, and every object now resolves to it. Per CarAudioSettings (first 8 objects): its Engine
// (+8) and GranularEngine (+0xc) references and what they resolve to; per pack vehicle model
// already registered: the audioNameHash its model info holds and whether this chunk defines it.
static int pack_audio_verify(uint32_t row, uint32_t before) {
  char line[192], chunk[GTAV_CUSTOM_PACK_AUDIO_CHUNK_MAX + 1];
  pack_audio_chunk_name(row, chunk, sizeof(chunk));
  const uint8_t* rel = g_pack_data_memory[row];
  PackAudioMgr m;
  const int readable = pack_audio_mgr(&m) == 0;
  const uintptr_t at = m.chunks + (uintptr_t)before * kAudioChunkStride;
  const int added = readable && m.count == before + 1u &&
                    *(const volatile uint32_t*)at == pack_joaat(chunk) &&
                    *(const volatile uint8_t*)(at + 0x71) == 1u &&
                    *(const volatile uint32_t*)(at + 0x1c) == g_pack_audio_info.objects &&
                    *(const volatile uint32_t*)(at + 0x18) == g_pack_audio_info.data_size;
  uint32_t resolved = 0, cars = 0;
  for (uint32_t i = 0; readable && i < g_pack_audio_info.objects; ++i) {
    const uint8_t* entry = rel + g_pack_audio_info.index_offset + 12u * i;
    const uint32_t hash = pack_audio_u32(entry);
    uintptr_t object = 0;
    uint32_t object_class = 0;
    const int32_t found = pack_audio_find(&m, hash, &object, &object_class);
    resolved += found == (int32_t)before;
    if (object_class != 3u || found != (int32_t)before || cars >= 8u ||
        !pack_readable(object, 0x10))
      continue;
    ++cars;
    const uint32_t engine = *(const volatile uint32_t*)(object + 8);
    const uint32_t granular = *(const volatile uint32_t*)(object + 0xc);
    uintptr_t e_object = 0, g_object = 0;
    uint32_t e_class = 0, g_class = 0;
    const int32_t e_chunk = engine ? pack_audio_find(&m, engine, &e_object, &e_class) : -1;
    const int32_t g_chunk = granular ? pack_audio_find(&m, granular, &g_object, &g_class) : -1;
    snprintf(line, sizeof(line),
             "GTAVMenu pack data %u audio car 0x%08x engine 0x%08x class=%u chunk=%d granular "
             "0x%08x class=%u chunk=%d",
             (unsigned)row, (unsigned)hash, (unsigned)engine, (unsigned)e_class, (int)e_chunk,
             (unsigned)granular, (unsigned)g_class, (int)g_chunk);
    gtav_worker_klog(line);
  }
  for (int i = 0; readable && i < g_pack.spawn_count; ++i) {
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_VEHICLE) continue;
    uintptr_t model = 0;
    if (pack_model_index(pack_joaat(g_pack.spawns[i].model), &model) < 0 ||
        !pack_readable(model + kAudioModelHash, 4))
      continue;  // vehicles.meta not loaded yet: list the audio row after it to see this line
    const uint32_t audio = *(const volatile uint32_t*)(model + kAudioModelHash);
    uintptr_t object = 0;
    uint32_t object_class = 0;
    const int32_t found = pack_audio_find(&m, audio, &object, &object_class);
    snprintf(line, sizeof(line),
             "GTAVMenu pack data %u audio model %s audioNameHash=0x%08x class=%u chunk=%d ours=%d",
             (unsigned)row, g_pack.spawns[i].model, (unsigned)audio, (unsigned)object_class,
             (int)found, found == (int32_t)before);
    gtav_worker_klog(line);
  }
  snprintf(line, sizeof(line),
           "GTAVMenu pack data %u sync type=140 chunks %u->%u/%u chunk=%s added=%d objects=%u/%u "
           "cars=%u",
           (unsigned)row, (unsigned)before, (unsigned)m.count, (unsigned)m.capacity, chunk, added,
           (unsigned)resolved, (unsigned)g_pack_audio_info.objects, (unsigned)cars);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  return added && resolved == g_pack_audio_info.objects;
}

// Read-only headroom gates for data row `row` declaring `n` names (hashes, at most
// GTAV_PACK_DATA_NAMES_MAX). Returns nullptr when the row may load.
static const char* pack_data_gate(uint32_t row, uint32_t n, const uint32_t* hashes) {
  const uint32_t type = g_pack.data[row].type;
  const uint32_t archetypes_free = *(const volatile uint32_t*)kArchetypePoolFree;
  const uint32_t checked = n < GTAV_PACK_DATA_NAMES_MAX ? n : GTAV_PACK_DATA_NAMES_MAX;
  uint32_t count = 0, capacity = 0;
  switch (type) {
    case GTAV_CUSTOM_PACK_DATA_CARCOLS:
      return pack_carcols_gate(row, n);
    case GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL:
      return pack_apparel_gate(row);
    case GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD:
      return pack_tc_gate(n, hashes);
    case GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA:
      return pack_audio_gate(row);
    case GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO:
      return pack_component_gate(row, n, hashes);
    case GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA:
      if (pack_vehicle_model_store(&count, &capacity) != 0) return "vehicle model store unreadable";
      break;
    case GTAV_CUSTOM_PACK_DATA_PED_METADATA:
      if (pack_model_factory_block(kPedModelFactory, kPedModelFactoryVtable, &count, &capacity) != 0)
        return "ped model store unreadable";
      break;
    case GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA:
      if (pack_model_factory_block(kWeaponModelFactory, kWeaponModelFactoryVtable, &count,
                                   &capacity) != 0)
        return "weapon model store unreadable";
      break;
    case GTAV_CUSTOM_PACK_DATA_WEAPONINFO: {
      // One blob record per file (the fixed list checked here); the by-hash info array the PostLoad
      // rebuilds is heap-grown, so a weapon the name scanner missed cannot overrun it (it only
      // fails the post-pump known == names check).
      count = *(const volatile uint16_t*)(kWeaponInfoBlobs + 8);
      capacity = *(const volatile uint16_t*)(kWeaponInfoBlobs + 0xa);
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack data weapons names=%u blobs=%u/%u",
                         (unsigned)n, (unsigned)count, (unsigned)capacity);
      if (!n || n > GTAV_PACK_DATA_NAMES_MAX) return "weapons file names no weapon (XML only)";
      if (count >= capacity) return "weapon info file list is full";
      for (uint32_t i = 0; i < checked; ++i)
        if (pack_weapon_known(hashes[i]) != 0) return "weapon name already exists or unreadable";
      return nullptr;
    }
    default:
      return nullptr;
  }
  // Model rows: every name is new, fits the factory block and gets an archetype slot. All three
  // factories append one record per element child of <InitDatas> with no capacity check (vehicles:
  // the 0xf000 block handler; peds 0xfd4dc1; AddWeaponModel), so the headroom must hold for the
  // structural bound, not only for the names the scanner matched (parser differential: a spelling
  // it misses would otherwise run the append past the fixed block). bound >= names always: a name
  // outside the list means the file does not have the layout the scanner assumes.
  const uint32_t bound = pack_data_list_bound(row, "InitDatas");
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  snprintf(line, sizeof(line),
           "GTAVMenu pack data models type=%u names=%u entries=%d store=%u/%u archetypes_free=%u",
           (unsigned)type, (unsigned)n, (int)bound, (unsigned)count, (unsigned)capacity,
           (unsigned)archetypes_free);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  if (!n || n > GTAV_PACK_DATA_NAMES_MAX) return "model file names no model (XML only)";
  if (bound == UINT32_MAX) return "model file is not plain XML (structure refused)";
  if (bound < n) return "model file names a model outside <InitDatas>";
  if (capacity - count < bound || archetypes_free < bound) {
    if (type == GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA)
      return "vehicle model store headroom gate refused";
    return type == GTAV_CUSTOM_PACK_DATA_PED_METADATA ? "ped model store headroom gate refused"
                                                      : "weapon model store headroom gate refused";
  }
  if (type != GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA) {
    // Peds skip a known name silently, weapon models register it twice: refuse both.
    for (uint32_t i = 0; i < checked; ++i)
      if (pack_model_known(hashes[i]) != 0) return "model name already exists or map unreadable";
    if (type == GTAV_CUSTOM_PACK_DATA_PED_METADATA) {
      // pft/pmt slots are looked up by name only; the MetaDataStore must have room for any it adds.
      const uintptr_t meta_store = 0x6173230;
      const uint32_t size =
          pack_readable(meta_store, 0x64) ? *(const volatile uint32_t*)(meta_store + 0x50) : 0;
      const uint32_t used =
          size ? *(const volatile uint32_t*)(meta_store + 0x60) & 0x3fffffffu : UINT32_MAX;
      if (!size || used > size || *(const volatile uint32_t*)(meta_store + 0x58) == UINT32_MAX ||
          size - used < bound)
        return "ped metadata store headroom gate refused";
    }
  }
  return nullptr;
}

// Weapon model rows: AddWeaponModel appends a
// 0x190-byte CWeaponModelInfo to the factory block (name hash +0x18); its InitData step finds the
// drawable by the model's own name (0x3044690 normalized: kind +0x60, 2 = drawable store slot
// +0x64, 1 = fragment, 0 = none, i.e. invisible) and records the txdName slot in that drawable
// store entry (+0x18; -1 = no texture dictionary). One klog line per new model, from engine state.
static const uintptr_t kDrawableStore = 0x61a6cd8;  // entries* +0x40, flags* +0x48, size +0x50
static const uintptr_t kDrawableStoreVtable = 0x40934c0;

static void pack_weapon_model_note(uint32_t row, uint32_t from, uint32_t to) {
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  uint32_t count = 0, capacity = 0;
  uintptr_t models = 0;
  if (pack_model_factory_block(kWeaponModelFactory, kWeaponModelFactoryVtable, &count, &capacity,
                               &models) != 0 ||
      to > count || from >= to || !pack_readable(models + (uintptr_t)from * 0x190u,
                                                 (size_t)(to - from) * 0x190u)) {
    snprintf(line, sizeof(line), "GTAVMenu pack data %u weapon models %u..%u unreadable",
             (unsigned)row, (unsigned)from, (unsigned)to);
    gtav_worker_klog(line);
    return;
  }
  const int store_ok = pack_readable(kDrawableStore, 0x58) &&
                       *(const volatile uintptr_t*)kDrawableStore == kDrawableStoreVtable;
  const uintptr_t entries = store_ok ? *(const volatile uintptr_t*)(kDrawableStore + 0x40) : 0;
  const uintptr_t flags = store_ok ? *(const volatile uintptr_t*)(kDrawableStore + 0x48) : 0;
  const uint32_t slots = store_ok ? *(const volatile uint32_t*)(kDrawableStore + 0x50) : 0;
  const int32_t stride = store_ok ? *(const volatile int32_t*)(kDrawableStore + 0x54) : 0;
  for (uint32_t i = from; i < to && i < from + 8u; ++i) {
    const uintptr_t info = models + (uintptr_t)i * 0x190u;
    const uint32_t hash = *(const volatile uint32_t*)(info + 0x18);
    const uint32_t kind = *(const volatile uint8_t*)(info + 0x60);
    const int32_t slot = *(const volatile int32_t*)(info + 0x64);
    int32_t txd = -2;  // -2: not read (no drawable, or the store is unreadable)
    if (kind == 2u && slot >= 0 && (uint32_t)slot < slots && stride >= 0x1c && flags &&
        pack_readable(flags + (uintptr_t)slot, 1) &&
        *(const volatile int8_t*)(flags + (uintptr_t)slot) >= 0) {
      const uintptr_t entry = entries + (uintptr_t)slot * (uintptr_t)stride;
      if (entries && pack_readable(entry + 0x18, 4)) txd = *(const volatile int32_t*)(entry + 0x18);
    }
    snprintf(line, sizeof(line),
             "GTAVMenu pack data %u weapon model 0x%08x known=%d kind=%u slot=%d txd=%d",
             (unsigned)row, (unsigned)hash, pack_model_known(hash), (unsigned)kind, (int)slot,
             (int)txd);
    gtav_worker_klog(line);
  }
}

// Sync rows parse inside LoadDataFile: confirm the factory (or, for timecycle modifiers, the
// modifier array) grew by `n` and every name resolves; an audio chunk checks itself.
static int pack_data_sync_verify(uint32_t row, uint32_t n, const uint32_t* hashes,
                                 uint32_t count_before) {
  const uint32_t type = g_pack.data[row].type;
  if (type == GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD) return pack_tc_verify(row, n, hashes, count_before);
  if (type == GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA) return pack_audio_verify(row, count_before);
  uint32_t count = 0, capacity = 0;
  const int store =
      type == GTAV_CUSTOM_PACK_DATA_PED_METADATA
          ? pack_model_factory_block(kPedModelFactory, kPedModelFactoryVtable, &count, &capacity)
          : pack_model_factory_block(kWeaponModelFactory, kWeaponModelFactoryVtable, &count,
                                     &capacity);
  uint32_t known = 0;
  for (uint32_t i = 0; i < n && i < GTAV_PACK_DATA_NAMES_MAX; ++i)
    known += pack_model_known(hashes[i]) == 1;
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  snprintf(line, sizeof(line), "GTAVMenu pack data %u sync type=%u models %u->%u/%u known=%u/%u",
           (unsigned)row, (unsigned)type, (unsigned)count_before, (unsigned)count,
           (unsigned)capacity, (unsigned)known, (unsigned)n);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + 9);
  gtav_worker_klog(line);
  if (type == GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA && store == 0 && count > count_before)
    pack_weapon_model_note(row, count_before, count);
  return store == 0 && count == count_before + n && known == n;
}

static uint64_t g_pack_data_parsed;  // rows whose contents landed (pumped, or sync and verified)
// Names and weapon count of the queued row, for the post-pump check.
static uint32_t g_pack_pending_names[GTAV_PACK_DATA_NAMES_MAX];
static uint32_t g_pack_pending_name_count;

// Weapon wheel icon aliases (`wicon` rows). The
// wheel shows a weapon's icon with wT<WheelSlot>.gotoAndStop("INT" + name hash) on an instance of
// the hud.gfx sprite SLOT_WEAPONS_<WheelSlot> (MASTER_WEAPONS in a vehicle); a pack weapon's name
// has no such frame label. After its WEAPONINFO row lands, each row adds the label
// INT<signed weapon hash> to the loaded movie's frame-label tables of both sprites, pointing at the
// retail donor's frame (same WheelSlot). The movie file is not patched; the game keeps sending the
// pack weapon's own hash, so selection is unchanged. Path (GFx 3.x in the eboot, all pinned):
// ScaleformMgr movie array [id] (+0xb0 store slot, +0x1b8 state 3 = active) -> ScaleformStore
// object +0x18 -> +0x10 GFxMovieRoot -> +0x58 GFxMovieDefImpl -> vt+0xe0 GetResource(export) ->
// GFxSpriteDef; its vt+0x70 GetLabeledFrame looks the label up in NamedFrames (+0x28, a GFx string
// hash), and NamedFrames.add (the FrameLabel loader's own insert) adds the alias under the movie's
// lock. The key is handed over as a string with a foreign heap, so add copies it into the movie
// heap; nothing the table keeps points at worker memory. Main thread only (pump_pack_data).
static const uintptr_t kSfMovies = 0x458dc00;           // movie structs (stride 0x1e0)
static const uintptr_t kSfMovieCount = 0x458dc08;       // u16
static const uintptr_t kSfMovieLocks = 0x458ef08;       // mutex per movie id (8 bytes each)
static const uintptr_t kHudContainerMovie = 0x54581a8;  // s32: HUD component 0 (hud.gfx) movie id
static const uintptr_t kSfStoreSlots = 0x459c968;  // ScaleformStore +0x40: slots, +0x50 size, +0x54 stride
static const uint32_t kSfMovieStride = 0x1e0;
static const uint32_t kSfMovieMax = 0x32;  // the manager refuses ids above 0x31
static const uint32_t kSfMovieActive = 3;
static const uintptr_t kGfxMovieRootVtable = 0x409cc08;
static const uintptr_t kGfxGetMovieDef = 0x36a9140;  // root vt+0x10
static const uintptr_t kGfxMovieDefImplVtable = 0x409c8d0;
static const uintptr_t kGfxGetResource = 0x369a0e0;  // def vt+0xe0 (def, export name) -> resource
static const uintptr_t kGfxSpriteDefVtable = 0x409d3d0;
static const uintptr_t kGfxGetLabeledFrame = 0x36d56f0;  // sprite vt+0x70 (def, label, u32*, bool)
static const uintptr_t kGfxTranslateFrame = 0x3692000;   // its tail call (NamedFrames lookup)
static const uintptr_t kGfxFrameNameAdd = 0x3695610;  // (hash*, hash*, {key*, u32*}*, hash value)
static const uint32_t kGfxNamedFrames = 0x28;          // GFxSpriteDef: NamedFrames table pointer
static const uint8_t kGfxGetMovieDefBytes[5] = {0x48, 0x8b, 0x47, 0x58, 0xc3};
static const uint8_t kGfxGetLabeledFrameBytes[9] = {0x48, 0x83, 0xc7, 0x28, 0xe9,
                                                    0x07, 0xc9, 0xfb, 0xff};
static const uint8_t kGfxGetResourceBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x48, 0x48, 0x8b, 0x1d, 0x68, 0x59, 0xa1, 0x00, 0x48, 0x8b, 0x03, 0x48, 0x89, 0x45, 0xd0, 0x48};
static const uint8_t kGfxTranslateFrameBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x88, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x1d, 0x45, 0xda, 0xa1, 0x00, 0x48, 0x8b, 0x03, 0x48, 0x89};
static const uint8_t kGfxFrameNameAddBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x48, 0x48, 0x8b, 0x07, 0x49, 0x89, 0xcd, 0x48, 0x89, 0xd3, 0x49, 0x89, 0xfe, 0x48, 0x85, 0xc0};
static const uint32_t kWheelSlots = 8;  // CWeaponInfo WheelSlot (info +0x58) = SLOT_WEAPONS_<n>

static uint32_t g_pack_wicons_applied;  // per merged wicon row (bit = row)
static_assert(GTAV_CUSTOM_PACK_WICON_CAP <= 32, "wicon row mask is uint32_t");

static int pack_wicon_anchors_ok() {
  const uintptr_t* root_vt = (const uintptr_t*)kGfxMovieRootVtable;
  const uintptr_t* def_vt = (const uintptr_t*)kGfxMovieDefImplVtable;
  const uintptr_t* sprite_vt = (const uintptr_t*)kGfxSpriteDefVtable;
  return pack_readable(kGfxMovieRootVtable, 0x18) && pack_readable(kGfxMovieDefImplVtable, 0xe8) &&
         pack_readable(kGfxSpriteDefVtable, 0x78) && root_vt[0x10 / 8] == kGfxGetMovieDef &&
         def_vt[0xe0 / 8] == kGfxGetResource && sprite_vt[0x70 / 8] == kGfxGetLabeledFrame &&
         memcmp((const void*)kGfxGetMovieDef, kGfxGetMovieDefBytes, sizeof(kGfxGetMovieDefBytes)) ==
             0 &&
         memcmp((const void*)kGfxGetLabeledFrame, kGfxGetLabeledFrameBytes,
                sizeof(kGfxGetLabeledFrameBytes)) == 0 &&
         memcmp((const void*)kGfxGetResource, kGfxGetResourceBytes, sizeof(kGfxGetResourceBytes)) ==
             0 &&
         memcmp((const void*)kGfxTranslateFrame, kGfxTranslateFrameBytes,
                sizeof(kGfxTranslateFrameBytes)) == 0 &&
         memcmp((const void*)kGfxFrameNameAdd, kGfxFrameNameAddBytes,
                sizeof(kGfxFrameNameAddBytes)) == 0 &&
         pack_pump_anchors_ok();  // the PLT mutex stubs
}

// The GFxMovieDefImpl of active movie `id`, or 0 (every pointer bounds- and vtable-checked).
static uintptr_t pack_sf_movie_def(int32_t id) {
  const uintptr_t movies = *(const volatile uintptr_t*)kSfMovies;
  const uint32_t count = *(const volatile uint16_t*)kSfMovieCount;
  if (id < 0 || (uint32_t)id >= count || (uint32_t)id >= kSfMovieMax || !movies) return 0;
  const uintptr_t entry = movies + (uintptr_t)id * kSfMovieStride;
  if (!pack_readable(entry, kSfMovieStride) ||
      *(const volatile uint32_t*)(entry + 0x1b8) != kSfMovieActive)
    return 0;
  const int32_t slot = *(const volatile int32_t*)(entry + 0xb0);
  const uintptr_t slots = *(const volatile uintptr_t*)kSfStoreSlots;
  const uint32_t size = *(const volatile uint32_t*)(kSfStoreSlots + 0x10);
  const int32_t stride = *(const volatile int32_t*)(kSfStoreSlots + 0x14);
  if (slot < 0 || (uint32_t)slot >= size || stride < 8 || !slots) return 0;
  const uintptr_t at = slots + (uintptr_t)slot * (uint32_t)stride;
  if (!pack_readable(at, 8)) return 0;
  const uintptr_t object = *(const volatile uintptr_t*)at;
  if (!object || !pack_readable(object + 0x18, 8)) return 0;
  const uintptr_t wrapper = *(const volatile uintptr_t*)(object + 0x18);
  if (!wrapper || !pack_readable(wrapper + 0x10, 8)) return 0;
  const uintptr_t root = *(const volatile uintptr_t*)(wrapper + 0x10);
  if (!root || !pack_readable(root, 0x60) ||
      *(const volatile uintptr_t*)root != kGfxMovieRootVtable)
    return 0;
  const uintptr_t def = *(const volatile uintptr_t*)(root + 0x58);
  return def && pack_readable(def, 0x40) &&
                 *(const volatile uintptr_t*)def == kGfxMovieDefImplVtable
             ? def
             : 0;
}

// Exported sprite `name` of movie def `def` (a GFxSpriteDef with a label table), or 0.
static uintptr_t pack_sf_sprite(uintptr_t def, const char* name) {
  typedef uintptr_t (*GetResourceFn)(uintptr_t, const char*);
  const uintptr_t sprite = ((GetResourceFn)kGfxGetResource)(def, name);
  return sprite && pack_readable(sprite, kGfxNamedFrames + 8) &&
                 *(const volatile uintptr_t*)sprite == kGfxSpriteDefVtable &&
                 *(const volatile uintptr_t*)(sprite + kGfxNamedFrames) != 0
             ? sprite
             : 0;
}

// The HUD movie: HUD component 0's movie when it exports SLOT_WEAPONS_0, else the first active
// movie that does. Returns its id (or -1) and the def.
static int32_t pack_hud_movie(uintptr_t* def_out) {
  const int32_t container = *(const volatile int32_t*)kHudContainerMovie;
  for (int32_t pass = -1; pass < (int32_t)kSfMovieMax; ++pass) {
    const int32_t id = pass < 0 ? container : pass;
    if (pass >= 0 && id == container) continue;
    const uintptr_t def = pack_sf_movie_def(id);
    if (def && pack_sf_sprite(def, "SLOT_WEAPONS_0")) {
      *def_out = def;
      return id;
    }
  }
  return -1;
}

// Frame index of `label` in sprite def `sprite`, or -1.
static int64_t pack_sf_frame(uintptr_t sprite, const char* label) {
  typedef bool (*GetLabeledFrameFn)(uintptr_t, const char*, uint32_t*, bool);
  uint32_t frame = 0;
  return ((GetLabeledFrameFn)kGfxGetLabeledFrame)(sprite, label, &frame, false) ? (int64_t)frame
                                                                                 : -1;
}

// NamedFrames hash of a label: GFx's case-insensitive Bernstein hash, last character first.
static uint64_t pack_sf_label_hash(const char* text, size_t length) {
  uint64_t hash = 5381u;
  while (length) {
    uint32_t c = (uint8_t)text[--length];
    if (c - 'A' < 26u) c += 0x20u;
    hash = (hash * 33u) ^ c;
  }
  return hash;
}

// Make `label` name frame `frame` of `sprite` (movie `id`): skip when it already does, refuse when
// it names another frame. Returns nullptr on success.
static const char* pack_sf_alias(int32_t id, uintptr_t sprite, const char* label, uint32_t frame,
                                 int* added) {
  typedef void (*AddFn)(uintptr_t, uintptr_t, const void*, uint64_t);
  typedef void (*MutexFn)(void*);
  // GString::DataDesc {size | ASCII flag, refcount, text} at 8-aligned memory; the key is a GString
  // with an explicit heap (tag 2) that is never a GFx heap, so add copies the text.
  struct alignas(8) {
    uint64_t size;
    int32_t refs;
    char text[20];
  } desc;
  static const uint8_t kForeignHeap = 0;
  const uintptr_t key[2] = {(uintptr_t)&desc | 2u, (uintptr_t)&kForeignHeap};
  const void* node[2] = {key, &frame};
  *added = 0;
  const int64_t have = pack_sf_frame(sprite, label);
  if (have == (int64_t)frame) return nullptr;
  if (have >= 0) return "label names another frame";
  const size_t length = strlen(label);
  if (length >= sizeof(desc.text)) return "label too long";
  desc.size = (uint64_t)length | (1ull << 63);
  desc.refs = 1;
  memcpy(desc.text, label, length + 1);
  const uintptr_t table = sprite + kGfxNamedFrames;
  void* lock = (void*)(kSfMovieLocks + (uintptr_t)id * 8u);
  ((MutexFn)kLockMutex)(lock);
  ((AddFn)kGfxFrameNameAdd)(table, table, node, pack_sf_label_hash(label, length));
  ((MutexFn)kUnlockMutex)(lock);
  *added = 1;
  return pack_sf_frame(sprite, label) == (int64_t)frame ? nullptr : "alias did not resolve";
}

// Apply the unapplied wicon rows whose weapon the WEAPONINFO row just parsed added (`names`).
static void pack_wicon_apply(const uint32_t* names, uint32_t name_count) {
  int rows = 0, applied = 0;
  for (int i = 0; i < g_pack.wicon_count; ++i) {
    const uint32_t weapon = pack_joaat(g_pack.wicons[i].weapon);
    int ours = 0;
    for (uint32_t n = 0; n < name_count; ++n) ours |= names[n] == weapon;
    if (!ours || (g_pack_wicons_applied & (1u << i))) continue;
    rows++;
    const uint32_t donor = pack_joaat(g_pack.wicons[i].donor);
    uintptr_t info = 0, donor_info = 0, def = 0;
    uint32_t slot = UINT32_MAX, donor_slot = UINT32_MAX;
    int32_t id = -1;
    int64_t frames[2] = {-1, -1};
    int added[2] = {0, 0};
    const char* refusal = nullptr;
    if (pack_weapon_known(weapon, &info) != 1 || !pack_readable(info + 0x58, 4))
      refusal = "weapon not loaded";
    else if (pack_weapon_known(donor, &donor_info) != 1 || !pack_readable(donor_info + 0x58, 4))
      refusal = "donor weapon unknown";
    else if ((slot = *(const volatile uint32_t*)(info + 0x58)) >= kWheelSlots)
      refusal = "weapon WheelSlot out of range";
    else if ((donor_slot = *(const volatile uint32_t*)(donor_info + 0x58)) != slot)
      refusal = "donor is in another WheelSlot";
    else if (!pack_wicon_anchors_ok())
      refusal = "scaleform anchors mismatch";
    else if ((id = pack_hud_movie(&def)) < 0)
      refusal = "HUD movie not found";
    char label[24], donor_label[24], sprite_name[24];
    snprintf(label, sizeof(label), "INT%d", (int)(int32_t)weapon);
    snprintf(donor_label, sizeof(donor_label), "INT%d", (int)(int32_t)donor);
    // Group sprite first (the on-foot wheel), then MASTER_WEAPONS (the vehicle wheel).
    for (int s = 0; s < 2 && !refusal; ++s) {
      if (s)
        snprintf(sprite_name, sizeof(sprite_name), "MASTER_WEAPONS");
      else
        snprintf(sprite_name, sizeof(sprite_name), "SLOT_WEAPONS_%u", (unsigned)slot);
      const uintptr_t sprite = pack_sf_sprite(def, sprite_name);
      if (!sprite) refusal = s ? "MASTER_WEAPONS not found" : "SLOT_WEAPONS sprite not found";
      else if ((frames[s] = pack_sf_frame(sprite, donor_label)) < 0)
        refusal = s ? "donor has no MASTER_WEAPONS frame" : "donor has no wheel frame";
      else
        refusal = pack_sf_alias(id, sprite, label, (uint32_t)frames[s], &added[s]);
    }
    if (!refusal) {
      g_pack_wicons_applied |= 1u << i;
      applied++;
    }
    char line[224];
    snprintf(line, sizeof(line),
             "GTAVMenu pack wicon %d %.40s -> %.40s %s slot=%u/%u movie=%d frames=%lld/%lld "
             "added=%d/%d%s%s",
             i, g_pack.wicons[i].weapon, g_pack.wicons[i].donor, label, (unsigned)slot,
             (unsigned)donor_slot, (int)id, (long long)frames[0], (long long)frames[1], added[0],
             added[1], refusal ? " refused: " : " ok", refusal ? refusal : "");
    gtav_worker_klog(line);
  }
  if (rows)
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack wicon applied %d/%d", applied, rows);
}

// Load data row `param` of the active pack through its mounter, once per row and process.
static uint32_t load_pack_data(uint32_t param) {
  typedef bool (*LoadDataFileFn)(void*, const void*);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.data_count) return unavailable(action, "pack data row out of range");
  if (g_pack_data_loaded & (1ull << param)) return unavailable(action, "pack data row already loaded");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  const GtavCustomPackData* data = &g_pack.data[param];
  const char* type_name = gtav_custom_pack_data_type_name(data->type);
  size_t m = 0;
  while (m < sizeof(kDataFileMounters) / sizeof(kDataFileMounters[0]) &&
         kDataFileMounters[m].type != data->type)
    ++m;
  if (!type_name || m == sizeof(kDataFileMounters) / sizeof(kDataFileMounters[0]) ||
      data->type >= kDataFileInterfaceCount)
    return unavailable(action, "pack data type has no pinned mounter");
  const uintptr_t iface = ((const volatile uintptr_t*)kDataFileInterfaces)[data->type];
  if (iface != kDataFileMounters[m].iface || !pack_readable(iface, 8) ||
      *(const volatile uintptr_t*)iface != kDataFileMounters[m].vtable ||
      ((const volatile uintptr_t*)kDataFileMounters[m].vtable)[2] != kDataFileMounters[m].load ||
      memcmp((const void*)kDataFileMounters[m].load, kDataFileMounters[m].pin,
             sizeof(kDataFileMounters[m].pin)) != 0) {
    char line[GTAV_FEATURE_MESSAGE_LEN + 16];
    snprintf(line, sizeof(line), "GTAVMenu pack data %u type=%u mounter anchors mismatch",
             (unsigned)param, (unsigned)data->type);
    gtav_worker_klog(line);
    return unavailable(action, "pack data mounter anchors mismatch");
  }
  const int sync = kDataFileMounters[m].sync;

  char path[GTAV_PACK_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", pack_data_dir(param), data->file);
  char name[0x80];  // DataFile filename field is char[0x80]
  if (const char* refusal = pack_stage_data_file(param, path, name, sizeof(name)))
    return unavailable(action, refusal);
  static uint32_t hashes[GTAV_PACK_DATA_NAMES_MAX];
  const uint32_t names =
      data->type == GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD
          ? pack_tc_modifier_names(param, hashes, GTAV_PACK_DATA_NAMES_MAX)
      : kDataFileMounters[m].names
          ? pack_data_names(param, kDataFileMounters[m].names, kDataFileMounters[m].anchor, hashes,
                            GTAV_PACK_DATA_NAMES_MAX)
          : 0;

  if (!pack_pump_anchors_ok()) return unavailable(action, "pack data pump anchors mismatch");
  const uintptr_t parser = pack_parser();
  if (!parser || *(const volatile uint16_t*)(parser + 0x78) != 0 ||
      *(const volatile uintptr_t*)(parser + 0x80) != 0)
    return unavailable(action, "parser is not idle");
  // The in-use refusal repeats while the autoload waits (every ~2 s): klog its first one per row.
  static int32_t in_use_row = -1;
  if (const char* refusal = pack_data_gate(param, names, hashes)) {
    if (refusal != kPackCarcolsInUse || in_use_row != (int32_t)param) {
      char line[GTAV_FEATURE_MESSAGE_LEN + 16];
      snprintf(line, sizeof(line), "GTAVMenu pack data %u gate refused: %s", (unsigned)param,
               refusal);
      gtav_worker_klog(line);
    }
    in_use_row = refusal == kPackCarcolsInUse ? (int32_t)param : -1;
    return unavailable(action, refusal);
  }
  in_use_row = -1;
  // Sync rows: the store whose growth pack_data_sync_verify checks, read right before the call.
  uint32_t sync_before = 0, capacity = 0;
  if (sync) {
    int store = -1;
    PackTcState tc;
    switch (data->type) {
      case GTAV_CUSTOM_PACK_DATA_PED_METADATA:
        store = pack_model_factory_block(kPedModelFactory, kPedModelFactoryVtable, &sync_before,
                                         &capacity);
        break;
      case GTAV_CUSTOM_PACK_DATA_WEAPON_METADATA:
        store = pack_model_factory_block(kWeaponModelFactory, kWeaponModelFactoryVtable,
                                         &sync_before, &capacity);
        break;
      case GTAV_CUSTOM_PACK_DATA_TIMECYCLEMOD:
        store = pack_tc_state(&tc);
        sync_before = tc.count;
        break;
      case GTAV_CUSTOM_PACK_DATA_AUDIO_GAMEDATA: {
        PackAudioMgr audio;
        store = pack_audio_mgr(&audio);
        sync_before = audio.count;
        break;
      }
      default:
        break;
    }
    if (store != 0) return unavailable(action, "sync row store unreadable");
  }

  if (const char* refusal = pack_fill_loose_entry(name, data->size))
    return unavailable(action, refusal);
  g_pack_data_loaded |= 1ull << param;
  memset(g_pack_datafile, 0, sizeof(g_pack_datafile));
  memcpy(g_pack_datafile, name, strlen(name));
  *(uint32_t*)(g_pack_datafile + 0x90) = data->type;
  gtav_worker_klog("GTAVMenu pack data load before");
  const bool loaded =
      ((LoadDataFileFn)kDataFileMounters[m].load)((void*)iface, g_pack_datafile);
  gtav_worker_klog("GTAVMenu pack data load returned");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  if (sync) {
    // Parsed already: nothing may be queued, and the new models must be registered.
    const int clean = *(const volatile uint16_t*)(parser + 0x78) == 0 &&
                      pack_data_sync_verify(param, names, hashes, sync_before);
    if (loaded && clean) __atomic_or_fetch(&g_pack_data_parsed, 1ull << param, __ATOMIC_RELEASE);
    snprintf(message, sizeof(message), "pack data %u %s sync returned %d verified %d",
             (unsigned)param, type_name, (int)loaded, clean);
    return loaded && clean ? ok(action, message) : failed(action, message);
  }
  // Exactly one request must now be pending: ours. pump_pack_data parses it once its read is done.
  const uintptr_t requests = *(const volatile uintptr_t*)(parser + 0x70);
  if (*(const volatile uint16_t*)(parser + 0x78) == 1 && requests && pack_readable(requests, 8)) {
    g_pack_pending_request = *(const volatile uintptr_t*)requests;
    g_pack_pending_row = (int32_t)param;
    g_pack_pending_name_count = names < GTAV_PACK_DATA_NAMES_MAX ? names : GTAV_PACK_DATA_NAMES_MAX;
    memcpy(g_pack_pending_names, hashes, g_pack_pending_name_count * sizeof(uint32_t));
  }
  snprintf(message, sizeof(message), "pack data %u %s queued=%d returned %d", (unsigned)param,
           type_name, g_pack_pending_row == (int32_t)param, (int)loaded);
  if (data->type == GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL) {
    char line[GTAV_FEATURE_MESSAGE_LEN + 16];
    snprintf(line, sizeof(line), "GTAVMenu %s", message);
    gtav_worker_klog(line);
  }
  return loaded && g_pack_pending_row == (int32_t)param ? ok(action, message)
                                                         : failed(action, message);
}

// Light and siren settings ids.
// The CARCOLS merge appends a Sirens item (0x5e8 B, u8 id at +0) whose id is new and then records
// id -> index in a 256-byte map (ELF 0x41a50e0; filled at 0x1023c92, pinned below). The
// VEHICLE_VARIATION parse resolves <lightSettings>/<sirenSettings> through the light/siren maps and
// stores the indices at model info +0x53a/+0x53b (0x1021797, pinned); the siren renderer indexes
// the array with +0x53b without a bound (0x22cc5b9; 0 = no sirens), so an id the map lacks (index
// 0xff) must never reach a lit siren. Read-only: pack-notes lines only.
static const uintptr_t kSirenIdMap = 0x45a50e0;
static const uintptr_t kSirenIdMapCode = 0x1423c8b;
static const uint8_t kSirenIdMapCodeBytes[] = {0x49, 0x8b, 0x4a, 0x48, 0x49, 0x63, 0xc7,
                                               0x48, 0x8d, 0x35, 0x47, 0x14, 0x18, 0x03};
static const uintptr_t kVariationLightSirenCode = 0x1421797;
static const uint8_t kVariationLightSirenCodeBytes[] = {
    0x43, 0x0f, 0xb6, 0x44, 0x34, 0x48, 0x88, 0x83, 0x3a, 0x05, 0x00, 0x00,
    0x43, 0x0f, 0xb6, 0x44, 0x34, 0x49, 0x88, 0x83, 0x3b, 0x05, 0x00, 0x00};

static void pack_world_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void pack_note_light_sirens(int32_t row, const PackVarArrays* before,
                                   const PackVarArrays* after) {
  const uint32_t type = g_pack.data[row].type;
  if (type == GTAV_CUSTOM_PACK_DATA_CARCOLS) {
    pack_world_note("pack data %d carcols kits %u->%u lights %u->%u sirens %u->%u", (int)row,
                    (unsigned)before->kit_count, (unsigned)after->kit_count,
                    (unsigned)before->light_count, (unsigned)after->light_count,
                    (unsigned)before->siren_count, (unsigned)after->siren_count);
    const int map_ok =
        memcmp((const void*)kSirenIdMapCode, kSirenIdMapCodeBytes, sizeof(kSirenIdMapCodeBytes)) ==
            0 &&
        pack_readable(kSirenIdMap, 256);
    for (uint32_t i = before->siren_count; i < after->siren_count && i < before->siren_count + 4u;
         ++i) {
      const uintptr_t item = after->sirens + (uintptr_t)i * 0x5e8u;
      if (!pack_readable(item, 1)) break;
      const uint8_t id = *(const volatile uint8_t*)item;
      const int index = map_ok ? ((const volatile uint8_t*)kSirenIdMap)[id] : -1;
      pack_world_note("pack data %d siren item %u id=%u map=%d %s", (int)row, (unsigned)i,
                      (unsigned)id, index, index == (int)i ? "ok" : "MISMATCH");
    }
    return;
  }
  if (type != GTAV_CUSTOM_PACK_DATA_VEHICLE_VARIATION) return;
  if (memcmp((const void*)kVariationLightSirenCode, kVariationLightSirenCodeBytes,
             sizeof(kVariationLightSirenCodeBytes)) != 0) {
    pack_world_note("pack data %d siren check skipped: variation code mismatch", (int)row);
    return;
  }
  uint32_t hashes[4];
  const uint32_t names = pack_data_names((uint32_t)row, "modelName", nullptr, hashes, 4);
  for (uint32_t i = 0; i < names && i < 4u; ++i) {
    uintptr_t model = 0;
    if (pack_model_index(hashes[i], &model) < 0 || !pack_readable(model + 0x53a, 2)) continue;
    const uint32_t light = *(const volatile uint8_t*)(model + 0x53a);
    const uint32_t siren = *(const volatile uint8_t*)(model + 0x53b);
    const uintptr_t light_item = after->lights + (uintptr_t)light * 0x1a8u;
    const uintptr_t siren_item = after->sirens + (uintptr_t)siren * 0x5e8u;
    const int light_id = light < after->light_count && pack_readable(light_item, 1)
                             ? *(const volatile uint8_t*)light_item
                             : -1;
    const int siren_id = siren < after->siren_count && pack_readable(siren_item, 1)
                             ? *(const volatile uint8_t*)siren_item
                             : -1;
    pack_world_note("pack data %d model 0x%08x lights idx=%u id=%d sirens idx=%u id=%d %s",
                    (int)row, (unsigned)hashes[i], (unsigned)light, light_id, (unsigned)siren,
                    siren_id,
                    !siren          ? "no sirens"
                    : siren_id >= 0 ? "ok"
                                    : "UNRESOLVED: do not switch this car's siren on");
  }
}

// Wheels a CARCOLS row appended: per changed type the count, and for
// the first new items the wheelName hash and its drawable store slot (the wheel request looks the
// hash up there, 0xfdcc10; no slot = the wheel draws nothing). A wheel index above 254 cannot be
// selected (u8 mod index, 0xff = stock).
static void pack_note_wheels(int32_t row, const PackWheelArrays* before,
                             const PackWheelArrays* after) {
  uint32_t shown = 0;
  for (uint32_t t = 0; t < PACK_WHEEL_TYPES; ++t) {
    if (after->count[t] == before->count[t]) continue;
    pack_world_note("pack data %d wheels type %u %u->%u/%u%s", (int)row, (unsigned)t,
                    (unsigned)before->count[t], (unsigned)after->count[t],
                    (unsigned)after->capacity[t], after->count[t] > 255u ? " OVER 255" : "");
    for (uint32_t i = before->count[t]; i < after->count[t] && shown < 4u; ++i, ++shown) {
      const uintptr_t item = after->data[t] + (uintptr_t)i * 0x38u;
      if (!pack_readable(item + 0x20, 4)) break;
      const uint32_t hash = *(const volatile uint32_t*)(item + 0x20);
      const int32_t slot = pack_store_slot(kDrawableStore, hash);
      pack_world_note("pack data %d wheel type %u index %u name 0x%08x drawable=%d %s", (int)row,
                      (unsigned)t, (unsigned)i, (unsigned)hash, (int)slot,
                      slot < 0 ? "NO DRAWABLE" : "ok");
    }
  }
}

// Parse the queued data row (defect B): call the parser pump on the main thread, as the game's
// changeset loop does after each data-file action, but only when the parser holds exactly our
// request and its read completed with the full size. Retried by the operator until ready.
static uint32_t pump_pack_data() {
  typedef void (*PumpFn)(uintptr_t);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PUMP_PACK_DATA;
  if (g_pack_pending_row < 0) return unavailable(action, "no pack data row is queued");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!pack_pump_anchors_ok()) return unavailable(action, "pack data pump anchors mismatch");
  const GtavCustomPackData* data = &g_pack.data[g_pack_pending_row];
  const uintptr_t parser = pack_parser();
  const uintptr_t requests = parser ? *(const volatile uintptr_t*)(parser + 0x70) : 0;
  const uintptr_t request = g_pack_pending_request;
  if (!parser || *(const volatile uint16_t*)(parser + 0x78) != 1 ||
      *(const volatile uintptr_t*)(parser + 0x80) != 0 || !requests ||
      !pack_readable(requests, 8) || *(const volatile uintptr_t*)requests != request ||
      !pack_readable(request, 0x88))
    return unavailable(action, "parser does not hold exactly our request");
  const uint8_t ready = *(const volatile uint8_t*)(request + 0x81);
  const uint64_t read_size = *(const volatile uint64_t*)(request + 0x68);
  const uintptr_t read_data = *(const volatile uintptr_t*)(request + 0x60);
  if (!ready) return unavailable(action, "pack data read pending; retry");
  if (read_size != data->size || !read_data || *(const volatile uint8_t*)kParserPumpGuard != 0) {
    char message[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(message, sizeof(message), "pack data read size=%llu data=%d guard=%u refused",
             (unsigned long long)read_size, read_data != 0,
             (unsigned)*(const volatile uint8_t*)kParserPumpGuard);
    return unavailable(action, message);
  }
  const uint32_t handling_before = *(const volatile uint16_t*)kHandlingCount;
  uint32_t store_before = 0, store_capacity = 0, store_after = 0;
  pack_vehicle_model_store(&store_before, &store_capacity);
  const uint32_t free_before = *(const volatile uint32_t*)kArchetypePoolFree;
  const int32_t row = g_pack_pending_row;
  __atomic_store_n(&g_pack_pending_row, -1, __ATOMIC_RELEASE);  // one pump per queued row
  const uint32_t weapons_before = *(const volatile uint16_t*)(kWeaponInfoByHash + 8);
  const int32_t components_before = pack_component_count();
  PackVarArrays vars_before, vars_after;
  const int vars_ok = pack_var_arrays(&vars_before) == 0;
  PackWheelArrays wheels_before, wheels_after;
  const int wheels_ok = g_pack.data[row].type == GTAV_CUSTOM_PACK_DATA_CARCOLS &&
                        pack_wheel_arrays(&wheels_before) == 0;
  if (vars_ok) pack_log_var_arrays("before", row, &vars_before);
  gtav_worker_klog("GTAVMenu pack data pump before");
  ((PumpFn)kParserPump)(parser);
  gtav_worker_klog("GTAVMenu pack data pump returned");
  if (vars_ok && pack_var_arrays(&vars_after) == 0) {
    pack_log_var_arrays("after", row, &vars_after);
    if (vars_after.kits != vars_before.kits || vars_after.lights != vars_before.lights ||
        vars_after.sirens != vars_before.sirens)
      gtav_worker_klog("GTAVMenu pack data pump moved a vehicle variation array");
    pack_note_light_sirens(row, &vars_before, &vars_after);
  }
  if (wheels_ok && pack_wheel_arrays(&wheels_after) == 0)
    pack_note_wheels(row, &wheels_before, &wheels_after);
  pack_vehicle_model_store(&store_after, &store_capacity);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message),
           "pack data %d parsed handling %u->%u vehicles %u->%u archetypes_free %u->%u",
           (int)row, (unsigned)handling_before, (unsigned)*(const volatile uint16_t*)kHandlingCount,
           (unsigned)store_before, (unsigned)store_after, (unsigned)free_before,
           (unsigned)*(const volatile uint32_t*)kArchetypePoolFree);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
  // The worker's autoload advances on this bit (the pending row is cleared before the pump).
  __atomic_or_fetch(&g_pack_data_parsed, 1ull << row, __ATOMIC_RELEASE);
  if (g_pack.data[row].type == GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL) {
    const int added = pack_apparel_verify(row, message, sizeof(message));
    gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
    return added ? ok(action, message) : failed(action, message);
  }
  if (g_pack.data[row].type == GTAV_CUSTOM_PACK_DATA_WEAPONCOMPONENTSINFO) {
    const int added = pack_component_verify(row, components_before, g_pack_pending_names,
                                            g_pack_pending_name_count, message, sizeof(message));
    gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
    return added ? ok(action, message) : failed(action, message);
  }
  if (g_pack.data[row].type == GTAV_CUSTOM_PACK_DATA_WEAPONINFO) {
    // The PostLoad rebuilt the sorted by-hash array: every new weapon must now resolve.
    // klog each weapon's Model hash (CItemInfo +0x14) and whether that archetype is registered:
    // a pack weapon model (type 72 row) must already be in the map when weapons.meta lands.
    uint32_t known = 0;
    for (uint32_t i = 0; i < g_pack_pending_name_count; ++i) {
      uintptr_t info = 0;
      const int found = pack_weapon_known(g_pack_pending_names[i], &info) == 1;
      known += found;
      if (i >= 8u) continue;
      const uint32_t model =
          found && pack_readable(info + 0x14, 4) ? *(const volatile uint32_t*)(info + 0x14) : 0;
      char line[GTAV_FEATURE_MESSAGE_LEN + 16];
      snprintf(line, sizeof(line),
               "GTAVMenu pack data %d weapon 0x%08x found=%d model=0x%08x known=%d", (int)row,
               (unsigned)g_pack_pending_names[i], found, (unsigned)model,
               model ? pack_model_known(model) : -1);
      gtav_worker_klog(line);
    }
    snprintf(message, sizeof(message), "pack data %d weapons %u->%u known=%u/%u", (int)row,
             (unsigned)weapons_before, (unsigned)*(const volatile uint16_t*)(kWeaponInfoByHash + 8),
             (unsigned)known, (unsigned)g_pack_pending_name_count);
    gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, message);
    if (known != g_pack_pending_name_count) return failed(action, message);
    // Wheel icons are cosmetic: a refused row is logged, the weapons stay loaded.
    pack_wicon_apply(g_pack_pending_names, g_pack_pending_name_count);
  }
  return ok(action, message);
}

// Custom Packs `spawn timecycle` row (param = joaat of the modifier name): apply the pack modifier
// at full strength, or clear it when it is the one applied. Main thread (run_gated_action), the
// lane of the Screen Effect cycler. Success is read back from the index SET_TIMECYCLE_MODIFIER
// stores (what GET_TIMECYCLE_MODIFIER_INDEX returns) and logged to the klog.
static uint32_t apply_pack_timecycle(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_APPLY_PACK_TIMECYCLE;
  if (!g_n.set_timecycle_modifier || !g_n.clear_timecycle_modifier)
    return unavailable(action, "missing timecycle natives");
  const char* name = nullptr;
  if (__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE))
    for (int i = 0; i < g_pack.spawn_count && !name; ++i)
      if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_TIMECYCLE &&
          pack_joaat(g_pack.spawns[i].model) == param)
        name = g_pack.spawns[i].model;
  if (!name) return unavailable(action, "not a screen effect of the loaded packs");
  PackTcState s;
  uint32_t mods = 0;
  const int readable = pack_tc_state(&s) == 0 && pack_readable(kTcCurrentIndex, 4);
  const int32_t index = readable ? pack_tc_index(&s, param, &mods) : -1;
  char line[GTAV_FEATURE_MESSAGE_LEN + 16];
  if (index < 0 || !mods) {
    snprintf(line, sizeof(line),
             "GTAVMenu pack tc %s 0x%08x not usable index=%d mods=%u modifiers=%u readable=%d",
             name, (unsigned)param, (int)index, (unsigned)mods, (unsigned)s.count, readable);
    gtav_worker_klog(line);
    return failed(action, "pack screen effect is not loaded (see klog)");
  }
  const int32_t current = *(const volatile int32_t*)kTcCurrentIndex;
  const int clear = current == index;
  if (clear) {
    invoke_void(g_n.clear_timecycle_modifier);
  } else {
    invoke_void(g_n.set_timecycle_modifier, name);
    if (g_n.set_timecycle_modifier_strength)
      invoke_void(g_n.set_timecycle_modifier_strength, 1.0f);
  }
  const int32_t after = *(const volatile int32_t*)kTcCurrentIndex;
  snprintf(line, sizeof(line), "GTAVMenu pack tc %s %s 0x%08x index=%d mods=%u current %d->%d",
           clear ? "clear" : "apply", name, (unsigned)param, (int)index, (unsigned)mods,
           (int)current, (int)after);
  gtav_worker_klog(line);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  if (clear)
    snprintf(message, sizeof(message), "screen effect %.32s off", name);
  else
    snprintf(message, sizeof(message), "screen effect %.32s on (index %d)", name, (int)after);
  const int good = clear ? after == -1 : after == index;
  return good ? ok(action, message) : failed(action, message);
}

// Pack particle effects: a `<asset>.ppt`
// member of a pack archive is FindOrAdded into the PtFxAssetStore by RegisterFile at LoadImage
// (extension "ppt" = module type 12), the route retail DLC ptfx.rpf archives take; no data row.
// Store (ELF 0x5e26148; final vtable written at 0x316d0a9): +8 streaming base, slot pool +0x40
// defs (stride +0x54 = 0x18; def +0xc name hash, +0x10 parent local index, -1 until a session init
// or a PTFXASSETINFO post-parse links it to "core"), +0x48 flag bytes (bit 7 = free), +0x50 size,
// +0x58 first free (-1 = full), +0x60 used (low 30 bits); name map +0x78 nodes {u32 hash, s32
// slot, u32 next}, +0x80 u32 bucket heads, +0x88 bucket count. REQUEST_NAMED_PTFX_ASSET (live
// 0x1c0ada0) and the effect start's USE_PARTICLE_FX_ASSET lookup (0x1aa0286) walk this map with
// the lowercase joaat; the effect rule is then looked up under that asset (0x3594afb).
static const uintptr_t kPtfxStore = 0x6226148;
static const uintptr_t kPtfxStoreVtable = 0x4097eb0;
static const uint32_t kPtfxDefStride = 0x18;

struct PackPtfxSlot {
  int32_t slot;     // local slot index, -1 when the name is not in the store
  uint32_t status;  // streaming-info word (bits 0-1: 0 none, 1 loaded, 2 requested), or ~0
  int32_t parent;   // def +0x10 (-1: no parent dictionary yet)
  uint32_t used, size;
};

// Read-only store view of particle dictionary `asset` (no game code runs, so the worker may call
// it). Returns 0 when the store is readable (slot may still be -1), -1 otherwise.
static int pack_ptfx_slot(const char* asset, PackPtfxSlot* out) {
  out->slot = -1;
  out->status = UINT32_MAX;
  out->parent = -1;
  out->used = out->size = 0;
  if (!pack_readable(kPtfxStore, 0x90) ||
      *(const volatile uintptr_t*)kPtfxStore != kPtfxStoreVtable ||
      *(const volatile uint32_t*)(kPtfxStore + 0x54) != kPtfxDefStride)
    return -1;
  out->size = *(const volatile uint32_t*)(kPtfxStore + 0x50);
  out->used = *(const volatile uint32_t*)(kPtfxStore + 0x60) & 0x3fffffffu;
  const uintptr_t nodes = *(const volatile uintptr_t*)(kPtfxStore + 0x78);
  const uintptr_t buckets = *(const volatile uintptr_t*)(kPtfxStore + 0x80);
  const uint32_t bucket_count = *(const volatile uint32_t*)(kPtfxStore + 0x88);
  const uintptr_t defs = *(const volatile uintptr_t*)(kPtfxStore + 0x40);
  const uintptr_t flags = *(const volatile uintptr_t*)(kPtfxStore + 0x48);
  if (!bucket_count || !pack_readable(buckets, (size_t)bucket_count * 4u) || !defs || !flags)
    return -1;
  const uint32_t hash = pack_joaat(asset);
  uint32_t node = *(const volatile uint32_t*)(buckets + (uintptr_t)(hash % bucket_count) * 4u);
  for (uint32_t steps = 0; node != UINT32_MAX && steps < 4096u; ++steps) {
    const uintptr_t at = nodes + (uintptr_t)node * 12u;
    if (!pack_readable(at, 12)) return -1;
    if (*(const volatile uint32_t*)at == hash) {
      const int32_t slot = *(const volatile int32_t*)(at + 4);
      if (slot < 0 || (uint32_t)slot >= out->size || !pack_readable(flags + (uint32_t)slot, 1) ||
          *(const volatile int8_t*)(flags + (uint32_t)slot) < 0)
        return 0;
      const uintptr_t def = defs + (uintptr_t)(uint32_t)slot * kPtfxDefStride;
      if (!pack_readable(def, kPtfxDefStride)) return -1;
      out->slot = slot;
      out->parent = *(const volatile int32_t*)(def + 0x10);
      const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
      const uint32_t base = *(const volatile uint32_t*)(kPtfxStore + 8);
      const uintptr_t info = infos + ((uintptr_t)base + (uint32_t)slot) * 8u + 4u;
      if (infos && pack_readable(info, 4)) out->status = *(const volatile uint32_t*)info;
      return 0;
    }
    node = *(const volatile uint32_t*)(at + 8);
  }
  return 0;
}

// `spawn ptfx <asset>:<effect>` row whose model text hashes to `param`: copies the two names.
static int pack_ptfx_row(uint32_t param, char* asset, char* effect) {
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.spawn_count; ++i) {
    const char* model = g_pack.spawns[i].model;
    const char* colon = strchr(model, ':');
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_PTFX || !colon ||
        pack_joaat(model) != param)
      continue;
    snprintf(asset, GTAV_CUSTOM_PACK_NAME_MAX, "%.*s", (int)(colon - model), model);
    snprintf(effect, GTAV_CUSTOM_PACK_NAME_MAX, "%s", colon + 1);
    return 1;
  }
  return 0;
}

// After a pack load: one klog line per `spawn ptfx` row saying whether its dictionary name has a
// PtFxAssetStore slot (created by the archive registration), with the slot's status and parent.
static void pack_ptfx_report() {
  for (int i = 0; i < g_pack.spawn_count; ++i) {
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_PTFX) continue;
    char asset[GTAV_CUSTOM_PACK_NAME_MAX], effect[GTAV_CUSTOM_PACK_NAME_MAX];
    if (!pack_ptfx_row(pack_joaat(g_pack.spawns[i].model), asset, effect)) continue;
    PackPtfxSlot s;
    const int readable = pack_ptfx_slot(asset, &s) == 0;
    char line[192];
    snprintf(line, sizeof(line),
             "GTAVMenu pack ptfx asset %s slot=%d status=0x%x parent=%d store=%u/%u readable=%d %s",
             asset, (int)s.slot, (unsigned)s.status, (int)s.parent, (unsigned)s.used,
             (unsigned)s.size, readable, s.slot >= 0 ? "resolved" : "NOT IN STORE");
    gtav_worker_klog(line);
  }
}

// Custom Packs `spawn ptfx` row (param = joaat of "<asset>:<effect>"): request the pack particle
// dictionary, re-queue on the game thread until it is loaded (bounded), then play the effect once
// at the player's feet. Natives: the Fireworks job's (REQUEST_NAMED_PTFX_ASSET,
// HAS_NAMED_PTFX_ASSET_LOADED, USE_PARTICLE_FX_ASSET, SET_PARTICLE_FX_NON_LOOPED_COLOUR,
// START_PARTICLE_FX_NON_LOOPED_AT_COORD); START_NETWORKED_* is not in the reviewed table.
static uint32_t play_pack_ptfx(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_PLAY_PACK_PTFX;
  static time_t deadline = 0;
  static time_t started_at = 0;
  static uint32_t pending = 0;
  if (!g_n.request_named_ptfx_asset || !g_n.has_named_ptfx_asset_loaded ||
      !g_n.use_particle_fx_asset || !g_n.start_particle_fx_non_looped_at_coord ||
      !g_n.get_entity_coords || !g_n.player_ped_id) {
    deadline = 0;
    return unavailable(action, "missing particle natives");
  }
  char asset[GTAV_CUSTOM_PACK_NAME_MAX], effect[GTAV_CUSTOM_PACK_NAME_MAX];
  if (!pack_ptfx_row(param, asset, effect)) {
    deadline = 0;
    return unavailable(action, "not a particle effect of the loaded packs");
  }
  const int first = deadline == 0 || pending != param;
  PackPtfxSlot s;
  const int readable = pack_ptfx_slot(asset, &s) == 0;
  char line[192];
  if (first) {
    snprintf(line, sizeof(line),
             "GTAVMenu pack ptfx request %s slot=%d status=0x%x parent=%d store=%u/%u readable=%d",
             asset, (int)s.slot, (unsigned)s.status, (int)s.parent, (unsigned)s.used,
             (unsigned)s.size, readable);
    gtav_worker_klog(line);
  }
  // REQUEST_NAMED_PTFX_ASSET of a name with no slot silently does nothing: refuse with the reason.
  if (!readable || s.slot < 0) {
    deadline = 0;
    return failed(action, "pack effect dictionary has no store slot (see klog)");
  }
  invoke_void(g_n.request_named_ptfx_asset, asset);
  if (!invoke_return<int>(g_n.has_named_ptfx_asset_loaded, asset)) {
    if (first) {
      pending = param;
      started_at = time(NULL);
      deadline = started_at + GTAV_MODEL_STREAM_WAIT_SECONDS;
    }
    if (time(NULL) < deadline && gtav_frame_hook_enqueue(action, param))
      return ok(action, "pack effect loading");
    snprintf(line, sizeof(line),
             "GTAVMenu pack ptfx load timeout %s slot=%d status=0x%x waited=%llds", asset,
             (int)s.slot, (unsigned)s.status, (long long)(time(NULL) - started_at));
    gtav_worker_klog(line);
    deadline = 0;
    return failed(action, "pack effect did not load (see klog)");
  }
  snprintf(line, sizeof(line),
           "GTAVMenu pack ptfx loaded %s slot=%d status=0x%x parent=%d waited=%llds", asset,
           (int)s.slot, (unsigned)s.status, (int)s.parent,
           first ? 0ll : (long long)(time(NULL) - started_at));
  gtav_worker_klog(line);
  deadline = 0;
  const int ped = native_player_ped_id();
  if (!ped) return failed(action, "player ped unavailable");
  const Vec3 c = invoke_vector(g_n.get_entity_coords, ped, 1);
  invoke_void(g_n.use_particle_fx_asset, asset);
  // White tint: some effects (the fireworks) render near-black without one; white keeps the
  // authored look of the rest and is the brightest choice.
  if (g_n.set_particle_fx_non_looped_colour)
    invoke_void(g_n.set_particle_fx_non_looped_colour, 1.0f, 1.0f, 1.0f);
  // Ped coordinates are about 1 m above the ground; the effect starts at the feet.
  const int started = invoke_return<int>(g_n.start_particle_fx_non_looped_at_coord, effect, c.x,
                                         c.y, c.z - 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0, 0, 0);
  snprintf(line, sizeof(line), "GTAVMenu pack ptfx start %s:%s at %.1f %.1f %.1f result=%d", asset,
           effect, (double)c.x, (double)c.y, (double)(c.z - 1.0f), started);
  gtav_worker_klog(line);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "effect %.40s %s", effect,
           started ? "started" : "start returned 0");
  return started ? ok(action, message) : failed(action, message);
}

// `spawn component <weapon>:<component>` row whose model text hashes to `param`: the two joaats.
static int pack_component_row(uint32_t param, uint32_t* weapon, uint32_t* component) {
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.spawn_count; ++i) {
    const char* model = g_pack.spawns[i].model;
    const char* colon = strchr(model, ':');
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_COMPONENT || !colon ||
        pack_joaat(model) != param)
      continue;
    char name[GTAV_CUSTOM_PACK_NAME_MAX];
    snprintf(name, sizeof(name), "%.*s", (int)(colon - model), model);
    *weapon = pack_joaat(name);
    *component = pack_joaat(colon + 1);
    return 1;
  }
  return 0;
}

// Custom Packs `spawn component` row (param = joaat of "<weapon>:<component>"), game thread: give
// and equip the weapon unless it is the current one, then toggle the component with the Weapons >
// Attachments natives. The engine attaches a component only when the weapon's AttachPoints list
// it, so has=0 after a give means the weapon refused it (or the component name is unknown).
static uint32_t toggle_pack_component(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_COMPONENT;
  if (!g_n.give_weapon_component_to_ped || !g_n.remove_weapon_component_from_ped ||
      !g_n.has_ped_got_weapon_component || !g_n.give_weapon_to_ped ||
      !g_n.get_current_ped_weapon || !g_n.player_ped_id)
    return unavailable(action, "missing weapon component natives");
  uint32_t weapon = 0, component = 0;
  if (!pack_component_row(param, &weapon, &component))
    return unavailable(action, "not a weapon component of the loaded packs");
  const int ped = native_player_ped_id();
  if (ped <= 0) return failed(action, "no valid player ped (be in active gameplay)");
  uintptr_t info = 0;
  const int found = pack_component_known(component, &info) == 1;
  const uint32_t model =
      found && pack_readable(info + 0x14, 4) ? *(const volatile uint32_t*)(info + 0x14) : 0;
  uint32_t current = 0;
  invoke_return<int>(g_n.get_current_ped_weapon, ped, &current, 1);
  const int given = current != weapon;
  if (given) {
    invoke_void(g_n.give_weapon_to_ped, ped, weapon, 250, 0, 1);
    if (g_n.set_current_ped_weapon) invoke_void(g_n.set_current_ped_weapon, ped, weapon, 1);
  }
  int had = 0;
  const int has = weapon_component_flip(ped, weapon, component, &had);
  current = 0;
  invoke_return<int>(g_n.get_current_ped_weapon, ped, &current, 1);
  char line[192];
  snprintf(line, sizeof(line),
           "GTAVMenu pack component 0x%08x weapon=0x%08x info=%d model=0x%08x known=%d given=%d "
           "had=%d has=%d current=0x%08x",
           (unsigned)component, (unsigned)weapon, found, (unsigned)model,
           model ? pack_model_known(model) : -1, given, had, has, (unsigned)current);
  gtav_worker_klog(line);
  if (has == had)
    return failed(action, had ? "component could not be removed (see klog)"
                              : "weapon refused the component (see klog)");
  return ok(action, has ? "component on" : "component off");
}

// ---- Pack weapons in the Weapon Browser and Weapons > Attachments (roadmap F4.3) ----
//
// A pack weapon's menu facts come from its own WEAPONINFO_FILE text, which stays staged in
// g_pack_data_memory (never freed) once its row is loaded: the label key the wheel shows
// (HumanNameHash) and the components its AttachPoints list (gtav_custom_pack_weapon_meta). Its
// wheel slot is the parsed CWeaponInfo's WheelSlot (info +0x58, the field the wicon gate reads).

static const char kWheelSlotNames[kWheelSlots][10] = {
    "Pistol", "SMG", "Rifle", "Sniper", "Melee", "Shotgun", "Heavy", "Throwable",
};

// Spawn row of pack weapon `weapon` (the joaat of its name), or -1.
static int pack_weapon_spawn(uint32_t weapon) {
  if (!weapon || !__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return -1;
  for (int i = 0; i < g_pack.spawn_count; ++i)
    if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_WEAPON &&
        pack_joaat(g_pack.spawns[i].model) == weapon)
      return i;
  return -1;
}

// Live WheelSlot (0..7) of any loaded weapon, or -1.
static int pack_weapon_wheel_slot(uint32_t weapon) {
  uintptr_t info = 0;
  if (pack_weapon_known(weapon, &info) != 1 || !pack_readable(info + 0x58, 4)) return -1;
  const uint32_t slot = *(const volatile uint32_t*)(info + 0x58);
  return slot < kWheelSlots ? (int)slot : -1;
}

// The meta of spawn row `spawn`'s weapon from the loaded WEAPONINFO rows, into g_pack_wmeta (the
// last weapon scanned). Worker thread only. 1 when found.
static GtavCustomPackWeaponMeta g_pack_wmeta;
static uint32_t g_pack_wmeta_weapon;
static int pack_weapon_meta(int spawn) {
  const uint32_t weapon = pack_joaat(g_pack.spawns[spawn].model);
  if (weapon == g_pack_wmeta_weapon) return 1;
  g_pack_wmeta_weapon = 0;
  for (int r = 0; r < g_pack.data_count; ++r) {
    if (g_pack.data[r].type != GTAV_CUSTOM_PACK_DATA_WEAPONINFO ||
        !(g_pack_data_loaded & (1ull << r)) || !g_pack_data_memory[r])
      continue;
    const char* text = (const char*)g_pack_data_memory[r];
    if (gtav_custom_pack_weapon_meta(text, (size_t)g_pack.data[r].size, g_pack.spawns[spawn].model,
                                     &g_pack_wmeta) == 0) {
      g_pack_wmeta_weapon = weapon;
      return 1;
    }
  }
  return 0;
}

// Menu name of pack weapon spawn row `spawn`: the text of its own pack's label row for the weapon's
// HumanNameHash (the name the wheel shows), or nullptr (the row text stays). Cached per row once
// the weapon's meta is loaded. Worker thread only.
static uint8_t g_pack_weapon_label[GTAV_CUSTOM_PACK_SPAWN_CAP];  // 0 unknown, 1 none, label + 2
static const char* pack_weapon_label(int spawn) {
  if (!g_pack_weapon_label[spawn]) {
    if (!pack_weapon_meta(spawn)) return nullptr;
    g_pack_weapon_label[spawn] = 1;
    const uint32_t key = g_pack_wmeta.human_name[0] ? pack_joaat(g_pack_wmeta.human_name) : 0;
    for (int i = 0; key && i < g_pack.label_count; ++i)
      if (g_pack.label_source[i] == g_pack.spawn_source[spawn] &&
          pack_joaat(g_pack.label_keys[i]) == key && g_pack.label_texts[i][0]) {
        g_pack_weapon_label[spawn] = (uint8_t)(i + 2);
        break;
      }
  }
  return g_pack_weapon_label[spawn] > 1 ? g_pack.label_texts[g_pack_weapon_label[spawn] - 2]
                                        : nullptr;
}

// The components pack weapon `weapon`'s meta lists (joaats, and menu labels `stride` bytes apart:
// the text of the pack's `spawn component` row for it, else the part's kind), at most `max`;
// *defaults gets a bit per default part (<Default value="true" />). Returns the count, or -1 when
// `weapon` is not a loaded pack weapon. Worker thread only.
static int pack_weapon_components(uint32_t weapon, uint32_t* hashes, char* labels, size_t stride,
                                  int max, uint32_t* defaults) {
  const int spawn = pack_weapon_spawn(weapon);
  if (spawn < 0 || !pack_weapon_meta(spawn)) return -1;
  int n = 0;
  for (int c = 0; c < g_pack_wmeta.component_count && c < GTAV_CUSTOM_PACK_WEAPON_COMPONENTS_MAX &&
                  n < max;
       ++c) {
    const char* name = g_pack_wmeta.components[c];
    char key[2 * GTAV_CUSTOM_PACK_NAME_MAX];
    snprintf(key, sizeof(key), "%s:%s", g_pack.spawns[spawn].model, name);
    const uint32_t row_hash = pack_joaat(key);
    const char* row = nullptr;
    for (int i = 0; i < g_pack.spawn_count && !row; ++i)
      if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_COMPONENT &&
          pack_joaat(g_pack.spawns[i].model) == row_hash)
        row = g_pack.spawns[i].text;
    hashes[n] = pack_joaat(name);
    gtav_custom_pack_component_label(name, row, labels + (size_t)n * stride, stride);
    ++n;
  }
  gtav_custom_pack_label_dedupe(labels, stride, n);
  *defaults = n < 32 ? g_pack_wmeta.default_mask & ((1u << n) - 1u) : g_pack_wmeta.default_mask;
  return n;
}

// The weapons the loaded packs add (their joaats), at most `max`. Returns the count.
static int pack_weapon_hashes(uint32_t* out, int max) {
  int n = 0;
  if (!__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.spawn_count && n < max; ++i)
    if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_WEAPON)
      out[n++] = pack_joaat(g_pack.spawns[i].model);
  return n;
}

// 1 when pack weapon spawn row `spawn` has a `tints <weapon> none` row (its model reads no tint
// palette: Weapon Tint skips the setter), else 0 (`palette`, or no row).
static int pack_weapon_tint_none(int spawn) {
  return spawn >= 0 && spawn < g_pack.spawn_count &&
         g_pack.spawn_tints[spawn] == GTAV_CUSTOM_PACK_TINTS_NONE;
}

// Weapon Browser footer of a pack weapon row (param = the weapon's joaat), once it is loaded: its
// wheel slot, the retail icon its wicon row borrows, the components Weapons > Attachments offers
// and, with a `tints` row, "Tints: 8" (its model reads a tint palette: Weapon Tint's 8) or "Tints:
// none". "" for any other row (the menu shows the static help). Worker thread only.
static const char* pack_weapon_footer(uint32_t weapon) {
  static uint32_t cached;
  static char text[128];
  if (weapon && weapon == cached) return text;
  const int spawn = pack_weapon_spawn(weapon);
  const int slot = spawn < 0 ? -1 : pack_weapon_wheel_slot(weapon);
  if (slot < 0) return "";
  const char* icon = nullptr;
  for (int i = 0; i < g_pack.wicon_count && !icon; ++i)
    if (!strcmp(g_pack.wicons[i].weapon, g_pack.spawns[spawn].model)) icon = g_pack.wicons[i].donor;
  const int parts = pack_weapon_meta(spawn) ? g_pack_wmeta.component_count : 0;
  const int tints = g_pack.spawn_tints[spawn];
  snprintf(text, sizeof(text), "Wheel: %s slot%s%.30s; %d component%s in Attachments%s",
           kWheelSlotNames[slot], icon ? ", icon of " : "", icon ? icon : "", parts,
           parts == 1 ? "" : "s",
           tints == GTAV_CUSTOM_PACK_TINTS_PALETTE ? "; Tints: 8"
           : tints == GTAV_CUSTOM_PACK_TINTS_NONE  ? "; Tints: none"
                                                   : "");
  cached = weapon;
  return text;
}
