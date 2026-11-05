// Source fragment of features.cpp, textually included after custom_device.inc.
//
// CUSTOM_STREAM=1 runtime pack lane (roadmap P2 on the archive route). The active pack is named by
// <pack root>/active; its resources/pack.cfg (src/common/custom_pack_runtime.c) gives one engine
// to GTAV_CUSTOM_PACK_ARCHIVE_MAX engine archives (size + SHA-256 each), the texture-dictionary cards an operator may request and show, and the
// data files the worker may load. The archive is registered at runtime exactly as the game's own
// startup/changeset code does (call site 0x15ba62d..0x15ba735 normalized):
//   idx = AddImageToList(DataFile*, 0,0,0,0,0); name = HashToString(1, rec+0x14);
//   if (rec+0x28 == -1 && rec+0x44) { time = device->GetFileTime(name); LoadImage(name, idx);
//                                     rec+0 = time; }
// but served by the engine memory device (verified on hardware): a local-device file reports
// class 2 (APR) and every streamer read of it is routed to the APR queue, which reads unrelated
// retail data. One pack registration per process; each card and data row is used at most once.
//
// Sections: this file holds the descriptor, archive staging/registration, archive load and texture
// cards; custom_pack_data.inc (data files), custom_pack_world.inc (archetypes and maps),
// custom_pack_labels.inc (text labels), custom_pack_select.inc (installed packs and the active
// selection) and custom_pack_autoload.inc (one-press load and menu rows) follow in the engine lane
// (PS5, and PS4 with custom_pack_ps4_world.inc in place of the world section). Every other build
// gets the refusal stubs at the end.
// GTAV_MENU_CUSTOM_PACK_ENGINE=0 (a target without a custom lane) keeps only the descriptor, the
// selection and the natives-only rows: no lane address, no engine device, custom_pack_shell.inc in
// place of the data and world sections, and every engine step refuses.

#if GTAV_MENU_ENABLE_CUSTOM_PACKS

#include "gtavmenu/aes256.h"
#include "gtavmenu/custom_pack_checks.h"
#include "gtavmenu/custom_pack_runtime.h"

#if GTAV_MENU_CUSTOM_PACK_ENGINE
static const uintptr_t kAddImageToList = GTAV_LANE_kAddImageToList;
static const uintptr_t kLoadImage = GTAV_LANE_kLoadImage;
static const uintptr_t kHashToString = GTAV_LANE_kHashToString;
static const uintptr_t kCloseArchiveToc = GTAV_LANE_kCloseArchiveToc;
static const uintptr_t kRequestObject = GTAV_LANE_kRequestObject;       // (global index, request flags)
static const uintptr_t kStreamingModules = GTAV_LANE_kStreamingModules;    // module* []
static const uintptr_t kRpfModuleIndex = GTAV_LANE_kRpfModuleIndex;      // u32 index of the "rpf" module
static const uintptr_t kArchiveTable = GTAV_LANE_kArchiveTable;        // atArray {ptr, u16 count, u16 capacity}
#ifdef GTAV_LANE_kArchiveHeaderCache
static const uintptr_t kArchiveHeaderCache = GTAV_LANE_kArchiveHeaderCache;  // startup-only TOC cache stream
#endif
static const uintptr_t kCollectionTable = GTAV_LANE_kCollectionTable;     // fiCollection* [7000], slot 0 loose
static const uint32_t kCollectionSlots = GTAV_LANE_kCollectionSlots;
static const uint8_t kAddImageToListBytes[] = GTAV_LANE_kAddImageToListBytes;
static const uint8_t kLoadImageBytes[] = GTAV_LANE_kLoadImageBytes;
static const uint8_t kCloseArchiveTocBytes[] = GTAV_LANE_kCloseArchiveTocBytes;
static const uint8_t kRequestObjectBytes[] = GTAV_LANE_kRequestObjectBytes;
static const uint8_t kHashToStringBytes[] = GTAV_LANE_kHashToStringBytes;

#ifdef GTAV_LANE_kFiMemoryDevice
// The engine memory device (fiDeviceMemory, class 6): OpenBulk returns the parsed address and
// ReadBulk is a plain memcpy. The archive TOC key hashes the text after the second ':', i.e. the
// archive basename, so a memory name keeps the key the host tool encrypted the table for.
static const uintptr_t kFiMemoryDevice = GTAV_LANE_kFiMemoryDevice;
static const uintptr_t kFiMemoryVtable = GTAV_LANE_kFiMemoryVtable;
static const uintptr_t kStreamerQueues = GTAV_LANE_kStreamerQueues;
#endif
static const uint32_t kRpfMagic = GTAV_LANE_kRpfMagic;
static const uint32_t kRpfKeyedTable = GTAV_LANE_kRpfKeyedTable;  // retail AES table tag
#endif

#define GTAV_PACK_PATH_MAX 192
#define GTAV_PS4_CACHE_HOME "/data/gtavmenu"
#define GTAV_PS4_CACHE_CUSTOM GTAV_PS4_CACHE_HOME "/custom"
#define GTAV_PS4_CACHE_PACKS GTAV_PS4_CACHE_CUSTOM "/packs"
#define GTAV_PS4_CACHE_ROOT GTAV_PS4_CACHE_PACKS "/.cache"

// <custom root>/packs (/data/GTAVMenu/custom/packs), then "/<id>" when `id` is given, then `tail`.
// Empty when shared /data is unavailable
// or the path does not fit, so every pack read/write fails closed.
static void pack_root_path(char* out, size_t size, const char* id, const char* tail) {
  if (!gtav_custom_game_root_available()) {
    out[0] = '\0';
    return;
  }
  const int length = id ? snprintf(out, size, "%s/packs/%s%s", gtav_custom_game_root(), id, tail)
                        : snprintf(out, size, "%s/packs%s", gtav_custom_game_root(), tail);
  if (length < 0 || (size_t)length >= size) out[0] = '\0';
}

// Never-freed buffer slack beyond the archive: memory ReadBulk does not bound its memcpy.
#define GTAV_PACK_ARCHIVE_SLACK 0x10000ull

static GtavCustomPack g_pack;
static int g_pack_parsed;
// One descriptor parse buffer, shared by the load (pack_load_descriptor, main thread, once per
// process) and the Custom Packs selection check (custom_pack_select.inc pack_select_clash,
// worker, which merges into g_pack until the load has parsed it): a GtavCustomPack is ~68 KiB and
// the worker image must stay well under its 2 MiB region. Whoever takes it first (compare-exchange
// from free) owns it until it releases it; the other refuses and retries, it never waits. The
// one-press load re-sends a registration the check made busy (g_pack_register_busy).
enum { PACK_SCRATCH_FREE = 0, PACK_SCRATCH_WORKER = 1, PACK_SCRATCH_LOAD = 2 };
static GtavCustomPack g_pack_scratch;
static int g_pack_scratch_owner;
static int g_pack_register_busy;

static int pack_scratch_take(int owner) {
  int expected = PACK_SCRATCH_FREE;
  return __atomic_compare_exchange_n(&g_pack_scratch_owner, &expected, owner, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void pack_scratch_release() {
  __atomic_store_n(&g_pack_scratch_owner, PACK_SCRATCH_FREE, __ATOMIC_RELEASE);
}
// <pack root>/<id>/resources of every active pack, in `active` order (GtavCustomPack sources).
static char g_pack_dirs[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_PACK_PATH_MAX];
#if GTAV_MENU_CUSTOM_PACK_ENGINE
// Per archive (0 = the descriptor's first): its never-freed buffer and memory-device name.
static uint8_t* g_rpf_memory[GTAV_CUSTOM_PACK_ARCHIVE_CAP];
static uint64_t g_rpf_memory_bytes[GTAV_CUSTOM_PACK_ARCHIVE_CAP];
static char g_rpf_memory_name[GTAV_CUSTOM_PACK_ARCHIVE_CAP][GTAV_PACK_PATH_MAX];
static uint32_t g_rpf_file_header[GTAV_CUSTOM_PACK_ARCHIVE_CAP][4];

// DataFile as read by AddImageToList: filename at +0, the remaining fields zero (plain RPF_FILE).
static uint8_t g_rpf_datafile[GTAV_CUSTOM_PACK_ARCHIVE_CAP][0x100];
#endif
static int g_rpf_register_attempted;  // stays 0 in the engine-less lane
// Additive loads (custom_pack_autoload.inc start_pack_append): once a load finished, the merged set
// grows by newly selected packs and every step goes on from where the last load stopped. Where the
// load in progress starts in the merged set (all zero for the first load):
struct PackLoadStart {
  int source, archive, data, label;
};
static PackLoadStart g_pack_load_start;
static int g_rpf_register_started = -1;  // first archive of the last registration that began
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static int g_rpf_registered;  // archives registered so far: [0, g_rpf_registered)
static int g_rpf_requested;   // archives whose streaming was requested so far
#endif
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static int32_t g_rpf_indices[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static_assert(GTAV_CUSTOM_PACK_ARCHIVE_CAP == 32,
              "g_rpf_indices lists -1 (unregistered) per archive");
#endif
static_assert(GTAV_CUSTOM_PACK_ACTIVE_MAX <= 31, "pack-source masks are uint32_t");
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static int32_t g_rpf_index = -1;  // archive 0, set once every archive of the pack is registered
#endif
static int g_rpf_request_attempted;  // any card requested
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static uint32_t g_pack_cards_requested;  // per card row (bit = row)
static_assert(GTAV_CUSTOM_PACK_CARD_CAP <= 32, "the card mask is uint32_t");
#endif
static int g_pack_last_card;  // 1-based card the operator requested last; 0 none
#if GTAV_MENU_CUSTOM_PACK_ENGINE
static int g_rpf_archive_load_attempted;
// The archive (index as `override.archive`) whose staging, override gate or registration refused
// the pack, so the one-press load can name its pack (custom_pack_autoload.inc); -1 none.
static int g_pack_fail_archive = -1;

typedef void* (*PackGetDeviceFn)(const char*, bool);
typedef uint64_t (*PackGetFileSizeFn)(void*, const char*);
typedef uint64_t (*PackOpenFn)(void*, const char*, bool);
typedef int32_t (*PackReadFn)(void*, uint64_t, void*, uint32_t);
typedef void (*PackCloseFn)(void*, uint64_t);

// Synchronous local-device read of a whole file of exactly `size` bytes (or, with size 0, of at
// most `capacity` bytes). Returns the byte count, or -1.
static int64_t pack_read_local(const char* path, void* buffer, uint64_t capacity, uint64_t size) {
  if (!path[0]) return -1;  // pack_root_path did not fit
  void* device = ((PackGetDeviceFn)kFiDeviceGetDevice)(path, true);
  const uintptr_t* vtable = device ? *(const uintptr_t**)device : nullptr;
  if ((uintptr_t)vtable != kFiLocalVtable) return -1;
  const uint64_t file_size = ((PackGetFileSizeFn)vtable[kFiSlotGetFileSize])(device, path);
  if (file_size == 0 || file_size > capacity || (size && file_size != size)) return -1;
  const uint64_t handle = ((PackOpenFn)vtable[kFiSlotOpen])(device, path, true);
  if (handle == UINT64_MAX) return -1;
  uint64_t done = 0;
  while (done < file_size) {
    const uint64_t left = file_size - done;
    const int32_t got = ((PackReadFn)vtable[kFiSlotRead])(
        device, handle, (uint8_t*)buffer + done, left < 0x10000u ? (uint32_t)left : 0x10000u);
    if (got <= 0) break;
    done += (uint64_t)got;
  }
  ((PackCloseFn)vtable[kFiSlotClose])(device, handle);
  return done == file_size ? (int64_t)done : -1;
}

static int pack_sha256_matches(const uint8_t* data, uint64_t size, const uint8_t expected[32]) {
  GtavSha256 sha;
  uint8_t digest[32];
  gtav_sha256_init(&sha);
  gtav_sha256_update(&sha, data, (size_t)size);
  gtav_sha256_final(&sha, digest);
  return memcmp(digest, expected, sizeof(digest)) == 0;
}
#else
// Engine-less lane: a whole file through the platform's /data reader (custom_assets.h
// gtav_custom_read_file; PS4: gtav_ps4_read_file), with the engine reader's contract: exactly
// `size` bytes (size 0: at most `capacity`). Returns the byte count, or -1.
static int64_t pack_read_local(const char* path, void* buffer, uint64_t capacity, uint64_t size) {
  if (!path[0]) return -1;  // pack_root_path did not fit
  const int64_t got = gtav_custom_read_file(path, buffer, capacity);
  if (got <= 0 || (uint64_t)got > capacity || (size && (uint64_t)got != size)) return -1;
  return got;
}
#endif

// Merge the descriptors of `ids` into g_pack, parsing each into the taken g_pack_scratch.
static const char* pack_merge_descriptors(const char ids[][GTAV_CUSTOM_PACK_ID_MAX], int id_count) {
  GtavCustomPack& one = g_pack_scratch;
  memset(&g_pack, 0, sizeof(g_pack));
  for (int i = 0; i < id_count; ++i) {
    pack_root_path(g_pack_dirs[i], sizeof(g_pack_dirs[i]), ids[i], "/resources");
    char path[GTAV_PACK_PATH_MAX];
    snprintf(path, sizeof(path), "%s/pack.cfg", g_pack_dirs[i]);
    static char text[GTAV_CUSTOM_PACK_DESCRIPTOR_MAX + 1];
    const int64_t size = pack_read_local(path, text, GTAV_CUSTOM_PACK_DESCRIPTOR_MAX, 0);
    if (size <= 0 || gtav_custom_pack_parse(text, (size_t)size, &one) != 0 ||
        strcmp(one.pack_id, ids[i]) != 0)
      return "pack descriptor missing or invalid";
    if (gtav_custom_pack_merge(&g_pack, &one) != 0) return "active packs overflow or collide";
  }
  return nullptr;
}

// Read <pack root>/active and merge every listed pack's descriptor into g_pack (once per process).
static const char* pack_load_descriptor() {
  if (g_pack_parsed) return nullptr;
  char active[GTAV_CUSTOM_PACK_ACTIVE_MAX * GTAV_CUSTOM_PACK_ID_MAX + 1] = {};
  char active_path[GTAV_PACK_PATH_MAX];
  pack_root_path(active_path, sizeof(active_path), nullptr, "/active");
  const int64_t active_size = pack_read_local(active_path, active, sizeof(active) - 1, 0);
  char ids[GTAV_CUSTOM_PACK_ACTIVE_MAX][GTAV_CUSTOM_PACK_ID_MAX];
  int id_count = 0;
  if (active_size <= 0 ||
      gtav_custom_pack_parse_active(active, (size_t)active_size, ids, &id_count) != 0)
    return "pack active file missing or invalid";
  if (!pack_scratch_take(PACK_SCRATCH_LOAD)) {
    __atomic_store_n(&g_pack_register_busy, 1, __ATOMIC_RELEASE);
    return "pack descriptors busy (menu check); retry";
  }
  const char* refusal = pack_merge_descriptors(ids, id_count);
  // Parsed before the release: the selection check merges into g_pack while it holds the scratch
  // and nothing is parsed (custom_pack_select.inc pack_select_clash_locked).
  if (!refusal) __atomic_store_n(&g_pack_parsed, 1, __ATOMIC_RELEASE);
  pack_scratch_release();
  if (refusal) return refusal;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack cards=%d data=%d typs=%d maps=%d labels=%d archives=%d packs=%d id=%s",
                     g_pack.card_count, g_pack.data_count, g_pack.typ_count, g_pack.map_count,
                     g_pack.label_count, 1 + g_pack.extra_archive_count, g_pack.source_count,
                     g_pack.pack_id);
  return nullptr;
}

#if GTAV_MENU_CUSTOM_PACK_ENGINE
// Directory holding archive `i` / data row `row` of the merged set.
static const char* pack_archive_dir(int i) {
  return g_pack_dirs[i == 0 ? 0 : g_pack.extra_archive_source[i - 1]];
}

static const char* pack_data_dir(uint32_t row) { return g_pack_dirs[g_pack.data_source[row]]; }

#if defined(__PROSPERO__)
// Never-freed, zeroed direct memory of the HDD streamer pool's own type and protection
// (seen on hardware: type 12, prot 0xf3). The worker's libc mmap faulted inside GTA's process,
// and the cave cannot hold archives or data files. Returns nullptr on success.
static const char* pack_map_direct(uint64_t bytes, uint8_t** out) {
  // Largest free contiguous direct-memory block (read-only query): a pack buffer may take at most
  // half of it, leaving the game its own headroom.
  off_t free_phys = 0;
  size_t free_bytes = 0;
  const int free_rc = sceKernelAvailableDirectMemorySize(
      0, (off_t)sceKernelGetDirectMemorySize(), 0x10000, &free_phys, &free_bytes);
  if (free_rc != 0 || bytes > free_bytes / 2u) {
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                       "pack dmem free rc=0x%x largest=0x%llx need=0x%llx", (unsigned)free_rc,
                       (unsigned long long)free_bytes, (unsigned long long)bytes);
    return "pack dmem headroom gate refused";
  }
  GtavLooseVqInfo pool_info = {};
  if (!stream_pool_query(&pool_info) || (pool_info.protection & 3) != 3)
    return "pack pool memory query refused";
  off_t phys = 0;
  void* mapped = nullptr;
  const int alloc_rc =
      sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), (size_t)bytes,
                                    0x10000, pool_info.memory_type, &phys);
  const int map_rc = alloc_rc == 0 ? sceKernelMapDirectMemory(&mapped, (size_t)bytes,
                                                              pool_info.protection, 0, phys, 0x10000)
                                   : -1;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack buffer bytes=0x%llx largest=0x%llx alloc=0x%x map=0x%x addr=%p",
                     (unsigned long long)bytes, (unsigned long long)free_bytes, (unsigned)alloc_rc,
                     (unsigned)map_rc, mapped);
  if (map_rc != 0 || !mapped) {
    if (alloc_rc == 0) sceKernelReleaseDirectMemory(phys, (size_t)bytes);
    return "pack buffer map failed";
  }
  memset(mapped, 0, (size_t)bytes);
  *out = (uint8_t*)mapped;
  return nullptr;
}
#endif

