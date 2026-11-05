// Source fragment of features.cpp, textually included by custom_rpf.inc inside the
// CUSTOM_PACKS branch of the runtime pack lane: the engine lane or the engine-less shell.
//
// Pack text labels merged into TheText.

#if GTAV_MENU_CUSTOM_PACK_ENGINE
// Text labels: every label lookup (GET_LABEL_TEXT, the
// vehicle-name display through the model's gameName) goes through TextGet, which searches a merged
// hash -> string map in TheText under one lock before the GXT2 tables. A label is added the way
// the game's DLC text merger adds one: the string is allocated with the calling thread's game
// allocator (TextClear frees map strings with it), then probed into an empty slot (fmix32,
// linear) under the reader lock, hash written last. Existing labels win; a text reload (language
// or DLC change) clears the map, after which labels must be added again.
static const uintptr_t kTextGet = GTAV_LANE_kTextGet;  // (u32 hash) -> const char* or NULL
static const uint8_t kTextGetBytes[] = GTAV_LANE_kTextGetBytes;
static const uintptr_t kTheText = GTAV_LANE_kTheText;
static const uintptr_t kTextLock = GTAV_LANE_kTextLock;
#if defined(__ORBIS__)
static const uintptr_t kLockMutex = GTAV_LANE_kLockMutex;
static const uintptr_t kUnlockMutex = GTAV_LANE_kUnlockMutex;
static const uint8_t kLockMutexBytes[] = GTAV_LANE_kLockMutexBytes;
static const uint8_t kUnlockMutexBytes[] = GTAV_LANE_kUnlockMutexBytes;
static const uint8_t kGameFreeBytes[] = GTAV_LANE_kGameFreeBytes;
#endif
// Labels of the merged set handled so far: an additive load adds only the rows after them.
static int g_pack_labels_added;

