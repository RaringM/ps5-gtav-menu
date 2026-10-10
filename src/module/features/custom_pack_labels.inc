// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS && PS5 branch of the runtime pack lane.
//
// Pack text labels merged into TheText.

// Text labels: every label lookup (GET_LABEL_TEXT, the
// vehicle-name display through the model's gameName) goes through TextGet, which searches a merged
// hash -> string map in TheText under one lock before the GXT2 tables. A label is added the way
// the game's DLC text merger adds one: the string is allocated with the calling thread's game
// allocator (TextClear frees map strings with it), then probed into an empty slot (fmix32,
// linear) under the reader lock, hash written last. Existing labels win; a text reload (language
// or DLC change) clears the map, after which labels must be added again.
static const uintptr_t kTextGet = 0x24d8ea0;  // (u32 hash) -> const char* or NULL
static const uint8_t kTextGetBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x89, 0xfb, 0x48, 0x8d, 0x3d, 0xa0, 0xe3, 0x01, 0x03,
    0xe8, 0x7b, 0x75, 0x2b, 0x01, 0xc7, 0x05, 0xa1, 0x7a, 0x5a, 0x03, 0xff, 0xff, 0xff, 0xff, 0x85};
static const uintptr_t kTheText = 0x5a806d0;
static const uintptr_t kTextLock = 0x54f7250;
static int g_pack_labels_added;

static uint32_t pack_fmix32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

static uintptr_t pack_thread_allocator() {
  uintptr_t tcb = 0;
  __asm__ volatile("mov %%fs:0, %0" : "=r"(tcb));
  return tcb && pack_readable(tcb - 0x20, 8) ? *(const volatile uintptr_t*)(tcb - 0x20) : 0;
}

static uint32_t add_pack_labels() {
  typedef const char* (*TextGetFn)(uint32_t);
  typedef void* (*AllocateFn)(uintptr_t, size_t, size_t, uint32_t);
  typedef void (*FreeFn)(uintptr_t, void*);
  typedef void (*MutexFn)(void*);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS;
  if (!g_pack_parsed) return unavailable(action, "pack descriptor not loaded; run pack-register");
  if (!g_pack.label_count) return unavailable(action, "pack has no labels");
  if (g_pack_labels_added) return unavailable(action, "pack labels already added");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (memcmp((const void*)kTextGet, kTextGetBytes, sizeof(kTextGetBytes)) != 0 ||
      memcmp((const void*)kLockMutex, kLockMutexBytes, sizeof(kLockMutexBytes)) != 0 ||
      memcmp((const void*)kUnlockMutex, kUnlockMutexBytes, sizeof(kUnlockMutexBytes)) != 0)
    return unavailable(action, "pack text anchors mismatch");
  const uint32_t capacity = *(const volatile uint32_t*)(kTheText + 0x278);
  const uintptr_t entries = *(const volatile uintptr_t*)(kTheText + 0x270);
  if (!capacity || (capacity & (capacity - 1)) || !entries ||
      !pack_readable(entries, (size_t)capacity * 16u))
    return unavailable(action, "pack text map unreadable");
  const uintptr_t allocator = pack_thread_allocator();
  if (!allocator || !pack_readable(allocator, 8) ||
      !pack_readable(*(const volatile uintptr_t*)allocator, 0x60))
    return unavailable(action, "pack text allocator unavailable on this thread");
  const uintptr_t* alloc_vt = *(const uintptr_t* const*)allocator;
  g_pack_labels_added = 1;
  int added = 0, existing = 0, refused = 0;
  const char* first_existing = "";  // a game (or earlier pack) key keeps its text
  for (int i = 0; i < g_pack.label_count; ++i) {
    const uint32_t hash = pack_joaat(g_pack.label_keys[i]);
    if (((TextGetFn)kTextGet)(hash)) {  // takes the text lock itself
      if (!existing++) first_existing = g_pack.label_keys[i];
      continue;
    }
    const size_t length = strlen(g_pack.label_texts[i]) + 1;
    char* text = (char*)((AllocateFn)alloc_vt[0x48 / 8])(allocator, length, 0x10, 0);
    if (!text) {
      ++refused;
      continue;
    }
    memcpy(text, g_pack.label_texts[i], length);
    int inserted = 0;
    ((MutexFn)kLockMutex)((void*)kTextLock);
    const uint32_t count = *(const volatile uint32_t*)(kTheText + 0x27c);
    if ((uint64_t)(count + 1u) * 100u <= (uint64_t)capacity * 75u) {
      uint32_t slot = pack_fmix32(hash) & (capacity - 1u);
      for (uint32_t probe = 0; probe < capacity; ++probe, slot = (slot + 1u) & (capacity - 1u)) {
        const uintptr_t entry = entries + (uintptr_t)slot * 16u;
        const uint32_t current = *(const volatile uint32_t*)entry;
        if (current == hash) break;  // raced in by the game: theirs wins
        if (current == 0) {
          *(char* volatile*)(entry + 8) = text;
          *(volatile uint32_t*)entry = hash;
          *(volatile uint32_t*)(kTheText + 0x27c) = count + 1u;
          inserted = 1;
          break;
        }
      }
    }
    ((MutexFn)kUnlockMutex)((void*)kTextLock);
    if (!inserted) {
      ((FreeFn)alloc_vt[0x58 / 8])(allocator, text);
      ++refused;
      continue;
    }
    const char* check = ((TextGetFn)kTextGet)(hash);
    if (check == text) ++added;
    else ++refused;
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack labels added=%d existing=%d refused=%d", added, existing,
           refused);
  pack_world_note("%s%s%s", message, existing ? " first-existing=" : "", first_existing);
  return refused ? failed(action, message) : ok(action, message);
}

// Worker: the browser / Custom Packs name of a loaded pack vehicle (SPAWN_VEHICLE row param = the
// model's joaat): "<Make> <text>" from its own pack's make label (gtav_custom_pack_vehicle_name).
// Returns 1 and fills `out` when `model_hash` is a vehicle row of the parsed set, else 0.
extern "C" int gtav_features_pack_vehicle_name(uint32_t model_hash, char* out, int size) {
  if (!out || size < 1 || !__atomic_load_n(&g_pack_parsed, __ATOMIC_ACQUIRE)) return 0;
  for (int i = 0; i < g_pack.spawn_count; ++i) {
    if (g_pack.spawns[i].kind != GTAV_CUSTOM_PACK_SPAWN_VEHICLE ||
        pack_joaat(g_pack.spawns[i].model) != model_hash)
      continue;
    return gtav_custom_pack_vehicle_name(&g_pack, i, out, (size_t)size) >= 0;
  }
  return 0;
}