static int pack_archive_count() { return g_pack_parsed ? 1 + g_pack.extra_archive_count : 0; }

static void pack_archive_ref(int i, const char** name, uint64_t* size, const uint8_t** sha256) {
  if (i == 0) {
    *name = g_pack.archive;
    *size = g_pack.archive_size;
    *sha256 = g_pack.archive_sha256;
  } else {
    *name = g_pack.extra_archives[i - 1].name;
    *size = g_pack.extra_archives[i - 1].size;
    *sha256 = g_pack.extra_archives[i - 1].sha256;
  }
}

// Plain-TOC archives: a pack built with
// `build_runtime_pack.py --plain-toc` carries the OPEN tag and a plaintext TOC, so the host needs no
// game binary. The worker keys the TOC in its staging buffer with the game's own key table (pinned
// by SHA-256; no key ships with a pack), exactly as the host would have, so LoadImage sees the usual
// keyed archive. Every block is decrypted back with a separate context before the tag flips.
static const uintptr_t kRpfTocKeyTable = GTAV_LANE_kRpfTocKeyTable;  // 101 x 32-byte AES-256 keys
static const uint8_t kRpfTocKeyTableSha256[] = GTAV_LANE_kRpfTocKeyTableSha256;

// The game's TOC key for archive `name` of `size` bytes (gtav_rpf_toc_index) from its pinned key
// table. Returns nullptr (key and *index filled) or the refusal.
static const char* pack_toc_key(const char* name, uint64_t size,
                                uint8_t key[GTAV_RPF_TOC_KEY_BYTES], int32_t* index) {
  *index = gtav_rpf_toc_index(name, size);
  if (*index < 0 || (uint32_t)*index >= GTAV_RPF_TOC_KEY_COUNT)
    return "pack plain toc index refused";
  if (!pack_sha256_matches((const uint8_t*)kRpfTocKeyTable,
                           GTAV_RPF_TOC_KEY_COUNT * GTAV_RPF_TOC_KEY_BYTES, kRpfTocKeyTableSha256))
    return "pack toc key table pin mismatch";
  memcpy(key, (const void*)(kRpfTocKeyTable + (uintptr_t)*index * GTAV_RPF_TOC_KEY_BYTES),
         GTAV_RPF_TOC_KEY_BYTES);
  return nullptr;
}

static const char* pack_key_plain_toc(int i, uint8_t* memory, uint64_t size, const char* name) {
  uint32_t* header = (uint32_t*)memory;
  const uint64_t count = header[1], names = header[2];
  const uint64_t bytes = count * 16u + names;
  if (count < 2u || (names >> 28) != 0u || 16u + bytes > size || bytes % 16u != 0u)
    return "pack plain toc layout refused";
  int32_t index = -1;
  uint8_t key[GTAV_RPF_TOC_KEY_BYTES];
  if (const char* refusal = pack_toc_key(name, size, key, &index)) return refusal;
  GtavAes256 encrypt, decrypt;
  const int init = gtav_aes256_init(&encrypt, key) | gtav_aes256_init(&decrypt, key);
  memset(key, 0, sizeof(key));
  int round_trip = init == 0;
  uint8_t* toc = memory + 16;
  for (uint64_t at = 0; round_trip && at < bytes; at += 16u) {
    uint8_t plain[16], back[16];
    memcpy(plain, toc + at, sizeof(plain));
    round_trip = gtav_aes256_encrypt_block(&encrypt, toc + at, toc + at) == 0 &&
                 gtav_aes256_decrypt_block(&decrypt, toc + at, back) == 0 &&
                 memcmp(plain, back, sizeof(plain)) == 0;
  }
  gtav_aes256_wipe(&encrypt);
  gtav_aes256_wipe(&decrypt);
  if (!round_trip) return "pack plain toc keying failed";
  header[3] = GTAV_RPF_TAG_KEYED;
  g_rpf_file_header[i][3] = GTAV_RPF_TAG_KEYED;
  // Every block was proven to decrypt back to the verified plaintext above; no whole-archive
  // re-hash (up to 64 MiB on the game thread) just for a log line.
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack toc a=%d keyed idx=%d bytes=%llu", i,
                     (int)index, (unsigned long long)bytes);
  return nullptr;
}

// Archive `i`'s TOC file entries by asset store, counted from its staged bytes (register_pack's
// exact store gate sums them; custom_pack_select.inc counts the same from the console file).
static GtavCustomPackTocCounts g_pack_toc_counts[GTAV_CUSTOM_PACK_ARCHIVE_CAP];

// Count archive `i`'s members per store from its staged, SHA-verified bytes: a plaintext (OPEN)
// table as it is; a keyed one decrypted in place, counted and encrypted back, its table bytes (the
// only bytes touched) hashed before and after, so LoadImage reads exactly the verified archive.
// nullptr or the refusal.
static const char* pack_count_toc(int i, uint8_t* memory, uint64_t size, const char* name) {
  const uint32_t* header = (const uint32_t*)memory;
  const uint64_t count = header[1], names = header[2];
  const uint64_t bytes = count * 16u + names;
  if (count < 2u || (names >> 28) != 0u || 16u + bytes > size || bytes % 16u != 0u)
    return "pack archive toc layout refused";
  uint8_t* toc = memory + 16;
  int counted = -1;
  if (header[3] == GTAV_RPF_TAG_OPEN) {
    counted =
        gtav_custom_pack_toc_counts(toc, (size_t)bytes, (uint32_t)count, &g_pack_toc_counts[i]);
  } else {
    int32_t index = -1;
    uint8_t key[GTAV_RPF_TOC_KEY_BYTES];
    if (const char* refusal = pack_toc_key(name, size, key, &index)) return refusal;
    uint8_t before[32];
    GtavSha256 sha;
    gtav_sha256_init(&sha);
    gtav_sha256_update(&sha, toc, (size_t)bytes);
    gtav_sha256_final(&sha, before);
    if (gtav_rpf_toc_crypt(toc, (size_t)bytes, key, 0) == 0) {
      counted =
          gtav_custom_pack_toc_counts(toc, (size_t)bytes, (uint32_t)count, &g_pack_toc_counts[i]);
      if (gtav_rpf_toc_crypt(toc, (size_t)bytes, key, 1) != 0) counted = -2;
    } else {
      counted = -2;  // the table may be partly decrypted: never hand it to LoadImage
    }
    memset(key, 0, sizeof(key));
    if (counted == -2 || !pack_sha256_matches(toc, bytes, before))
      return "pack keyed toc count failed";
  }
  if (counted != 0) return "pack archive toc layout refused";
  const GtavCustomPackTocCounts& c = g_pack_toc_counts[i];
#if defined(__ORBIS__)
  {
    // A PS5 pack (.p* resources) counts no PS4 member at all.
    uint32_t members = 0;
    for (uint32_t k = 0; k < GTAV_CUSTOM_PACK_MEMBER_KINDS; ++k) members += c.members[k];
    if (c.other && !members) {
      static char refusal[GTAV_FEATURE_MESSAGE_LEN];
      snprintf(refusal, sizeof(refusal), "%s holds no PS4 resources (.oft/.odr/.otd)", name);
      return refusal;
    }
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack toc a=%d files=%u oft=%u odr=%u otd=%u otyp=%u omap=%u obn=%u omt=%u "
                     "odd=%u opt=%u other=%u",
                     i, (unsigned)c.files, (unsigned)c.members[0], (unsigned)c.members[1],
                     (unsigned)c.members[2], (unsigned)c.members[3], (unsigned)c.members[4],
                     (unsigned)c.members[5], (unsigned)c.members[6], (unsigned)c.members[7],
                     (unsigned)c.members[8], (unsigned)c.other);
#else
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack toc a=%d files=%u pft=%u pdr=%u ptd=%u ptyp=%u pmap=%u pbn=%u pmt=%u "
                     "pdd=%u ppt=%u",
                     i, (unsigned)c.files, (unsigned)c.members[0], (unsigned)c.members[1],
                     (unsigned)c.members[2], (unsigned)c.members[3], (unsigned)c.members[4],
                     (unsigned)c.members[5], (unsigned)c.members[6], (unsigned)c.members[7],
                     (unsigned)c.members[8]);
#endif
  return nullptr;
}

#if defined(__ORBIS__)
#define GTAV_PACK_TOC_BUFFER_BYTES (1024u * 1024u)
static uint8_t g_pack_archive_toc[GTAV_PACK_TOC_BUFFER_BYTES];
static uint8_t g_pack_archive_io[0x10000u];

static int pack_read_exact(FILE* stream, void* data, size_t bytes) {
  return bytes == 0 || fread(data, 1, bytes, stream) == bytes;
}

static int pack_write_exact(FILE* stream, const void* data, size_t bytes) {
  return bytes == 0 || fwrite(data, 1, bytes, stream) == bytes;
}

// Write the already validated/transformed header and TOC, then copy the rest of `input` (positioned
// right after them) in bounded chunks while finishing the archive's SHA-256 over the same bytes:
// what is copied is what was hashed, and every byte is read once. The temporary file is renamed
// only when every byte, both closes and the digest check succeed. Closes `input`.
static const char* pack_write_archive_copy(FILE* input, GtavSha256* sha,
                                           const uint8_t expected[32], const char* final_path,
                                           uint64_t archive_size, size_t prefix_bytes) {
  char temporary[GTAV_PACK_PATH_MAX];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp", final_path) >= (int)sizeof(temporary)) {
    fclose(input);
    return "pack cache path too long";
  }
  FILE* output = fopen(temporary, "w");
  int good = output && pack_write_exact(output, g_pack_archive_toc, prefix_bytes);
  uint64_t done = prefix_bytes;
  while (good && done < archive_size) {
    const size_t want = archive_size - done < sizeof(g_pack_archive_io)
                            ? (size_t)(archive_size - done)
                            : sizeof(g_pack_archive_io);
    const size_t got = fread(g_pack_archive_io, 1, want, input);
    if (!got) break;
    gtav_sha256_update(sha, g_pack_archive_io, got);
    done += got;
    good = pack_write_exact(output, g_pack_archive_io, got);
  }
  const int exact = good && done == archive_size && fgetc(input) == EOF;
  uint8_t digest[32];
  gtav_sha256_final(sha, digest);
  if (fclose(input) != 0) good = 0;
  if (output && fclose(output) != 0) good = 0;
  if (!good || !exact || memcmp(digest, expected, sizeof(digest)) != 0) {
    remove(temporary);
    return good && (!exact || memcmp(digest, expected, sizeof(digest)) != 0)
               ? "pack archive size/hash gate refused"
               : "pack keyed archive copy failed";
  }
  if (rename(temporary, final_path) != 0) {
    remove(temporary);
    return "pack keyed archive rename failed";
  }
  return nullptr;
}

// Where archive `i` is staged and what the game registers: the engine cache of its pack,
// <GTAV_PS4_CACHE_ROOT>/<pack id>/<archive>. 0, or -1 when it does not fit.
static int pack_cache_path(int i, char* dir, size_t dir_size, char* path, size_t path_size) {
  const char* archive = nullptr;
  uint64_t archive_size = 0;
  const uint8_t* archive_sha256 = nullptr;
  pack_archive_ref(i, &archive, &archive_size, &archive_sha256);
  const uint32_t source = i == 0 ? 0u : g_pack.extra_archive_source[i - 1];
  const int dir_length =
      snprintf(dir, dir_size, "%s/%s", GTAV_PS4_CACHE_ROOT, g_pack.source_ids[source]);
  if (dir_length < 0 || (size_t)dir_length >= dir_size) return -1;
  const int path_length = snprintf(path, path_size, "%s/%s", dir, archive);
  return path_length < 0 || (size_t)path_length >= path_size ? -1 : 0;
}