#if !defined(__ORBIS__)
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
  if (g_pack_labels_added >= g_pack.label_count)
    return unavailable(action, "pack labels already added");
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
  const int from = g_pack_labels_added;
  g_pack_labels_added = g_pack.label_count;
  int added = 0, existing = 0, refused = 0;
  const char* first_existing = "";  // a game (or earlier pack) key keeps its text
  for (int i = from; i < g_pack.label_count; ++i) {
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
#else
static const char* pack_text_sorted(uint32_t hash) {
  const uintptr_t entries = *(const volatile uintptr_t*)(kTheText + 0x278);
  uint32_t lo = 0, hi = *(const volatile uint16_t*)(kTheText + 0x280);
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2u;
    const uint32_t key = *(const volatile uint32_t*)(entries + (uintptr_t)mid * 16u);
    if (key < hash)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < *(const volatile uint16_t*)(kTheText + 0x280) &&
                 *(const volatile uint32_t*)(entries + (uintptr_t)lo * 16u) == hash
             ? *(const char* const volatile*)(entries + (uintptr_t)lo * 16u + 8u)
             : nullptr;
}

static uint32_t pack_text_insertion(uint32_t hash) {
  uint32_t lo = 0, hi = *(const volatile uint16_t*)(kTheText + 0x280);
  const uintptr_t entries = *(const volatile uintptr_t*)(kTheText + 0x278);
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2u;
    if (*(const volatile uint32_t*)(entries + (uintptr_t)mid * 16u) < hash)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

static uint32_t add_pack_labels() {
  typedef const char* (*TextGetFn)(uintptr_t, uint32_t);
  typedef void* (*AllocateFn)(size_t, size_t, uint32_t);
  typedef void (*FreeFn)(void*);
  typedef void (*MutexFn)(void*);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS;
  if (!g_pack_parsed) return unavailable(action, "pack descriptor not loaded; run pack-register");
  if (!g_pack.label_count) return unavailable(action, "pack has no labels");
  if (g_pack_labels_added >= g_pack.label_count)
    return unavailable(action, "pack labels already added");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (memcmp((const void*)kTextGet, kTextGetBytes, sizeof(kTextGetBytes)) != 0 ||
      memcmp((const void*)kLockMutex, kLockMutexBytes, sizeof(kLockMutexBytes)) != 0 ||
      memcmp((const void*)kUnlockMutex, kUnlockMutexBytes, sizeof(kUnlockMutexBytes)) != 0 ||
      memcmp((const void*)GTAV_LANE_kGameAllocate, kGameAllocateBytes,
             sizeof(kGameAllocateBytes)) != 0 ||
      memcmp((const void*)GTAV_LANE_kGameFree, kGameFreeBytes, sizeof(kGameFreeBytes)) != 0)
    return unavailable(action, "pack text anchors mismatch");
  const uint32_t initial_count = *(const volatile uint16_t*)(kTheText + 0x280);
  const uint32_t initial_capacity = *(const volatile uint16_t*)(kTheText + 0x282);
  const uintptr_t initial_entries = *(const volatile uintptr_t*)(kTheText + 0x278);
  if (initial_count > initial_capacity || (initial_count && !initial_entries) ||
      !pack_readable(initial_entries, (size_t)initial_capacity * 16u))
    return unavailable(action, "pack text map unreadable");
  const int from = g_pack_labels_added;
  g_pack_labels_added = g_pack.label_count;
  int added = 0, existing = 0, refused = 0;
  const char* first_existing = "";
  for (int i = from; i < g_pack.label_count; ++i) {
    const uint32_t hash = pack_joaat(g_pack.label_keys[i]);
    if (((TextGetFn)kTextGet)(kTheText, hash)) {
      if (!existing++) first_existing = g_pack.label_keys[i];
      continue;
    }
    const size_t length = strlen(g_pack.label_texts[i]) + 1;
    char* text = (char*)((AllocateFn)GTAV_LANE_kGameAllocate)(length, 0x10u, 0);
    if (!text) {
      ++refused;
      continue;
    }
    memcpy(text, g_pack.label_texts[i], length);
    uintptr_t old_entries = 0;
    int inserted = 0;
    ((MutexFn)kLockMutex)((void*)kTextLock);
    if (!pack_text_sorted(hash)) {
      uint32_t count = *(const volatile uint16_t*)(kTheText + 0x280);
      uint32_t capacity = *(const volatile uint16_t*)(kTheText + 0x282);
      uintptr_t entries = *(const volatile uintptr_t*)(kTheText + 0x278);
      if (count == capacity) {
        if (capacity > UINT16_MAX - 16u) {
          count = capacity;
        } else {
          const uint32_t new_capacity = capacity + 16u;
          uintptr_t grown = (uintptr_t)((AllocateFn)GTAV_LANE_kGameAllocate)(new_capacity * 16u,
                                                                            0x10u, 0);
          if (grown) {
            if (count) memcpy((void*)grown, (const void*)entries, count * 16u);
            old_entries = entries;
            entries = grown;
            capacity = new_capacity;
            *(volatile uintptr_t*)(kTheText + 0x278) = entries;
            *(volatile uint16_t*)(kTheText + 0x282) = (uint16_t)capacity;
          }
        }
      }
      if (count < capacity) {
        const uint32_t at = pack_text_insertion(hash);
        memmove((void*)(entries + (uintptr_t)(at + 1u) * 16u),
                (const void*)(entries + (uintptr_t)at * 16u), (count - at) * 16u);
        const uintptr_t entry = entries + (uintptr_t)at * 16u;
        *(char* volatile*)(entry + 8) = text;
        *(volatile uint32_t*)entry = hash;
        *(volatile uint16_t*)(kTheText + 0x280) = (uint16_t)(count + 1u);
        *(volatile uint8_t*)(kTheText + 0x270) = 1;
        inserted = 1;
      }
    }
    ((MutexFn)kUnlockMutex)((void*)kTextLock);
    if (old_entries) ((FreeFn)GTAV_LANE_kGameFree)((void*)old_entries);
    if (!inserted) {
      ((FreeFn)GTAV_LANE_kGameFree)(text);
      if (pack_text_sorted(hash)) {
        ++existing;
      } else {
        ++refused;
      }
      continue;
    }
    const char* check = ((TextGetFn)kTextGet)(kTheText, hash);
    if (check == text)
      ++added;
    else
      ++refused;
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack labels added=%d existing=%d refused=%d", added, existing,
           refused);
  pack_world_note("%s%s%s", message, existing ? " first-existing=" : "", first_existing);
  return refused ? failed(action, message) : ok(action, message);
}
#endif
#endif

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