// Stage every archive in a lowercase engine cache: AddImageToList lowercases its path before
// OpenBulk. Only the header and TOC enter worker memory; resource pages stream from the cache file.
static const char* stage_pack_archive(int i) {
  const char* archive = nullptr;
  uint64_t archive_size = 0;
  const uint8_t* archive_sha256 = nullptr;
  pack_archive_ref(i, &archive, &archive_size, &archive_sha256);
  char path[GTAV_PACK_PATH_MAX];
  if (snprintf(path, sizeof(path), "%s/%s", pack_archive_dir(i), archive) >= (int)sizeof(path))
    return "pack archive path too long";
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack stage begin a=%d bytes=%llu", i,
                     (unsigned long long)archive_size);
  // One pass: the header and TOC are hashed as read (before keying rewrites the TOC), the payload
  // as it is copied. Until the digest matches at the end of the copy, what the TOC count saw is
  // only provisional: a mismatch refuses the whole archive.
  GtavSha256 sha;
  gtav_sha256_init(&sha);
  FILE* input = fopen(path, "r");
  if (!input || !pack_read_exact(input, g_pack_archive_toc, 16)) {
    if (input) fclose(input);
    return "pack archive header gate refused";
  }
  gtav_sha256_update(&sha, g_pack_archive_toc, 16);
  const uint32_t* header = (const uint32_t*)g_pack_archive_toc;
  const uint64_t count = header[1], names = header[2];
  const uint64_t bytes = count * 16u + names;
  if (count < 2u || (names >> 28) != 0u || bytes % 16u != 0u || 16u + bytes > archive_size ||
      16u + bytes > sizeof(g_pack_archive_toc) ||
      (header[3] != kRpfKeyedTable && header[3] != GTAV_RPF_TAG_OPEN) ||
      !pack_read_exact(input, g_pack_archive_toc + 16, (size_t)bytes)) {
    fclose(input);
    return "pack archive toc layout refused";
  }
  gtav_sha256_update(&sha, g_pack_archive_toc + 16, (size_t)bytes);
  memcpy(g_rpf_file_header[i], header, sizeof(g_rpf_file_header[i]));
  if (const char* refusal = pack_count_toc(i, g_pack_archive_toc, archive_size, archive)) {
    fclose(input);
    return refusal;
  }

  char cache_dir[GTAV_PACK_PATH_MAX], cache_path[GTAV_PACK_PATH_MAX];
  if (pack_cache_path(i, cache_dir, sizeof(cache_dir), cache_path, sizeof(cache_path)) != 0) {
    fclose(input);
    return "pack cache path too long";
  }
  (void)mkdir(GTAV_PS4_CACHE_HOME, 0777);
  (void)mkdir(GTAV_PS4_CACHE_CUSTOM, 0777);
  (void)mkdir(GTAV_PS4_CACHE_PACKS, 0777);
  (void)mkdir(GTAV_PS4_CACHE_ROOT, 0777);
  (void)mkdir(cache_dir, 0777);
  if (header[3] == GTAV_RPF_TAG_OPEN) {
    if (const char* refusal = pack_key_plain_toc(i, g_pack_archive_toc, archive_size, cache_path)) {
      fclose(input);
      return refusal;
    }
  }
  if (const char* refusal = pack_write_archive_copy(input, &sha, archive_sha256, cache_path,
                                                    archive_size, (size_t)(16 + bytes)))
    return refusal;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack stage copied and hash verified a=%d", i);
  const char* registered_path = cache_path;
  if (snprintf(g_rpf_memory_name[i], GTAV_PACK_PATH_MAX, "%s", registered_path) >=
      (int)GTAV_PACK_PATH_MAX)
    return "pack registered path too long";
  typedef uint64_t (*PackOpenBulkFn)(void*, const char*, uint64_t*);
  typedef void (*PackCloseBulkFn)(void*, uint64_t);
  void* device = ((PackGetDeviceFn)kFiDeviceGetDevice)(registered_path, true);
  const uintptr_t* vtable = device ? *(const uintptr_t**)device : nullptr;
  if ((uintptr_t)vtable != kFiLocalVtable ||
      ((PackGetFileSizeFn)vtable[kFiSlotGetFileSize])(device, registered_path) != archive_size)
    return "pack /data archive route gate refused";
  uint64_t bulk_bias = 0;
  const uint64_t bulk =
      ((PackOpenBulkFn)vtable[kFiSlotOpenBulk])(device, registered_path, &bulk_bias);
  if (bulk == UINT64_MAX) return "pack archive bulk open refused";
  ((PackCloseBulkFn)vtable[kFiSlotCloseBulk])(device, bulk);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack route=%s", g_rpf_memory_name[i]);
  return nullptr;
}
// ASCII case-insensitive equality: the game lowercases archive paths it registers.
static int pack_same_path(const char* a, const char* b) {
  for (;; ++a, ++b) {
    const char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
    const char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;
    if (x != y) return 0;
    if (!x) return 1;
  }
}

static uint32_t pack_joaat(const char* text);
static int pack_model_known(uint32_t hash);
static int pack_readable(uintptr_t address, size_t bytes);
static int pack_data_type_mountable(uint32_t type);

// Content the PS4 lane cannot load (no typ, map or bounds steps; data types without a pinned
// mounter): 1 with the refusal in `out`, before anything is staged or registered. Without this the
// load registers the archives and fails at that step 10 to 60 s later.
static int pack_ps4_unsupported(char* out, size_t size) {
  const char* what = nullptr;
  uint8_t source = 0;
  if (g_pack.typ_count) {
    what = "archetype (typ)";
    source = g_pack.typ_source[0];
  } else if (g_pack.map_count) {
    what = "map";
    source = g_pack.map_source[0];
  } else if (g_pack.bounds_count) {
    what = "collision bounds";
    source = g_pack.bounds_source[0];
  }
  if (what) {
    snprintf(out, size, "%s rows: maps and typs are not supported on PS4; deselect %s", what,
             g_pack.source_ids[source]);
    return 1;
  }
  for (int r = 0; r < g_pack.data_count; ++r) {
    if (pack_data_type_mountable(g_pack.data[r].type)) continue;
    const char* type = gtav_custom_pack_data_type_name(g_pack.data[r].type);
    snprintf(out, size, "%s data has no PS4 data loader; deselect %s", type ? type : "unknown",
             g_pack.source_ids[g_pack.data_source[r]]);
    return 1;
  }
  return 0;
}

// The first archive of pack `source`, or -1 (the toast and the load row name the pack through it).
static int pack_source_archive(uint32_t source) {
  const int archives = pack_archive_count();
  for (int i = 0; i < archives; ++i)
    if ((i == 0 ? 0u : (uint32_t)g_pack.extra_archive_source[i - 1]) == source) return i;
  return -1;
}

// Reads only. 1 with the refusal in `out`, and the refused pack's archive in g_pack_fail_archive,
// when the game already holds part of this load (its archives and packs from g_pack_load_start;
// an earlier load's are in the game by design). Archives first: a cache file of this load is a
// registered archive, so an earlier load in this process (any plugin instance) has the pack and
// only a restart loads it again. Then models: a vehicle or ped model name the game already knows
// (stock, or another pack's), which no restart changes: deselect that pack. 0 when nothing
// overlaps (an additive load of new packs into a process that already has others is fine).
static int pack_already_in_game(char* out, size_t size) {
  typedef const char* (*HashToStringFn)(uint32_t, uint32_t);
  const uintptr_t records = *(const volatile uintptr_t*)kArchiveTable;
  const uint16_t count = *(const volatile uint16_t*)(kArchiveTable + 8);
  if (records && count && pack_readable(records, (size_t)count * 0x68u)) {
    const int archives = pack_archive_count();
    char paths[GTAV_CUSTOM_PACK_ARCHIVE_CAP][GTAV_PACK_PATH_MAX];
    for (int i = 0; i < archives; ++i) {
      char dir[GTAV_PACK_PATH_MAX];
      if (i < g_pack_load_start.archive ||
          pack_cache_path(i, dir, sizeof(dir), paths[i], sizeof(paths[i])) != 0)
        paths[i][0] = 0;
    }
    for (uint32_t r = 0; r < count; ++r) {
      const uint32_t name_hash = *(const volatile uint32_t*)(records + r * 0x68u + 0x14u);
      if (!name_hash) continue;
      const char* name = ((HashToStringFn)kHashToString)(1, name_hash);
      if (!name) continue;
      for (int i = 0; i < archives; ++i) {
        if (!paths[i][0] || !pack_same_path(name, paths[i])) continue;
        const uint32_t source = i == 0 ? 0u : g_pack.extra_archive_source[i - 1];
        // Fits the 80-byte message: "<id> already registered in this game; restart GTA".
        snprintf(out, size, "%.32s already registered in this game; restart GTA",
                 g_pack.source_ids[source]);
        __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
        return 1;
      }
    }
  }
  // A pack with override rows replaces stock members and spawns the stock model it replaces.
  uint32_t replacing = 0;
  for (int r = 0; r < g_pack.override_count; ++r) {
    const uint8_t a = g_pack.overrides[r].archive;
    replacing |= 1u << (a == 0 ? 0u : g_pack.extra_archive_source[a - 1]);
  }
  for (int r = 0; r < g_pack.spawn_count; ++r) {
    const uint32_t kind = g_pack.spawns[r].kind;
    if (kind != GTAV_CUSTOM_PACK_SPAWN_VEHICLE && kind != GTAV_CUSTOM_PACK_SPAWN_PED) continue;
    if (g_pack.spawn_source[r] < g_pack_load_start.source) continue;
    if (replacing & (1u << g_pack.spawn_source[r])) continue;
    if (pack_model_known(pack_joaat(g_pack.spawns[r].model)) != 1) continue;
    const uint32_t source = g_pack.spawn_source[r];
    snprintf(out, size, "%s model name already exists; deselect %s", g_pack.spawns[r].model,
             g_pack.source_ids[source]);
    __atomic_store_n(&g_pack_fail_archive, pack_source_archive(source), __ATOMIC_RELEASE);
    return 1;
  }
  return 0;
}
#else
// Stage archive `i` before anything is registered: exact size and a keyed RPF7 header with at
// least one member, its own direct-memory buffer, SHA-256, and read-only memory-route gates.
// Returns nullptr on success, otherwise the refusal.
static const char* stage_pack_archive(int i) {
  typedef uint64_t (*OpenBulkFn)(void*, const char*, uint64_t*);
  typedef int32_t (*ReadBulkFn)(void*, uint64_t, uint64_t, void*, uint32_t);
  typedef void (*CloseBulkFn)(void*, uint64_t);
  typedef int32_t (*ClassFn)(void*);
  typedef int32_t (*PathClassFn)(void*, const char*);
  const char* archive = nullptr;
  uint64_t archive_size = 0;
  const uint8_t* archive_sha256 = nullptr;
  pack_archive_ref(i, &archive, &archive_size, &archive_sha256);
  char path[GTAV_PACK_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", pack_archive_dir(i), archive);
  uint32_t* file_header = g_rpf_file_header[i];
  {
    void* device = ((PackGetDeviceFn)kFiDeviceGetDevice)(path, true);
    const uintptr_t* vtable = device ? *(const uintptr_t**)device : nullptr;
    if ((uintptr_t)vtable != kFiLocalVtable ||
        ((PackGetFileSizeFn)vtable[kFiSlotGetFileSize])(device, path) != archive_size)
      return "pack archive size gate refused";
    const uint64_t handle = ((PackOpenFn)vtable[kFiSlotOpen])(device, path, true);
    if (handle == UINT64_MAX) return "pack archive open refused";
    memset(file_header, 0, sizeof(g_rpf_file_header[i]));
    const int32_t got =
        ((PackReadFn)vtable[kFiSlotRead])(device, handle, file_header, sizeof(g_rpf_file_header[i]));
    ((PackCloseFn)vtable[kFiSlotClose])(device, handle);
    if (got != (int32_t)sizeof(g_rpf_file_header[i]) || file_header[0] != kRpfMagic ||
        file_header[1] < 2u ||
        (file_header[3] != kRpfKeyedTable && file_header[3] != GTAV_RPF_TAG_OPEN))
      return "pack archive header gate refused";
  }

#if defined(__PROSPERO__)
  if (!g_rpf_memory[i]) {
    const uint64_t bytes = (archive_size + GTAV_PACK_ARCHIVE_SLACK + 0xffffull) & ~0xffffull;
    if (const char* refusal = pack_map_direct(bytes, &g_rpf_memory[i])) return refusal;
    g_rpf_memory_bytes[i] = bytes;
  }
#else
  return "pack buffer needs PS5 direct memory";
#endif
  uint8_t* memory = g_rpf_memory[i];
  if (g_rpf_memory_bytes[i] < archive_size + GTAV_PACK_ARCHIVE_SLACK) return "pack buffer too small";
  memset(memory, 0, (size_t)g_rpf_memory_bytes[i]);
  if (pack_read_local(path, memory, archive_size, archive_size) != (int64_t)archive_size ||
      !pack_sha256_matches(memory, archive_size, archive_sha256))
    return "pack buffer hash gate refused";
  char* memory_name = g_rpf_memory_name[i];
  snprintf(memory_name, GTAV_PACK_PATH_MAX, "memory:$0x%llx,%llu,0:%s",
           (unsigned long long)(uintptr_t)memory, (unsigned long long)archive_size, archive);
  if (const char* refusal = pack_count_toc(i, memory, archive_size, memory_name)) return refusal;
  if (file_header[3] == GTAV_RPF_TAG_OPEN) {
    if (const char* refusal = pack_key_plain_toc(i, memory, archive_size, memory_name))
      return refusal;
  }

  // Read-only gates on the memory route before any registration.
  void* device = ((PackGetDeviceFn)kFiDeviceGetDevice)(memory_name, true);
  if ((uintptr_t)device != kFiMemoryDevice || *(const uintptr_t*)device != kFiMemoryVtable)
    return "pack memory device gate refused";
  const uintptr_t* vtable = *(const uintptr_t**)device;
  const int32_t device_class = ((ClassFn)vtable[0x120 / 8])(device);
  const int32_t path_class = ((PathClassFn)vtable[0x118 / 8])(device, memory_name);
  uint64_t bias = UINT64_MAX;
  const uint64_t handle = ((OpenBulkFn)vtable[kFiSlotOpenBulk])(device, memory_name, &bias);
  const uint64_t size = ((PackGetFileSizeFn)vtable[kFiSlotGetFileSize])(device, memory_name);
  uint32_t header[4] = {};
  const int32_t got =
      handle == (uint64_t)(uintptr_t)memory
          ? ((ReadBulkFn)vtable[kFiSlotReadBulk])(device, handle, 0, header, sizeof(header))
          : -1;
  if (handle != UINT64_MAX) ((CloseBulkFn)vtable[kFiSlotCloseBulk])(device, handle);
  const uintptr_t streamer = *(const volatile uintptr_t*)kStreamerQueues;
  const uintptr_t queue = streamer ? *(const volatile uintptr_t*)(streamer + 0xc48) : 0;
  const int queue_ok = streamer && (*(const volatile uint32_t*)streamer & 0x40u) && queue &&
                       *(const volatile uintptr_t*)queue == kFiMemoryDevice &&
                       *(const volatile uint32_t*)(queue + 8) == 0x476e7263u;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack memory a=%d class=%d/%d handle_ok=%d bias=%llu size=%llu read=%d queue=%d",
                     i, (int)device_class, (int)path_class, handle == (uint64_t)(uintptr_t)memory,
                     (unsigned long long)bias, (unsigned long long)size, (int)got, queue_ok);
  if (device_class != 6 || path_class != 6 || handle != (uint64_t)(uintptr_t)memory || bias != 0 ||
      size != archive_size || got != (int32_t)sizeof(header) ||
      memcmp(header, file_header, sizeof(header)) != 0 || !queue_ok)
    return "pack memory route gate refused";
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack route=%s", memory_name);
  return nullptr;
}
#endif

// Stock overrides: an `overlay` archive is added with
// AddImageToList a4 = 1 (archive record +0x48 bit 0x40), so LoadImage's RegisterFile re-points
// each stock member it carries at our archive and records {global -> old handle} in the overlay
// map. Every row is gated read-only before anything is registered: a stock name owned by a valid
// archive, neither requested nor resident (a resident object keeps the old data until evicted),
// and not already overlaid (a memory: archive may not re-override; patch DLCs overlay some cars).
static const uintptr_t kValidArchiveBits = GTAV_LANE_kValidArchiveBits;  // u32* bitset of valid archive indices
static const uintptr_t kValidArchiveBound = GTAV_LANE_kValidArchiveBound;  // u16
static const uintptr_t kOverlayBuckets = GTAV_LANE_kOverlayBuckets;    // node* [u16 @+8]; node {global, old, next}
static const uintptr_t kOverlayEntries = GTAV_LANE_kOverlayEntries;    // u16
static const struct {
  const char* extension;
  uintptr_t store;
  uintptr_t vtable;
  uintptr_t find_slot;  // vt+0x48 (store, stem) -> local or -1, never creates a slot
} kOverrideStores[] = {
#if defined(__ORBIS__)
    // 1.57: the PS4 forms; the four FindSlots are one probe (pack-engine RE section 13).
    {"otd", GTAV_LANE_kTextureStore, GTAV_LANE_kTextureStoreVtable, GTAV_LANE_kTxdFindSlot},
    {"oft", GTAV_LANE_kFragmentStore, GTAV_LANE_kFragmentStoreVtable, GTAV_LANE_kFragmentFindSlot},
    {"odr", GTAV_LANE_kDrawableStore, GTAV_LANE_kDrawableStoreVtable, GTAV_LANE_kDrawableFindSlot},
    {"odd", GTAV_LANE_kDwdStore, GTAV_LANE_kDwdStoreVtable, GTAV_LANE_kDwdFindSlot},
#else
    {"ptd", GTAV_LANE_kTextureStore, GTAV_LANE_kTextureStoreVtable, GTAV_LANE_kTxdFindSlot},
    {"pft", GTAV_LANE_kFragmentStore, GTAV_LANE_kFragmentStoreVtable, GTAV_LANE_kFragmentFindSlot},
    {"pdr", GTAV_LANE_kDrawableStore, GTAV_LANE_kDrawableStoreVtable, GTAV_LANE_kDrawableFindSlot},
    // DwdStore (drawable dictionaries; streamed ped components are filed by folder, so the slot
    // name is e.g. "player_one/uppr_014_u"): FindSlot equals the DrawableStore's but for the hash
    // rip displacement, and vt+0xd0 (refs) is the same code as the other three stores'.
    {"pdd", GTAV_LANE_kDwdStore, GTAV_LANE_kDwdStoreVtable, GTAV_LANE_kDwdFindSlot},
#endif
};
// Asset store slot fields: 1.57 has each 8 bytes before the PS5 builds' (RE section 13).
#if defined(__ORBIS__)
enum : uintptr_t {
  kStoreSlots = 0x38,
  kStoreSlotFlags = 0x40,
  kStoreSlotCount = 0x48,
  kStoreSlotStride = 0x4c
};
#else
enum : uintptr_t {
  kStoreSlots = 0x40,
  kStoreSlotFlags = 0x48,
  kStoreSlotCount = 0x50,
  kStoreSlotStride = 0x54
};
#endif
static uint32_t g_override_globals[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
// v1.1 (seen on hardware: patch DLCs already overlay many base cars, e.g. banshee.ptd): a row whose
// name is already overlaid by a `dlc...`/`update...` archive is accepted; right before our
// LoadImage the game's own remover (0x3558c60 live) drops that overlay node on the main thread, so
// RegisterFile takes the plain override path and records {global -> DLC handle} -- the state the
// game produces itself when a dlc/update archive re-overrides.
#if defined(__ORBIS__)
// 1.57 has no standalone remover: RegisterFile unlinks an overlay node inline (0x230d6cd) and frees
// it with the thread allocator's free (0x4000a0, byte-identical to kGameFree), as
// pack_overlay_remove does.
static const uint8_t kOverlayFreeBytes[] = GTAV_LANE_kGameFreeBytes;
#else
static const uintptr_t kOverlayRemove = GTAV_LANE_kOverlayRemove;  // (global): unlink + free the node, count--
static const uint8_t kOverlayRemoveBytes[] = GTAV_LANE_kOverlayRemoveBytes;
#endif
static uint8_t g_override_remove[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
// What a revert needs (revert_pack_overrides), recorded per row: the kOverrideStores index, the DLC
// node's old handle the remover dropped (v1.1 rows), and, after LoadImage, our handle and the old
// handle of the node RegisterFile inserted.
static uint8_t g_override_store[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
static uint32_t g_override_dlc_old[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
static uint32_t g_override_ours[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
static uint32_t g_override_prev[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
static uint8_t g_override_took[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
__attribute__((unused)) static int g_overrides_reverted;  // set once (game thread), menu reads it
// "Revert overrides" waits for a model still in use (F5.2; on hardware a removed weapon's model
// was evicted ~2 min after the player left it): the game-thread revert arms the wait and records
// each ask's answer; the worker (custom_pack_autoload.inc pack_revert_tick) asks again every 2 s
// for at most 3 min (gtav_custom_pack_gate_wait, the carcols gate's rule) and toasts the outcome.
enum {
  PACK_REVERT_ASKED = 0,  // an ask is queued, not answered yet
  PACK_REVERT_IN_USE,     // a row is loaded or referenced: keep waiting
  PACK_REVERT_BUSY,       // the game thread was not in a stable phase: keep waiting
  PACK_REVERT_REFUSED,    // any other refusal or a failed revert: stop
  PACK_REVERT_DONE,       // reverted
};
__attribute__((unused)) static uint32_t g_revert_answer;
__attribute__((unused)) static uint64_t g_revert_wait_ms;   // when the wait started; 0: none
__attribute__((unused)) static uint64_t g_revert_asked_ms;  // the last ask
__attribute__((unused)) static char g_revert_member[64];    // the row the wait is on

// Each override step also goes to the klog: the status ring overflows during a pack load, and the
// gate line (owner path) is longer than a status message.
static void pack_override_note(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void pack_override_note(const char* format, ...) {
  char line[224];
  const int prefix = snprintf(line, sizeof(line), "GTAVMenu ");
  va_list args;
  va_start(args, format);
  vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
  va_end(args);
  gtav_status_event(GTAV_MENU_EVENT_NATIVE_SHELL, line + prefix);
  gtav_worker_klog(line);
}

// The overlay node {u32 global, u32 old handle, node* next} of `global`, or 0.
static uintptr_t pack_overlay_node(uint32_t global) {
  const uintptr_t buckets = *(const volatile uintptr_t*)kOverlayBuckets;
  const uint32_t bucket_count = *(const volatile uint16_t*)(kOverlayBuckets + 8);
  if (!buckets || !bucket_count) return 0;
  for (uint32_t b = 0; b < bucket_count; ++b) {
    uintptr_t node = ((const volatile uintptr_t*)buckets)[b];
    for (int depth = 0; node && depth < 4096; ++depth) {
      if (*(const volatile uint32_t*)node == global) return node;
      node = *(const volatile uintptr_t*)(node + 8);
    }
  }
  return 0;
}

static int pack_overlay_has(uint32_t global, uint32_t* old_handle) {
  const uintptr_t node = pack_overlay_node(global);
  if (node && old_handle) *old_handle = *(const volatile uint32_t*)(node + 4);
  return node != 0;
}

#if defined(__ORBIS__)
// Drop the overlay node of `global` as RegisterFile does inline (0x230d6cd): unlink it from its
// chain, free it with the thread allocator, count - 1. The entry's handle is left as it is. Every
// bucket is searched, as pack_overlay_node does, so the map's hash is not assumed. Game thread.
static void pack_overlay_remove(uint32_t global) {
  typedef void (*FreeFn)(void*);
  const uintptr_t buckets = *(const volatile uintptr_t*)kOverlayBuckets;
  const uint32_t bucket_count = *(const volatile uint16_t*)(kOverlayBuckets + 8);
  if (!buckets || !bucket_count) return;
  for (uint32_t b = 0; b < bucket_count; ++b) {
    volatile uintptr_t* link = (volatile uintptr_t*)(buckets + 8u * b);
    for (int depth = 0; *link && depth < 4096; ++depth) {
      const uintptr_t node = *link;
      if (*(const volatile uint32_t*)node == global) {
        *link = *(const volatile uintptr_t*)(node + 8);
        ((FreeFn)GTAV_LANE_kGameFree)((void*)node);
        volatile uint16_t* entries = (volatile uint16_t*)kOverlayEntries;
        *entries = (uint16_t)(*entries - 1u);
        return;
      }
      link = (volatile uintptr_t*)(node + 8);
    }
  }
}
#endif

// Slot refs of `local` in override store `s`, read as the txd/frag/drawable vt+0xd0 do
// (0x3116830/0x313b4e0/0x313a5b0 ELF): slot flag byte >= 0, then
// u32[[store+0x40] + local * i32[store+0x54] + 8]. -1 = no slot data (no object, no refs).
static int32_t pack_override_refs(size_t s, int32_t local) {
  const uintptr_t store = kOverrideStores[s].store;
  const uintptr_t slot_flags = *(const volatile uintptr_t*)(store + kStoreSlotFlags);
  const uintptr_t slots = *(const volatile uintptr_t*)(store + kStoreSlots);
  if (!slot_flags || !slots || *(const volatile int8_t*)(slot_flags + (uint32_t)local) < 0)
    return -1;
  const intptr_t stride = *(const volatile int32_t*)(store + kStoreSlotStride);
  return *(const volatile int32_t*)(slots + (uintptr_t)((intptr_t)local * stride) + 8u);
}

static int pack_archive_index_valid(uint32_t coll) {
  const uintptr_t bits = *(const volatile uintptr_t*)kValidArchiveBits;
  return bits && coll < *(const volatile uint16_t*)kValidArchiveBound &&
         coll < *(const volatile uint16_t*)(kArchiveTable + 8) &&
         ((*(const volatile uint32_t*)(bits + (coll >> 5) * 4u) >> (coll & 31u)) & 1u);
}

// Gate override row `row` (read-only) and remember its global index. Returns nullptr when the row
// may be overridden, otherwise the refusal.
static const char* pack_override_gate(int row) {
  typedef int32_t (*FindSlotFn)(uintptr_t, const char*);
  typedef const char* (*HashToStringFn)(uint32_t, uint32_t);
  const GtavCustomPackOverride* o = &g_pack.overrides[row];
  char stem[GTAV_CUSTOM_PACK_NAME_MAX];
  snprintf(stem, sizeof(stem), "%s", o->member);
  char* dot = strrchr(stem, '.');
  if (!dot) return "pack override member has no extension";
  *dot = '\0';
  size_t s = 0;
  while (s < sizeof(kOverrideStores) / sizeof(kOverrideStores[0]) &&
         strcmp(kOverrideStores[s].extension, dot + 1) != 0)
    ++s;
  if (s == sizeof(kOverrideStores) / sizeof(kOverrideStores[0])) return "pack override type has no store";
  const uintptr_t store = kOverrideStores[s].store;
  if (*(const volatile uintptr_t*)store != kOverrideStores[s].vtable ||
      ((const volatile uintptr_t*)kOverrideStores[s].vtable)[0x48 / 8] != kOverrideStores[s].find_slot)
    return "pack override store anchors mismatch";
  const int32_t local = ((FindSlotFn)kOverrideStores[s].find_slot)(store, stem);
  if (local < 0 || (uint32_t)local >= *(const volatile uint32_t*)(store + kStoreSlotCount))
    return "pack override names no stock member";
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  if (!infos) return "streaming info unavailable";
  const uint32_t global = *(const volatile uint32_t*)(store + 8) + (uint32_t)local;
  const uint32_t handle = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u);
  const uint32_t flags = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u + 4u);
  const uint32_t coll = handle >> 16;
  uint32_t old_handle = 0;
  const int overlaid = pack_overlay_has(global, &old_handle);
  const char* owner = "-";
  if (handle && (handle >> 30) != 2u && pack_archive_index_valid(coll)) {
    const uintptr_t record = *(const volatile uintptr_t*)kArchiveTable + (uintptr_t)coll * 0x68u;
    const char* path = ((HashToStringFn)kHashToString)(1, *(const volatile uint32_t*)(record + 0x10));
    if (path) owner = path;
  }
  const int32_t refs = pack_override_refs(s, local);
  pack_override_note("pack override a=%d %s store=%s local=%d g=%u h=0x%x fl=0x%x refs=%d ovl=%d "
                     "old=0x%x owner=%.120s",
                     (int)o->archive, o->member, kOverrideStores[s].extension, (int)local,
                     (unsigned)global, (unsigned)handle, (unsigned)flags, (int)refs, overlaid,
                     (unsigned)old_handle, owner);
  if (!handle || (handle >> 30) == 2u || !pack_archive_index_valid(coll))
    return "pack override owner is not an archive";
  if (overlaid) {
    // Only the game's own re-override case: the current owner is a dlc/update archive.
    if (strncmp(owner, "dlc", 3) != 0 && strncmp(owner, "update", 6) != 0)
      return "pack override target overlaid by a non-DLC archive";
#if defined(__ORBIS__)
    if (memcmp((const void*)GTAV_LANE_kGameFree, kOverlayFreeBytes, sizeof(kOverlayFreeBytes)) != 0)
      return "pack override remover anchors mismatch";
#else
    if (memcmp((const void*)kOverlayRemove, kOverlayRemoveBytes, sizeof(kOverlayRemoveBytes)) != 0)
      return "pack override remover anchors mismatch";
#endif
  }
  // A model in use (spawned, held, in view) or a texture dictionary it holds is resident.
  if (flags & 3u) return "pack override target is loaded or requested (in use)";
  g_override_globals[row] = global;
  g_override_remove[row] = (uint8_t)(overlaid != 0);
  g_override_store[row] = (uint8_t)s;
  g_override_dlc_old[row] = overlaid ? old_handle : 0u;
  return nullptr;
}

// Rows of the merged set that archive `i` overrides.
static int pack_override_rows(int i) {
  int rows = 0;
  for (int r = 0; r < g_pack.override_count; ++r) rows += g_pack.overrides[r].archive == i;
  return rows;
}

// Distinct folders ("player_one/" of "player_one/uppr_014_u.pdd") of archive `i`'s rows: each is one
// directory entry of that archive's TOC.
static int pack_override_folders(int i) {
  int folders = 0;
  for (int r = 0; r < g_pack.override_count; ++r) {
    const char* member = g_pack.overrides[r].member;
    const char* slash = strchr(member, '/');
    if (g_pack.overrides[r].archive != i || !slash) continue;
    const size_t length = (size_t)(slash - member) + 1u;
    int seen = 0;
    for (int q = 0; q < r && !seen; ++q)
      seen = g_pack.overrides[q].archive == i &&
             strncmp(g_pack.overrides[q].member, member, length) == 0;
    folders += !seen;
  }
  return folders;
}

// After LoadImage of overlay archive `i`: each of its rows now resolves to our archive and has an
// overlay node. RegisterFile refuses an override silently, so a row that did not take fails.
static const char* pack_override_verify(int i, int32_t index, uint32_t entries_before) {
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  int taken = 0;
  for (int r = 0; r < g_pack.override_count; ++r) {
    if (g_pack.overrides[r].archive != i) continue;
    const uint32_t handle = *(const volatile uint32_t*)(infos + (uintptr_t)g_override_globals[r] * 8u);
    uint32_t old_handle = 0;
    const int took = (handle >> 16) == (uint32_t)index && !(handle & 0x80000000u) &&
                     pack_overlay_has(g_override_globals[r], &old_handle);
    const uint32_t flags =
        *(const volatile uint32_t*)(infos + (uintptr_t)g_override_globals[r] * 8u + 4u);
    pack_override_note("pack override %s h=0x%x fl=0x%x old=0x%x took=%d",
                       g_pack.overrides[r].member, (unsigned)handle, (unsigned)flags,
                       (unsigned)old_handle, took);
    g_override_took[r] = (uint8_t)took;
    g_override_ours[r] = handle;
    g_override_prev[r] = old_handle;
    taken += took;
  }
  const uint32_t entries = *(const volatile uint16_t*)kOverlayEntries;
  const int rows = pack_override_rows(i);
  pack_override_note("pack override a=%d idx=%d took=%d/%d overlay entries %u->%u", i, (int)index,
                     taken, rows, (unsigned)entries_before, (unsigned)entries);
  if (taken != rows || entries != entries_before + (uint32_t)taken)
    return "pack override did not take (see klog)";
  return nullptr;
}

// Register staged archive `i` exactly as the game's changeset code does (file header comment).
// Returns nullptr on success, otherwise the failure.
static const char* add_pack_archive(int i) {
#if defined(__ORBIS__)
  typedef int32_t (*AddImageToListFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t, uint32_t);
#else
  typedef int32_t (*AddImageToListFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#endif
  typedef const char* (*HashToStringFn)(uint32_t, uint32_t);
  typedef uint64_t (*FileTimeFn)(void*, const char*);
  typedef void (*LoadImageFn)(const char*, uint32_t);
  memset(g_rpf_datafile[i], 0, sizeof(g_rpf_datafile[i]));
  snprintf((char*)g_rpf_datafile[i], sizeof(g_rpf_datafile[i]), "%s", g_rpf_memory_name[i]);
  const uint32_t overlay = g_pack.archive_overlay[i] ? 1u : 0u;
#if defined(__ORBIS__)
  const int32_t index =
      ((AddImageToListFn)kAddImageToList)(g_rpf_datafile[i], 1, 0xFF, 0, 0, 0, overlay, 0);
#else
  const int32_t index = ((AddImageToListFn)kAddImageToList)(g_rpf_datafile[i], 0, 0, 0, overlay, 0);
#endif
  gtav_worker_klog(index >= 0 ? "GTAVMenu pack AddImageToList ok"
                              : "GTAVMenu pack AddImageToList failed");
  if (index < 0) return "pack AddImageToList returned -1";

  const uintptr_t table = *(const volatile uintptr_t*)kArchiveTable;
  const uintptr_t record = table + (uintptr_t)(uint32_t)index * 0x68u;
  const uint32_t name_hash = *(const volatile uint32_t*)(record + 0x14);
  const char* name = ((HashToStringFn)kHashToString)(1, name_hash);
  const int64_t toc_state = *(const volatile int64_t*)(record + 0x28);
  const uint8_t loadable = *(const volatile uint8_t*)(record + 0x44);
  // RegisterFile reads the permit at LoadImage time; it must be set (and the pairing bit clear).
  if (overlay) {
    const uint32_t permit = *(const volatile uint16_t*)(record + 0x48);
    pack_override_note("pack overlay a=%d idx=%d record flags=0x%x", i, (int)index,
                       (unsigned)permit);
    if ((permit & 0xc0u) != 0x40u) return "pack overlay permit not set on the archive record";
  }
  // v1.1: drop the DLC overlay node of each row that has one, then count entries from here.
  for (int r = 0; overlay && r < g_pack.override_count; ++r) {
#if defined(__ORBIS__)
    if (g_pack.overrides[r].archive != i || !g_override_remove[r]) continue;
    pack_overlay_remove(g_override_globals[r]);
#else
    typedef void (*RemoveFn)(uint32_t);
    if (g_pack.overrides[r].archive != i || !g_override_remove[r]) continue;
    ((RemoveFn)kOverlayRemove)(g_override_globals[r]);
#endif
    const int still = pack_overlay_has(g_override_globals[r], nullptr);
    pack_override_note("pack override %s dlc node removed=%d", g_pack.overrides[r].member, !still);
    if (still) return "pack override could not remove the DLC overlay node";
  }
  const uint32_t overlay_entries = *(const volatile uint16_t*)kOverlayEntries;
  int loaded = 0;
  if (name && toc_state == -1 && loadable) {
    void* archive_device = *(void* volatile*)(record + 0x18);
    if (!archive_device) {
      archive_device = ((PackGetDeviceFn)kFiDeviceGetDevice)(name, true);
      *(void* volatile*)(record + 0x18) = archive_device;
    }
    if (archive_device) {
      const uintptr_t* archive_vtable = *(const uintptr_t**)archive_device;
      const uint64_t file_time = ((FileTimeFn)archive_vtable[0xc0 / 8])(archive_device, name);
      gtav_worker_klog("GTAVMenu pack LoadImage before");
      ((LoadImageFn)kLoadImage)(name, (uint32_t)index);
      gtav_worker_klog("GTAVMenu pack LoadImage returned");
      *(volatile uint64_t*)record = file_time;
      loaded = 1;
    }
  }
  // Same epilogue as the game (0x15ba715..0x15ba735 normalized): when the archive's own streamable
  // is not resident, release the TOC LoadImage parsed. A later archive load re-parses it from the
  // streamed buffer. Skipping this on hardware made the request read page flags through a stale
  // TOC.
  int toc_closed = 0;
  uint32_t archive_status = UINT32_MAX;
  if (loaded) {
    typedef void (*CloseArchiveTocFn)(uint32_t);
    const uintptr_t modules = *(const volatile uintptr_t*)kStreamingModules;
    const uint32_t module_index = *(const volatile uint32_t*)kRpfModuleIndex;
    const uintptr_t module = modules ? ((const volatile uintptr_t*)modules)[module_index] : 0;
    const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
    if (module && infos) {
      const uint32_t base = *(const volatile uint32_t*)(module + 8);
      archive_status =
          *(const volatile uint32_t*)(infos + ((uintptr_t)base + (uint32_t)index) * 8u + 4u);
      if ((archive_status & 3u) == 0u) {
        ((CloseArchiveTocFn)kCloseArchiveToc)((uint32_t)index);
        toc_closed = 1;
      }
    }
  }
  g_rpf_indices[i] = index;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack registered a=%d idx=%d toc=%lld load=%u/%d st=0x%x closed=%d", i,
                     (int)index, (long long)toc_state, (unsigned)loadable, loaded,
                     (unsigned)archive_status, toc_closed);
  if (!loaded) return "pack LoadImage preconditions not met";
  return overlay ? pack_override_verify(i, index, overlay_entries) : nullptr;
}

// Asset stores the registration adds slots to: LoadImage adds one slot per archive member to the
// member type's store, and AddSlot traps (int 0x41) when the slot pool is full, so a load first
// requires each store it adds to to have its adds plus the store's margin free
// (pack_store_gate_exact; gtav_custom_pack_store_need). `name` is the store in refusals and
// pack-notes; `scope` (custom_pack_checks.h GTAV_CUSTOM_PACK_STORE_*) picks the menu's noun for the
// packs to deselect; gtav_custom_pack_store_reason gives the player's text. Indexed by
// PackStoreIndex.
enum PackStoreIndex {
  PACK_STORE_FRAG = 0,
  PACK_STORE_DRAWABLE,
  PACK_STORE_TXD,
  PACK_STORE_TYP,
  PACK_STORE_MAPDATA,
  PACK_STORE_BOUNDS,
  PACK_STORE_METADATA,
  PACK_STORE_DWD,
  PACK_STORE_PTFX,
  PACK_STORE_COUNT,
};
struct PackStoreGate {
  uintptr_t store;
  uintptr_t vtable;  // the store object's vtable; 0: not pinned
  const char* name;  // in refusals and pack-notes (custom_pack_checks.c kStores)
  int scope;         // GTAV_CUSTOM_PACK_STORE_* for the menu's noun
};
// One gate per GTAV_CUSTOM_PACK_MEMBER_* store, in that order. The need is exact: the members the
// selected archives add (their TOCs counted) and the slots the data rows add by name, plus the
// store's margin (gtav_custom_pack_store_need); a load that adds nothing to a store skips it.
static const PackStoreGate kPackStoreGates[PACK_STORE_COUNT] = {
    // Fragment, drawable, texture-dictionary, archetype-definition (ptyp) and map-data stores.
    {GTAV_LANE_kFragmentStore, GTAV_LANE_kFragmentStoreVtable, "frag", GTAV_CUSTOM_PACK_STORE_ANY},
    {GTAV_LANE_kDrawableStore, GTAV_LANE_kDrawableStoreVtable, "drawable",
     GTAV_CUSTOM_PACK_STORE_ANY},
    {GTAV_LANE_kTextureStore, GTAV_LANE_kTextureStoreVtable, "txd", GTAV_CUSTOM_PACK_STORE_ANY},
    {GTAV_LANE_kTypStore, GTAV_LANE_kTypStoreVtable, "typ", GTAV_CUSTOM_PACK_STORE_ANY},
    {GTAV_LANE_kMapStore, GTAV_LANE_kMapStoreVtable, "mapdata", GTAV_CUSTOM_PACK_STORE_ANY},
    // .pbn members add slots to the static bounds store.
    {GTAV_LANE_kBoundsStore, GTAV_LANE_kBoundsStoreVtable, "bounds", GTAV_CUSTOM_PACK_STORE_ANY},
    // Ped archives add .pmt slots to the MetaDataStore (it runs close to full: 1 free in a fresh
    // 011 process, 20 in a 010 snapshot) and .pdd slots to the DwdStore.
    {GTAV_LANE_kMetaDataStore, GTAV_LANE_kMetaDataStoreVtable, "metadata",
     GTAV_CUSTOM_PACK_STORE_PED},
    {GTAV_LANE_kDwdStore, GTAV_LANE_kDwdStoreVtable, "dwd", GTAV_CUSTOM_PACK_STORE_PED},
    // A `<asset>.ppt` member adds a PtFxAssetStore slot (368/400 used on 011).
    {GTAV_LANE_kPtfxStore, GTAV_LANE_kPtfxStoreVtable, "ptfx", GTAV_CUSTOM_PACK_STORE_PTFX},
};
static_assert(PACK_STORE_COUNT == GTAV_CUSTOM_PACK_MEMBER_KINDS,
              "one store gate per counted member store, in GTAV_CUSTOM_PACK_MEMBER_* order");

// Free slots of `gate`'s pool in *free_slots (0 when full) and its size in *size. The store header
// is eight bytes shorter on Orbis than on Prospero. Returns 0; -1 when the pool is unset; -2 when
// the store's pinned vtable differs. Plain reads of the store object in the eboot data (its pool
// fields change only while archives register); worker callers check pack_readable first.
static int pack_store_headroom(const PackStoreGate& gate, uint32_t* free_slots, uint32_t* size) {
  *free_slots = 0;
  *size = 0;
  if (gate.vtable && *(const volatile uintptr_t*)gate.store != gate.vtable) return -2;
  const uint32_t pool = *(const volatile uint32_t*)(gate.store + PACK_STORE_SIZE_OFFSET);
  const uint32_t used =
      *(const volatile uint32_t*)(gate.store + PACK_STORE_USED_OFFSET) & 0x3fffffffu;
  *size = pool;
  const uint32_t first_free =
      *(const volatile uint32_t*)(gate.store + PACK_STORE_FIRST_FREE_OFFSET);
  *free_slots = first_free == UINT32_MAX || used > pool ? 0u : pool - used;
  return pool ? 0 : -1;
}

// What a load adds to each member store: per kind the slots and the merged-set packs adding them
// (bit s = g_pack.source_ids[s]). `counts[i]` is archive i's TOC count, nullptr when unknown (a
// keyed archive the worker cannot read, or an unreadable one): it then adds the old fixed allowance
// to every store its pack may touch.
//  - Archive members, overlay archives included: an overlay's members should only re-point stock
//    slots, but registration checks only its entry count before LoadImage, so a member that is not
//    one of its override rows would add a slot; counting them over-counts by at most its rows.
//  - Data rows add by name, counted here per row (registration has not read the data files): each
//    vehicle model row its txd, each ped model row its ped dwd and txd and props dwd and txd (2 +
//    2) and its pmt in the MetaDataStore (a .pmt archive member of that name is found, not added:
//    an over-count). The row gates (custom_pack_data.inc pack_data_gate) check the row's full
//    model bound plus the margin when the row loads. Apparel rows add nothing by name: their .pmt
//    is an archive member (counted above), and a row whose .pmt is not registered is refused by
//    pack_apparel_gate before anything parses.
struct PackStoreAdds {
  uint32_t adds[GTAV_CUSTOM_PACK_MEMBER_KINDS];
  uint32_t sources[GTAV_CUSTOM_PACK_MEMBER_KINDS];
};
static const uint32_t kPackStoreUnknownAdds[GTAV_CUSTOM_PACK_MEMBER_KINDS] = {48, 48, 48, 56, 56,
                                                                             56, 7,  48, 6};

static int pack_archive_source(int i) { return i == 0 ? 0 : g_pack.extra_archive_source[i - 1]; }

// Only this load's archives and data rows count (g_pack_load_start): an earlier load's took their
// slots already.
static void pack_store_adds(const GtavCustomPackTocCounts* const* counts, PackStoreAdds* out) {
  memset(out, 0, sizeof(*out));
  uint32_t ped_sources = 0, ptfx_sources = 0;
  for (int i = 0; i < g_pack.data_count && i < (int)GTAV_CUSTOM_PACK_DATA_CAP; ++i)
    if ((g_pack.data[i].type == GTAV_CUSTOM_PACK_DATA_PED_METADATA ||
         g_pack.data[i].type == GTAV_CUSTOM_PACK_DATA_SHOP_PED_APPAREL) &&
        g_pack.data_source[i] < GTAV_CUSTOM_PACK_ACTIVE_MAX)
      ped_sources |= 1u << g_pack.data_source[i];
  for (int i = 0; i < g_pack.spawn_count && i < (int)GTAV_CUSTOM_PACK_SPAWN_CAP; ++i)
    if (g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_PTFX &&
        g_pack.spawn_source[i] < GTAV_CUSTOM_PACK_ACTIVE_MAX)
      ptfx_sources |= 1u << g_pack.spawn_source[i];
  const int archives = 1 + g_pack.extra_archive_count;
  for (int i = g_pack_load_start.archive; i < archives && i < (int)GTAV_CUSTOM_PACK_ARCHIVE_CAP;
       ++i) {
    const int source = pack_archive_source(i);
    const uint32_t bit = source < (int)GTAV_CUSTOM_PACK_ACTIVE_MAX ? 1u << source : 0u;
    for (int k = 0; k < GTAV_CUSTOM_PACK_MEMBER_KINDS; ++k) {
      uint32_t adds = 0;
      if (counts[i]) {
        adds = counts[i]->members[k];
      } else if ((k != GTAV_CUSTOM_PACK_MEMBER_METADATA && k != GTAV_CUSTOM_PACK_MEMBER_DWD &&
                  k != GTAV_CUSTOM_PACK_MEMBER_PTFX) ||
                 (k == GTAV_CUSTOM_PACK_MEMBER_PTFX ? (ptfx_sources & bit) : (ped_sources & bit))) {
        adds = kPackStoreUnknownAdds[k];
      }
      out->adds[k] += adds;
      if (adds) out->sources[k] |= bit;
    }
  }
  for (int i = g_pack_load_start.data; i < g_pack.data_count && i < (int)GTAV_CUSTOM_PACK_DATA_CAP;
       ++i) {
    const uint32_t type = g_pack.data[i].type;
    const uint32_t bit =
        g_pack.data_source[i] < GTAV_CUSTOM_PACK_ACTIVE_MAX ? 1u << g_pack.data_source[i] : 0u;
    if (type == GTAV_CUSTOM_PACK_DATA_VEHICLE_METADATA) {
      out->adds[GTAV_CUSTOM_PACK_MEMBER_TXD] += 1;
      out->sources[GTAV_CUSTOM_PACK_MEMBER_TXD] |= bit;
    } else if (type == GTAV_CUSTOM_PACK_DATA_PED_METADATA) {
      out->adds[GTAV_CUSTOM_PACK_MEMBER_TXD] += 2;
      out->adds[GTAV_CUSTOM_PACK_MEMBER_DWD] += 2;
      out->adds[GTAV_CUSTOM_PACK_MEMBER_METADATA] += 1;
      out->sources[GTAV_CUSTOM_PACK_MEMBER_TXD] |= bit;
      out->sources[GTAV_CUSTOM_PACK_MEMBER_DWD] |= bit;
      out->sources[GTAV_CUSTOM_PACK_MEMBER_METADATA] |= bit;
    }
  }
}

// Exact store gate (game thread, register_pack, after every archive is staged and counted): each
// store the load adds to must be the pinned, initialised one with the slots the load adds plus its
// margin free; a store the load adds nothing to is not required (a pin that drifted on an unused
// store does not block every pack). Logs every store's need/free to pack-notes ("pack store need
// frag=12/284 ...", '-' for an unread unused store); returns the first refusal ("pack <store> store
// headroom gate refused free=<n> need=<m>"), a used store whose vtable differs or whose pool is
// unset, or nullptr. *sources gets the packs adding to the refusing store.
static const char* pack_store_gate_exact(const PackStoreAdds& adds, uint32_t* sources) {
  static char refusal[GTAV_FEATURE_MESSAGE_LEN];
  char line[200];
  int length = snprintf(line, sizeof(line), "pack store need");
  const char* why = nullptr;
  for (int i = 0; i < PACK_STORE_COUNT; ++i) {
    const PackStoreGate& gate = kPackStoreGates[i];
    uint32_t free_slots = 0, size = 0;
    const int read = pack_store_headroom(gate, &free_slots, &size);
    const uint32_t need = gtav_custom_pack_store_need(i, adds.adds[i]);
    if (read != 0) {
      if (need) return read == -2 ? "pack store anchors mismatch" : "pack store pool unreadable";
      if (length >= 0 && (size_t)length < sizeof(line))
        length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=-", gate.name);
      continue;
    }
    if (length >= 0 && (size_t)length < sizeof(line))
      length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=%u/%u", gate.name,
                         (unsigned)need, (unsigned)free_slots);
    if (!why && free_slots < need) {
      gtav_custom_pack_store_refusal(refusal, sizeof(refusal), gate.name, free_slots, need);
      if (sources) *sources = adds.sources[i];
      why = refusal;
    }
  }
  pack_override_note("%s", line);
  return why;
}

// Store census before the descriptor (game thread): every gated store's free/size to pack-notes
// ("pack store free frag=284/42000 ...", "metadata=?" for a store whose pin or pool does not
// read). It refuses nothing: pack_store_gate_exact requires the stores the load adds to.
static void pack_store_census() {
  char line[200];
  int length = snprintf(line, sizeof(line), "pack store free");
  for (int i = 0; i < PACK_STORE_COUNT && length >= 0 && (size_t)length < sizeof(line); ++i) {
    const PackStoreGate& gate = kPackStoreGates[i];
    uint32_t free_slots = 0, size = 0;
    if (pack_store_headroom(gate, &free_slots, &size) != 0)
      length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=?", gate.name);
    else
      length += snprintf(line + length, sizeof(line) - (size_t)length, " %s=%u/%u", gate.name,
                         (unsigned)free_slots, (unsigned)size);
  }
  pack_override_note("%s", line);
}

// The packs adding to the store a refusal names (register_pack's exact gate), for the one-press
// load's toast (custom_pack_autoload.inc pack_al_store_toast). Written before the refusal is.
static uint32_t g_pack_store_refusal_sources;

#if defined(__ORBIS__)
// Adoption after a plugin reload (custom_pack_autoload.inc pack_adopt_start): the packs an earlier
// instance loaded into this game, merged as they were, with the archive record each registered.
static int g_pack_adopt_pending;
static int32_t g_pack_adopt_indices[GTAV_CUSTOM_PACK_ARCHIVE_CAP];
static int g_pack_adopted;  // the game thread took the set over (the worker finishes the load)
static uint32_t adopt_pack_set(uint32_t action);  // after pack_archive_flags_at
#endif

static uint32_t register_pack() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);

  // This load's archives are [first, count): every one for the first load, the appended packs'
  // for an additive one. Each range registers at most once.
  const int first = g_pack_load_start.archive;
  if (g_rpf_index >= 0 && g_rpf_registered >= pack_archive_count())
    return ok(action, "pack already registered");
  if (g_rpf_register_started == first)
    return unavailable(action, "pack registration already attempted");
  __atomic_store_n(&g_pack_fail_archive, -1, __ATOMIC_RELEASE);  // this attempt's refusal only
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (memcmp((const void*)kAddImageToList, kAddImageToListBytes, sizeof(kAddImageToListBytes)) ||
      memcmp((const void*)kLoadImage, kLoadImageBytes, sizeof(kLoadImageBytes)) ||
      memcmp((const void*)kHashToString, kHashToStringBytes, sizeof(kHashToStringBytes)) ||
      memcmp((const void*)kCloseArchiveToc, kCloseArchiveTocBytes, sizeof(kCloseArchiveTocBytes)) ||
      // Every pack file read starts with GetDevice (pack_read_local, stage_pack_archive).
      memcmp((const void*)kFiDeviceGetDevice, kFiDeviceGetDeviceBytes,
             sizeof(kFiDeviceGetDeviceBytes)))
    return unavailable(action, "pack code anchors mismatch");
#ifdef GTAV_LANE_kArchiveHeaderCache
  if (*(const volatile uintptr_t*)kArchiveHeaderCache != 0)
    return unavailable(action, "pack startup header cache still active");
#endif
  // Every gated store's free slots to pack-notes; the stores this load adds to must be the pinned,
  // initialised ones with room, checked once the archives are staged and counted.
  pack_store_census();

  if (const char* refusal = pack_load_descriptor()) return unavailable(action, refusal);
#if defined(__ORBIS__)
  if (g_pack_adopt_pending) return adopt_pack_set(action);
#endif
  const int archives = pack_archive_count();
  const int adding = archives - first;
  // One archive record and one collection slot per archive this load adds.
  const uintptr_t records = *(const volatile uintptr_t*)kArchiveTable;
  const uint16_t count = *(const volatile uint16_t*)(kArchiveTable + 8);
  const uint16_t capacity = *(const volatile uint16_t*)(kArchiveTable + 10);
  uint32_t free_slot = 0, free_slots = 0, last_free = 0;
  for (uint32_t i = 1; i < kCollectionSlots && free_slots < (uint32_t)adding; ++i)
    if (((const volatile uintptr_t*)kCollectionTable)[i] == 0) {
      if (!free_slot) free_slot = i;
      free_slots++;
      last_free = i;
    }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack archive table count=%u capacity=%u free_slot=%u records=%u archives=%d",
                     (unsigned)count, (unsigned)capacity, (unsigned)free_slot, records != 0, archives);
  if (!records || adding <= 0 || free_slots < (uint32_t)adding || last_free >= capacity ||
      (uint32_t)count + (uint32_t)adding > capacity)
    return unavailable(action, "pack archive capacity gate refused");

#if defined(__ORBIS__)
  // The game keeps what an earlier load registered (by any plugin instance) for the life of the
  // process, while this instance starts fresh: refuse before staging rewrites a cache file the game
  // streams from, or a model is registered twice.
  {
    static char held[GTAV_FEATURE_MESSAGE_LEN];
    if (pack_ps4_unsupported(held, sizeof(held)) || pack_already_in_game(held, sizeof(held)))
      return unavailable(action, held);
  }
#endif
  // Every archive is staged and gated before the first registration, so a bad archive refuses the
  // pack without registering any of it.
  for (int i = first; i < archives; ++i)
    if (const char* refusal = stage_pack_archive(i)) {
      __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
      return unavailable(action, refusal);
    }
  // An overlay archive holds exactly its override members (TOC entries: root + one per folder of
  // its rows + one per row).
  for (int i = first; i < archives; ++i) {
    const int rows = pack_override_rows(i);
    const int folders = pack_override_folders(i);
    if (g_pack.archive_overlay[i] &&
        g_rpf_file_header[i][1] != 1u + (uint32_t)folders + (uint32_t)rows) {
      __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
      pack_override_note("pack overlay a=%d toc entries=%u rows=%d folders=%d", i,
                         (unsigned)g_rpf_file_header[i][1], rows, folders);
      return unavailable(action, "pack overlay archive holds more than its override members");
    }
  }
  // Exact store headroom from the staged TOCs: LoadImage adds one slot per member and a full slot
  // pool traps the game, so the load needs every store's adds plus its margin free.
  {
    const GtavCustomPackTocCounts* counts[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {};
    for (int i = first; i < archives; ++i) counts[i] = &g_pack_toc_counts[i];
    PackStoreAdds adds;
    pack_store_adds(counts, &adds);
    uint32_t sources = 0;
    if (const char* refusal = pack_store_gate_exact(adds, &sources)) {
      __atomic_store_n(&g_pack_store_refusal_sources, sources, __ATOMIC_RELEASE);
      return unavailable(action, refusal);
    }
  }
  for (int r = 0; r < g_pack.override_count; ++r)
    if (const char* refusal = pack_override_gate(r)) {
      __atomic_store_n(&g_pack_fail_archive, (int)g_pack.overrides[r].archive, __ATOMIC_RELEASE);
      pack_override_note("pack override %s refused: %s", g_pack.overrides[r].member, refusal);
      // The row and toast name the member with the short reason; the klog line above has the
      // technical one.
      static char named[GTAV_FEATURE_MESSAGE_LEN];
      if (strstr(refusal, "loaded or requested"))
        snprintf(named, sizeof(named), "%s in use; restart GTA", g_pack.overrides[r].member);
      else
        snprintf(named, sizeof(named), "%s: %s", g_pack.overrides[r].member,
                 gtav_custom_pack_user_reason(refusal));
      return unavailable(action, named);
    }
  if (g_pack.override_count)
    pack_override_note("pack override gate ok rows=%d", g_pack.override_count);
  g_rpf_register_attempted = 1;
  g_rpf_register_started = first;
  for (int i = first; i < archives; ++i)
    if (const char* failure = add_pack_archive(i)) {
      __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
      return failed(action, failure);
    }
  g_rpf_registered = archives;
  if (g_rpf_index < 0) g_rpf_index = g_rpf_indices[0];
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack registered d=%d t=%d m=%d l=%d a=%d", g_pack.data_count,
           g_pack.typ_count, g_pack.map_count, g_pack.label_count, archives);
  return ok(action, message);
}

// Streaming-info word of archive `i`'s own streamable with bit 31 set as "found", or 0 when
// unavailable. Status is in bits 0-1.
static uint32_t pack_archive_flags_at(int i, uint32_t* global_out) {
  const uintptr_t modules = *(const volatile uintptr_t*)kStreamingModules;
  const uint32_t module_index = *(const volatile uint32_t*)kRpfModuleIndex;
  const uintptr_t module = modules ? ((const volatile uintptr_t*)modules)[module_index] : 0;
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  if (!module || !infos || i < 0 || i >= GTAV_CUSTOM_PACK_ARCHIVE_CAP || g_rpf_indices[i] < 0)
    return 0;
  const int32_t index = g_rpf_indices[i];
  const uint32_t global = *(const volatile uint32_t*)(module + 8) + (uint32_t)index;
  const volatile uint32_t* entry = (const volatile uint32_t*)(infos + (uintptr_t)global * 8u);
  if (entry[0] != (0x80000000u | (uint32_t)index)) return 0;
  if (global_out) *global_out = global;
  return entry[1] | 0x80000000u;
}

static uint32_t pack_archive_flags(uint32_t* global_out) {
  return pack_archive_flags_at(0, global_out);
}

#if defined(__ORBIS__)
// Game thread, read-only checks: each archive's recorded record must still name its cache file and
// be resident, and every vehicle or ped model must be known. Then the set counts as registered and
// streamed, and the worker marks its rows loaded; else the earlier instance's load stays the game's
// and only a restart loads these packs again.
static uint32_t adopt_pack_set(uint32_t action) {
  typedef const char* (*HashToStringFn)(uint32_t, uint32_t);
  g_pack_adopt_pending = 0;
  const uintptr_t records = *(const volatile uintptr_t*)kArchiveTable;
  const uint16_t count = *(const volatile uint16_t*)(kArchiveTable + 8);
  const int archives = pack_archive_count();
  int held = archives > 0 && records && count && pack_readable(records, (size_t)count * 0x68u);
  for (int i = 0; held && i < archives; ++i) {
    const int32_t index = g_pack_adopt_indices[i];
    char dir[GTAV_PACK_PATH_MAX], path[GTAV_PACK_PATH_MAX];
    held = index >= 0 && index < (int32_t)count &&
           pack_cache_path(i, dir, sizeof(dir), path, sizeof(path)) == 0;
    if (!held) break;
    const uint32_t name_hash =
        *(const volatile uint32_t*)(records + (uint32_t)index * 0x68u + 0x14u);
    const char* name = name_hash ? ((HashToStringFn)kHashToString)(1, name_hash) : nullptr;
    held = name && pack_same_path(name, path);
    if (held) {
      g_rpf_indices[i] = index;
      held = (pack_archive_flags_at(i, nullptr) & 0x80000003u) == 0x80000001u;
    }
  }
  for (int r = 0; held && r < g_pack.spawn_count; ++r) {
    const uint32_t kind = g_pack.spawns[r].kind;
    if (kind == GTAV_CUSTOM_PACK_SPAWN_VEHICLE || kind == GTAV_CUSTOM_PACK_SPAWN_PED)
      held = pack_model_known(pack_joaat(g_pack.spawns[r].model)) == 1;
  }
  if (!held) {
    for (int i = 0; i < GTAV_CUSTOM_PACK_ARCHIVE_CAP; ++i) g_rpf_indices[i] = -1;
    pack_override_note("pack adopt refused: the game no longer holds the recorded set");
    static char refusal[GTAV_FEATURE_MESSAGE_LEN];
    snprintf(refusal, sizeof(refusal),
             "%.32s already registered in this game; restart GTA",
             g_pack.source_ids[0]);
    return unavailable(action, refusal);
  }
  g_rpf_register_attempted = 1;
  g_rpf_register_started = 0;
  g_rpf_registered = archives;
  g_rpf_requested = archives;
  g_rpf_archive_load_attempted = 1;
  g_rpf_index = g_rpf_indices[0];
  __atomic_store_n(&g_pack_adopted, 1, __ATOMIC_RELEASE);
  return ok(action, "pack set kept from before the reload");
}
#endif

// Every archive from `first` (this load's, for an additive one) has its own streamable resident
// (members may be requested).
static int pack_archives_resident(int first = 0) {
  if (g_rpf_index < 0) return 0;
  for (int i = first; i < pack_archive_count(); ++i)
    if ((pack_archive_flags_at(i, nullptr) & 0x80000003u) != 0x80000001u) return 0;
  return 1;
}

// One-shot REQUEST_STREAMED_TEXTURE_DICT of card `param`; the card follows the latest request.
// Only once the archive's own streamable is resident (order verified on hardware: archive, then
// member).
static uint32_t request_pack_card(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REQUEST_PACK_CARD;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (param >= (uint32_t)g_pack.card_count) return unavailable(action, "pack card index out of range");
  if (g_pack_cards_requested & (1u << param))
    return unavailable(action, "pack card request already attempted");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (!g_n.request_streamed_texture_dict)
    return unavailable(action, "texture request native unavailable");
  if (!pack_archives_resident())
    return unavailable(action, "pack archive is not resident; run pack-load-archive first");
  g_pack_cards_requested |= 1u << param;
  g_rpf_request_attempted = 1;
  g_pack_last_card = (int)param + 1;
  const char* dict = g_pack.cards[param].dict;
  gtav_worker_klog("GTAVMenu pack card request before");
  invoke_void_ctx(0, g_n.request_streamed_texture_dict, dict, 0);
  gtav_worker_klog("GTAVMenu pack card request returned");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack card %u request returned dict=%s", (unsigned)param,
           dict);
  return ok(action, message);
}

// Request only each registered archive's own streamable (rpf module base + idx), which makes the
// engine stream the archive header/TOC and re-parse it (0x315a2d0 normalized) without requesting
// any member. On hardware: flags 0x0 -> 0x100002 -> 0x100001 within a few seconds.
static uint32_t load_pack_archive() {
#if defined(__ORBIS__)
  typedef void (*RequestObjectFn)(uintptr_t, uint32_t, uint32_t);
#else
  typedef void (*RequestObjectFn)(uint32_t, uint32_t);
#endif
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE;
  const int archives = pack_archive_count();
  if (g_rpf_index < 0 || g_rpf_registered < archives)
    return unavailable(action, "pack is not registered");
  // An additive load requests only the archives it registered (g_rpf_requested onwards).
  const int first = g_rpf_requested;
  if (g_rpf_archive_load_attempted && first >= archives)
    return unavailable(action, "pack archive load already attempted");
  if (g_rpf_request_attempted) return unavailable(action, "pack card already requested");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (memcmp((const void*)kRequestObject, kRequestObjectBytes, sizeof(kRequestObjectBytes)))
    return unavailable(action, "pack code anchors mismatch");
  uint32_t globals[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {};
  uint32_t before[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {};
  for (int i = first; i < archives; ++i)
    if (!(before[i] = pack_archive_flags_at(i, &globals[i])))
      return unavailable(action, "pack archive streamable handle mismatch");
  g_rpf_archive_load_attempted = 1;
  g_rpf_requested = archives;
  for (int i = first; i < archives; ++i) {
    gtav_worker_klog("GTAVMenu pack archive request before");
#if defined(__ORBIS__)
    ((RequestObjectFn)kRequestObject)(GTAV_LANE_kStreamingInfoManager, globals[i], 0x10u);
#else
    ((RequestObjectFn)kRequestObject)(globals[i], 0x10u);
#endif
    gtav_worker_klog("GTAVMenu pack archive request returned");
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                       "pack archive request a=%d global=%u flags 0x%x -> 0x%x", i,
                       (unsigned)globals[i], (unsigned)(before[i] & 0x7fffffffu),
                       (unsigned)(pack_archive_flags_at(i, nullptr) & 0x7fffffffu));
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack archive request returned archives=%d", archives - first);
  return ok(action, message);
}
#endif

// The sections: the engine lane (PS5, PS4), or the engine-less shell on any platform.
#if !GTAV_MENU_CUSTOM_PACK_ENGINE || defined(__PROSPERO__) || defined(__ORBIS__)
// Lowercase joaat, the engine's model / label / weapon name hash.
[[gnu::noinline]] static uint32_t pack_joaat(const char* text) {
  uint32_t hash = 0;
  for (const char* p = text; *p; ++p) {
    const char c = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
    hash += (uint8_t)c;
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

#if GTAV_MENU_CUSTOM_PACK_ENGINE
#include "custom_pack_data.inc"
#if defined(__ORBIS__)
#include "custom_pack_ps4_world.inc"
#else
#include "custom_pack_world.inc"
#endif
#else
#include "custom_pack_shell.inc"
#endif
#include "custom_pack_hide.inc"
#include "custom_pack_labels.inc"
#include "custom_pack_select.inc"
#include "custom_pack_autoload.inc"

#if GTAV_MENU_CUSTOM_PACK_ENGINE
// Read-only: the archive streamable's word, the members registered from it ((idx << 16) | n), and
// the re-parsed TOC (entry count and entry 1's 16 bytes).
static uint32_t inspect_pack() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_INSPECT_PACK;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  const uintptr_t entries = *(const volatile uintptr_t*)kTextureEntries;
  GtavLooseVqInfo info = {};
  if (!entries || !loose_query(entries, &info) || !(info.protection & 1))
    return unavailable(action, "streaming info unreadable");
  uint64_t slots = ((uintptr_t)info.end - entries) / 8u;
  if (slots > 4000000u) slots = 4000000u;
  uint32_t members = 0, resource_backed = 0, loaded = 0, requested = 0;
  for (uint64_t i = 0; i < slots; ++i) {
    const uint32_t handle = *(const volatile uint32_t*)(entries + i * 8u);
    if (handle & 0x80000000u) continue;
    int ours = 0;
    for (int a = 0; a < pack_archive_count() && !ours; ++a)
      ours = (handle >> 16) == (uint32_t)g_rpf_indices[a];
    if (!ours) continue;
    const uint32_t flags = *(const volatile uint32_t*)(entries + i * 8u + 4u);
    members++;
    if (flags & 0x08000000u) resource_backed++;
    if ((flags & 3u) == 1u) loaded++;
    if ((flags & 3u) == 2u) requested++;
  }
  for (int r = 0; r < g_pack.override_count; ++r) {
    const uint32_t global = g_override_globals[r];
    pack_override_note("pack override %s g=%u h=0x%x fl=0x%x", g_pack.overrides[r].member,
                       (unsigned)global,
                       (unsigned)*(const volatile uint32_t*)(entries + (uintptr_t)global * 8u),
                       (unsigned)*(const volatile uint32_t*)(entries + (uintptr_t)global * 8u + 4u));
  }
  const uint32_t archive_flags = pack_archive_flags(nullptr);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack members=%u rsc=%u loaded=%u requested=%u archive flags=0x%x found=%d",
                     (unsigned)members, (unsigned)resource_backed, (unsigned)loaded,
                     (unsigned)requested, (unsigned)(archive_flags & 0x7fffffffu),
                     archive_flags != 0);
  // Data files parse asynchronously; these stores show when a load has landed.
  uint32_t store_count = 0, store_capacity = 0;
  const int store_ok = pack_vehicle_model_store(&store_count, &store_capacity) == 0;
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack stores handling=%u vehicles=%u/%u ok=%d archetypes_free=%u",
                     (unsigned)*(const volatile uint16_t*)kHandlingCount, (unsigned)store_count,
                     (unsigned)store_capacity, store_ok,
                     (unsigned)*(const volatile uint32_t*)kArchetypePoolFree);
  const uintptr_t table = *(const volatile uintptr_t*)kArchiveTable;
  if (table) {
    const uintptr_t record = table + (uintptr_t)(uint32_t)g_rpf_index * 0x68u;
    const uintptr_t pack = *(const volatile uintptr_t*)(record + 0x38);
    uintptr_t toc = 0;
    uint32_t toc_count = 0;
    int memory_ok = 0;
    if (pack && loose_query(pack, &info) && (info.protection & 1)) {
      toc = *(const volatile uintptr_t*)(pack + 0x28);
      toc_count = *(const volatile uint32_t*)(pack + 0x30);
#if !defined(__ORBIS__)
      memory_ok = *(const volatile uintptr_t*)(pack + 0x58) == kFiMemoryDevice &&
                  *(const volatile uintptr_t*)(pack + 0x38) == (uintptr_t)g_rpf_memory[0];
#endif
    }
    char hex[33] = "-";
    if (toc && toc_count > 1 && loose_query(toc + 16u, &info) && (info.protection & 1) &&
        toc + 32u <= (uintptr_t)info.end) {
      for (int b = 0; b < 16; ++b)
        snprintf(hex + 2 * b, 3, "%02x", *(const volatile uint8_t*)(toc + 16u + (uintptr_t)b));
    }
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack toc flags=0x%x memory=%d n=%u e1=%s",
                       (unsigned)*(const volatile uint16_t*)(record + 0x48), memory_ok,
                       (unsigned)toc_count, hex);
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  int resident = 0;
  for (int a = 0; a < pack_archive_count(); ++a)
    resident += (pack_archive_flags_at(a, nullptr) & 0x80000003u) == 0x80000001u;
  snprintf(message, sizeof(message), "pack res=%d/%d m=%u ld=%u f=0x%x", resident,
           pack_archive_count(), (unsigned)members, (unsigned)loaded,
           (unsigned)(archive_flags & 0x7fffffffu));
  return members ? ok(action, message) : failed(action, "pack has no registered members");
}

// A row still loaded or referenced: start the wait (or keep it), log the row it waits on when that
// changes, and refuse with the waiting text.
static uint32_t pack_revert_wait(uint32_t action, const char* member, uint32_t flags, int32_t refs,
                                 uint32_t* answer) {
  static char named[GTAV_FEATURE_MESSAGE_LEN];
  const uint64_t now = pack_al_now_ms();
  uint64_t idle = 0;
  if (__atomic_compare_exchange_n(&g_revert_wait_ms, &idle, now ? now : 1u, false, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&g_revert_asked_ms, now, __ATOMIC_RELEASE);
    g_revert_member[0] = '\0';
  }
  if (strncmp(g_revert_member, member, sizeof(g_revert_member) - 1u) != 0) {
    snprintf(g_revert_member, sizeof(g_revert_member), "%s", member);
    pack_world_note("pack revert waiting %s fl=0x%x refs=%d", member, (unsigned)flags, (int)refs);
  }
  *answer = PACK_REVERT_IN_USE;
  snprintf(named, sizeof(named), "Waiting: %.32s in use; move away", member);
  return unavailable(action, named);
}

// Quick unload (PS4 attempt 061: a replaced Adder stayed cached for minutes after its last car was
// gone). A row that is loaded but unused -- no loaded object depending on it (info flags & 0xfffc),
// no references (store vt+0xd0, pack_override_refs) and no keep-resident reason (flags >> 16 &
// 0xf1) -- is unloaded with the game's own RemoveObject (1.57 0x230f060 (manager, global); PS5
// kRemoveObject (global)), the call the asset-store sweep makes under the same test (1.57
// 0x29d58d0, PS5 fwAssetStore_RemoveAll 0x4b8f50 normalized 011). RemoveObject itself refuses a
// loaded object a dependent counts on. Unloading a model releases its texture dictionary's
// dependent count, so a pass repeats while it unloads something. Rows still in use are left to the
// wait; a row RemoveObject declined is not tried again (the wait asks every 2 s). Game thread.
static uint64_t g_revert_evict_declined;  // per override row
static_assert(GTAV_CUSTOM_PACK_OVERRIDE_CAP <= 64, "override row masks are uint64_t");
static void pack_revert_evict_unused(uintptr_t infos) {
#if defined(__ORBIS__)
  typedef bool (*RemoveObjectFn)(uintptr_t, uint32_t);
  static const uint8_t kRemoveObjectBytes[] = GTAV_LANE_kRemoveObjectBytes;
  if (memcmp((const void*)GTAV_LANE_kRemoveObject, kRemoveObjectBytes, sizeof(kRemoveObjectBytes))) {
#else
  typedef bool (*RemoveObjectFn)(uint32_t);  // custom_pack_world.inc pins kRemoveObject
  if (memcmp((const void*)kRemoveObject, kRemoveObjectBytes, sizeof(kRemoveObjectBytes))) {
#endif
    pack_world_note("pack revert evict skipped: RemoveObject anchors mismatch");
    return;
  }
  for (int pass = 0; pass < g_pack.override_count; ++pass) {
    int removed = 0;
    for (int r = 0; r < g_pack.override_count; ++r) {
      const uint32_t global = g_override_globals[r];
      const uint32_t flags = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u + 4u);
      if ((g_revert_evict_declined >> r) & 1u) continue;
      if ((flags & 0xffffu) != 1u || ((flags >> 16) & 0xf1u) != 0u) continue;
      const size_t s = g_override_store[r];
      const uint32_t base = *(const volatile uint32_t*)(kOverrideStores[s].store + 8);
      if (pack_override_refs(s, (int32_t)(global - base)) != 0) continue;
#if defined(__ORBIS__)
      const bool done =
          ((RemoveObjectFn)GTAV_LANE_kRemoveObject)(GTAV_LANE_kStreamingInfoManager, global);
#else
      const bool done = ((RemoveObjectFn)kRemoveObject)(global);
#endif
      const uint32_t after = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u + 4u);
      pack_world_note("pack revert evict %s fl=0x%x->0x%x ok=%d", g_pack.overrides[r].member,
                      (unsigned)flags, (unsigned)after, (int)done);
      if ((after & 3u) == 0u)
        ++removed;
      else
        g_revert_evict_declined |= 1ull << r;
    }
    if (!removed) break;
  }
}

// Custom Packs "Revert overrides": put every stock member
// the loaded packs override back on its stock archive, without a restart. Per row it undoes exactly
// what the load changed:
//   info.handle   ours -> the handle RegisterFile saved in our overlay node (g_override_prev);
//   overlay node  plain row: removed with the pinned remover (0x3558c60, as the game's own RPF_FILE
//                 disable path 0x159b2c5 does inline); v1.1 row: our node's old handle is put back
//                 to the base handle of the DLC node the load removed (g_override_dlc_old), which
//                 is that DLC node's content again, without an allocation;
// then each overlay archive's record loses its 0x40 override permit, so no later registration of
// it can override again. Every row is gated before the first write (all or nothing): its handle and
// node are still the ones the load left, and the object is neither loaded nor requested (flags & 3
// == 0) with no slot refs. A loaded object holds data parsed from our archive, so a model in use
// refuses. A removed weapon's model stays resident until the streamer evicts it: on hardware it
// needed the weapon removed and ~2 minutes far away, so a row in use starts a wait instead (F5.2,
// pack_revert_wait): the toast says "Waiting: <member> in use; move away" (no '~': the game's text
// renderer reads it as a format token; the member is cut to 32 characters for the toast's 80-byte
// buffer with its status glyph) and the worker asks again every 2 s for up to 3 min. While waiting,
// a refused ask logs only the row it waits on when that changes (the klog ring holds 128 lines).
// Game thread.
static uint32_t revert_pack_override_rows(uint32_t* answer) {
#if !defined(__ORBIS__)
  typedef void (*RemoveFn)(uint32_t);
#endif
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (!g_pack.override_count) return unavailable(action, "no stock overrides are loaded");
  if (__atomic_load_n(&g_overrides_reverted, __ATOMIC_ACQUIRE))
    return unavailable(action, "overrides already reverted");
  if (!texture_action_is_stable_main_thread()) {
    *answer = PACK_REVERT_BUSY;
    return unavailable(action, "pack phase/thread gate refused");
  }
  const uintptr_t infos = *(const volatile uintptr_t*)kTextureEntries;
  if (!infos) return unavailable(action, "streaming info unavailable");
  pack_revert_evict_unused(infos);
  const int waiting = __atomic_load_n(&g_revert_wait_ms, __ATOMIC_ACQUIRE) != 0;
  int plain = 0;
  for (int r = 0; r < g_pack.override_count; ++r) {
    const char* member = g_pack.overrides[r].member;
    const uint32_t global = g_override_globals[r];
    const volatile uint32_t* entry = (const volatile uint32_t*)(infos + (uintptr_t)global * 8u);
    const uint32_t handle = entry[0], flags = entry[1];
    const size_t s = g_override_store[r];
    const uint32_t base = *(const volatile uint32_t*)(kOverrideStores[s].store + 8);
    const int32_t local = (int32_t)(global - base);
    const int32_t refs = pack_override_refs(s, local);
    uint32_t node_old = 0;
    const int node = pack_overlay_has(global, &node_old);
    const int changed = !g_override_took[r] || handle != g_override_ours[r] || !node ||
                        node_old != g_override_prev[r];
    if (!waiting || changed)
      pack_world_note("pack revert gate %s g=%u h=0x%x fl=0x%x refs=%d node=%d old=0x%x dlc=%d",
                      member, (unsigned)global, (unsigned)handle, (unsigned)flags, (int)refs, node,
                      (unsigned)node_old, (int)g_override_remove[r]);
    if (changed) {
      static char named[GTAV_FEATURE_MESSAGE_LEN];
      snprintf(named, sizeof(named), "%.32s changed since load; restart GTA", member);
      return unavailable(action, named);
    }
    if ((flags & 3u) || refs > 0) return pack_revert_wait(action, member, flags, refs, answer);
    plain += !g_override_remove[r];
  }
#if defined(__ORBIS__)
  if (plain &&
      memcmp((const void*)GTAV_LANE_kGameFree, kOverlayFreeBytes, sizeof(kOverlayFreeBytes)) != 0)
    return unavailable(action, "pack override remover anchors mismatch");
#else
  if (plain &&
      memcmp((const void*)kOverlayRemove, kOverlayRemoveBytes, sizeof(kOverlayRemoveBytes)) != 0)
    return unavailable(action, "pack override remover anchors mismatch");
#endif

  const uint32_t entries_before = *(const volatile uint16_t*)kOverlayEntries;
  __atomic_store_n(&g_overrides_reverted, 1, __ATOMIC_RELEASE);
  for (int r = 0; r < g_pack.override_count; ++r) {
    const uint32_t global = g_override_globals[r];
    *(volatile uint32_t*)(infos + (uintptr_t)global * 8u) = g_override_prev[r];
    if (g_override_remove[r]) {
      if (const uintptr_t node = pack_overlay_node(global))
        *(volatile uint32_t*)(node + 4) = g_override_dlc_old[r];
    } else {
#if defined(__ORBIS__)
      pack_overlay_remove(global);
#else
      ((RemoveFn)kOverlayRemove)(global);
#endif
    }
  }
  const uintptr_t table = *(const volatile uintptr_t*)kArchiveTable;
  for (int i = 0; i < pack_archive_count(); ++i)
    if (g_pack.archive_overlay[i] && table && g_rpf_indices[i] >= 0)
      __atomic_fetch_and((uint16_t*)(table + (uintptr_t)(uint32_t)g_rpf_indices[i] * 0x68u + 0x48),
                         (uint16_t)~0x40u, __ATOMIC_SEQ_CST);

  int reverted = 0;
  for (int r = 0; r < g_pack.override_count; ++r) {
    const uint32_t global = g_override_globals[r];
    const uint32_t handle = *(const volatile uint32_t*)(infos + (uintptr_t)global * 8u);
    uint32_t node_old = 0;
    const int node = pack_overlay_has(global, &node_old);
    const int done = handle == g_override_prev[r] &&
                     (g_override_remove[r] ? node && node_old == g_override_dlc_old[r] : !node);
    pack_world_note("pack revert %s h=0x%x->0x%x node=%d old=0x%x reverted=%d",
                    g_pack.overrides[r].member, (unsigned)g_override_ours[r], (unsigned)handle,
                    node, (unsigned)node_old, done);
    reverted += done;
  }
  const uint32_t entries = *(const volatile uint16_t*)kOverlayEntries;
  pack_world_note("pack revert rows=%d/%d overlay entries %u->%u", reverted,
                  g_pack.override_count, (unsigned)entries_before, (unsigned)entries);
  if (reverted != g_pack.override_count || entries + (uint32_t)plain != entries_before)
    return failed(action, "pack override revert did not take (see pack-notes)");
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack overrides reverted rows=%d", reverted);
  *answer = PACK_REVERT_DONE;
  return ok(action, message);
}

// The game-thread job (menu press, host pack-revert, or the wait's ask): one revert attempt, its
// answer published for the worker's wait (pack_revert_tick).
static uint32_t revert_pack_overrides() {
  uint32_t answer = PACK_REVERT_REFUSED;
  const uint32_t result = revert_pack_override_rows(&answer);
  __atomic_store_n(&g_revert_answer, answer, __ATOMIC_RELEASE);
  return result;
}
#else
// Engine-less lane: every step that registers or loads into the engine refuses before it reads
// anything.
static uint32_t pack_engine_refusal(uint32_t action) {
  return unavailable(action, GTAV_CUSTOM_PACK_ENGINE_REASON);
}

static uint32_t register_pack() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK);
}

static uint32_t request_pack_card(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_REQUEST_PACK_CARD);
}

static uint32_t load_pack_archive() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE);
}

static uint32_t inspect_pack() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_INSPECT_PACK);
}

static uint32_t load_pack_data(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA);
}

static uint32_t pump_pack_data() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_PUMP_PACK_DATA);
}

static uint32_t load_pack_typ(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP);
}

static uint32_t add_pack_labels() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS);
}

static uint32_t load_pack_map(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP);
}

static uint32_t finish_pack_map(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP);
}

static uint32_t load_pack_bounds(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS);
}

// Component rows name pack weapons and components, which come from the pack's data files.
static uint32_t toggle_pack_component(uint32_t) {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_COMPONENT);
}

static uint32_t revert_pack_overrides() {
  return pack_engine_refusal(GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES);
}
#endif
#else
#define GTAV_PACK_STUB_REASON "custom packs need PS5"
#endif

#else
#define GTAV_PACK_STUB_REASON "pack lane requires CUSTOM_PACKS=1 build"

static uint32_t register_pack() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK, GTAV_PACK_STUB_REASON);
}

static uint32_t request_pack_card(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_REQUEST_PACK_CARD, GTAV_PACK_STUB_REASON);
}

static uint32_t load_pack_archive() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE, GTAV_PACK_STUB_REASON);
}
#endif

#ifdef GTAV_PACK_STUB_REASON
static uint32_t inspect_pack() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_INSPECT_PACK, GTAV_PACK_STUB_REASON);
}

static uint32_t load_pack_data(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_DATA, GTAV_PACK_STUB_REASON);
}

static uint32_t pump_pack_data() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_PUMP_PACK_DATA, GTAV_PACK_STUB_REASON);
}

static uint32_t load_pack_typ(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_TYP, GTAV_PACK_STUB_REASON);
}

static uint32_t add_pack_labels() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_ADD_PACK_LABELS, GTAV_PACK_STUB_REASON);
}

static uint32_t load_pack_map(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_MAP, GTAV_PACK_STUB_REASON);
}

static uint32_t finish_pack_map(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_FINISH_PACK_MAP, GTAV_PACK_STUB_REASON);
}

static uint32_t load_pack_bounds(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_BOUNDS, GTAV_PACK_STUB_REASON);
}

static uint32_t apply_pack_timecycle(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_APPLY_PACK_TIMECYCLE, GTAV_PACK_STUB_REASON);
}

static uint32_t play_pack_ptfx(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_PLAY_PACK_PTFX, GTAV_PACK_STUB_REASON);
}

static uint32_t toggle_pack_component(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_COMPONENT, GTAV_PACK_STUB_REASON);
}

static uint32_t apply_pack_hide(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_PACK_HIDE, GTAV_PACK_STUB_REASON);
}

static uint32_t revert_pack_overrides() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_REVERT_PACK_OVERRIDES, GTAV_PACK_STUB_REASON);
}

static uint32_t start_pack_autoload() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD, GTAV_PACK_STUB_REASON);
}

static uint32_t toggle_pack_active(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_TOGGLE_PACK_ACTIVE, GTAV_PACK_STUB_REASON);
}

static uint32_t uninstall_pack(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_UNINSTALL_PACK, GTAV_PACK_STUB_REASON);
}

static const char* pack_uninstall_value_label() { return ""; }

static void pack_autoload_tick() {}

__attribute__((unused)) static void pack_interior_diag_tick() {}

__attribute__((unused)) static int pack_interior_collision_ready() { return 1; }

static int pack_map_place(uint32_t, int32_t*, int32_t*, int32_t*, const char**, int*) { return 0; }

static int scene_pack_model(uint32_t, const char**, const char**, int*) { return 0; }

extern "C" int gtav_features_pack_menu(GtavPackMenuRow* rows, int max) {
  if (!rows || max < 1) return 0;
  rows[0].action = GTAV_NATIVE_SHELL_ACTION_PACK_AUTOLOAD;
  rows[0].param = 0;
  rows[0].ready = 0;
  rows[0].reason = GTAV_PACK_STUB_REASON;
  rows[0].toggle = -1;
  snprintf(rows[0].label, sizeof(rows[0].label), "Custom packs unavailable");
  return 1;
}

// native_bridge.cpp references these weakly. Define them here so a pack-free worker carries no
// undefined weak imports: the injector resolves every import by name and fails closed on a miss.
extern "C" const char* gtav_features_pack_footer(uint32_t, uint32_t) { return ""; }

extern "C" int gtav_features_pack_vehicle_name(uint32_t, char* out, int size) {
  if (out && size > 0) out[0] = '\0';
  return 0;
}
#undef GTAV_PACK_STUB_REASON
#endif
