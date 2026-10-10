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
// selection) and custom_pack_autoload.inc (one-press load and menu rows) follow on PS5. Every other build gets the refusal stubs at the end.

#if GTAV_MENU_ENABLE_CUSTOM_PACKS

#include "gtavmenu/aes256.h"
#include "gtavmenu/custom_pack_checks.h"
#include "gtavmenu/custom_pack_runtime.h"

static const uintptr_t kAddImageToList = 0x35563e0;
static const uintptr_t kLoadImage = 0x3557250;
static const uintptr_t kHashToString = 0x2f08060;
static const uintptr_t kCloseArchiveToc = 0x3559f30;
static const uintptr_t kRequestObject = 0x355aa20;       // (global index, request flags)
static const uintptr_t kStreamingModules = 0x62271c8;    // module* []
static const uintptr_t kRpfModuleIndex = 0x620f768;      // u32 index of the "rpf" module
static const uintptr_t kArchiveTable = 0x620f758;        // atArray {ptr, u16 count, u16 capacity}
static const uintptr_t kArchiveHeaderCache = 0x620f810;  // startup-only TOC cache stream
static const uintptr_t kCollectionTable = 0x6210f40;     // fiCollection* [7000], slot 0 loose
static const uint32_t kCollectionSlots = 7000;
static const uint8_t kAddImageToListBytes[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};
static const uint8_t kLoadImageBytes[24] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                            0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
                                            0x58, 0x02, 0x00, 0x00, 0x4c, 0x8b, 0x3d, 0xf5};
static const uint8_t kCloseArchiveTocBytes[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                  0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x89, 0xf8};
static const uint8_t kRequestObjectBytes[16] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec};
static const uint8_t kHashToStringBytes[10] = {0x31, 0xc0, 0x85, 0xf6, 0x0f,
                                               0x84, 0x0d, 0x01, 0x00, 0x00};

// The engine memory device (fiDeviceMemory, class 6): OpenBulk returns the parsed address and
// ReadBulk is a plain memcpy. The archive TOC key hashes the text after the second ':', i.e. the
// archive basename, so a memory name keeps the key the host tool encrypted the table for.
static const uintptr_t kFiMemoryDevice = 0x5ca9c10;
static const uintptr_t kFiMemoryVtable = 0x4072168;
static const uintptr_t kStreamerQueues = 0x5c85b20;
static const uint32_t kRpfMagic = 0x52504637u;
static const uint32_t kRpfKeyedTable = 0x0ffeffffu;  // retail AES table tag

#define GTAV_PACK_PATH_MAX 192

// /data/gtavmenu/custom/packs, then "/<id>" when `id` is given, then `tail`.
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
// Per archive (0 = the descriptor's first): its never-freed buffer and memory-device name.
static uint8_t* g_rpf_memory[GTAV_CUSTOM_PACK_ARCHIVE_CAP];
static uint64_t g_rpf_memory_bytes[GTAV_CUSTOM_PACK_ARCHIVE_CAP];
static char g_rpf_memory_name[GTAV_CUSTOM_PACK_ARCHIVE_CAP][GTAV_PACK_PATH_MAX];
static uint32_t g_rpf_file_header[GTAV_CUSTOM_PACK_ARCHIVE_CAP][4];

// DataFile as read by AddImageToList: filename at +0, the remaining fields zero (plain RPF_FILE).
static uint8_t g_rpf_datafile[GTAV_CUSTOM_PACK_ARCHIVE_CAP][0x100];
static int g_rpf_register_attempted;
static int32_t g_rpf_indices[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                                              -1, -1, -1, -1, -1, -1, -1, -1};
static_assert(GTAV_CUSTOM_PACK_ARCHIVE_CAP == 16, "g_rpf_indices lists -1 (unregistered) per archive");
static int32_t g_rpf_index = -1;  // archive 0, set once every archive of the pack is registered
static int g_rpf_request_attempted;  // any card requested
static uint32_t g_pack_cards_requested;  // per card row (bit = row)
static_assert(GTAV_CUSTOM_PACK_CARD_CAP <= 32, "the card mask is uint32_t");
static int g_pack_last_card;  // 1-based card the operator requested last; 0 none
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

// Directory holding archive `i` / data row `row` of the merged set.
static const char* pack_archive_dir(int i) {
  return g_pack_dirs[i == 0 ? 0 : g_pack.extra_archive_source[i - 1]];
}

static const char* pack_data_dir(uint32_t row) { return g_pack_dirs[g_pack.data_source[row]]; }

#if defined(__PROSPERO__) || defined(__ORBIS__)
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
static const uintptr_t kRpfTocKeyTable = 0x4307060;  // 101 x 32-byte AES-256 keys
static const uint8_t kRpfTocKeyTableSha256[32] = {0x7f, 0x96, 0xda, 0x7f, 0xeb, 0x73, 0x0c, 0xf4, 0x19, 0x30, 0x9d, 0x87, 0x8b, 0x92, 0x76, 0x08, 0xe8, 0x12, 0x75, 0x3c, 0xad, 0x34, 0x8f, 0x1b, 0x93, 0x34, 0x38, 0x7a, 0x2c, 0xf3, 0xde, 0x8a};

static const char* pack_key_plain_toc(int i, uint8_t* memory, uint64_t size, const char* name) {
  uint32_t* header = (uint32_t*)memory;
  const uint64_t count = header[1], names = header[2];
  const uint64_t bytes = count * 16u + names;
  if (count < 2u || (names >> 28) != 0u || 16u + bytes > size || bytes % 16u != 0u)
    return "pack plain toc layout refused";
  const int32_t index = gtav_rpf_toc_index(name, size);
  if (index < 0 || (uint32_t)index >= GTAV_RPF_TOC_KEY_COUNT) return "pack plain toc index refused";
  if (!pack_sha256_matches((const uint8_t*)kRpfTocKeyTable,
                           GTAV_RPF_TOC_KEY_COUNT * GTAV_RPF_TOC_KEY_BYTES, kRpfTocKeyTableSha256))
    return "pack toc key table pin mismatch";
  uint8_t key[GTAV_RPF_TOC_KEY_BYTES];
  memcpy(key, (const void*)(kRpfTocKeyTable + (uintptr_t)index * GTAV_RPF_TOC_KEY_BYTES), sizeof(key));
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
  GtavSha256 sha;
  uint8_t digest[32];
  gtav_sha256_init(&sha);
  gtav_sha256_update(&sha, memory, (size_t)size);
  gtav_sha256_final(&sha, digest);
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack toc a=%d keyed idx=%d bytes=%llu sha=%02x%02x%02x%02x%02x%02x%02x%02x", i,
                     (int)index, (unsigned long long)bytes, digest[0], digest[1], digest[2], digest[3],
                     digest[4], digest[5], digest[6], digest[7]);
  return nullptr;
}

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

#if defined(__PROSPERO__) || defined(__ORBIS__)
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

// Stock overrides: an `overlay` archive is added with
// AddImageToList a4 = 1 (archive record +0x48 bit 0x40), so LoadImage's RegisterFile re-points
// each stock member it carries at our archive and records {global -> old handle} in the overlay
// map. Every row is gated read-only before anything is registered: a stock name owned by a valid
// archive, neither requested nor resident (a resident object keeps the old data until evicted),
// and not already overlaid (a memory: archive may not re-override; patch DLCs overlay some cars).
static const uintptr_t kValidArchiveBits = 0x620f798;  // u32* bitset of valid archive indices
static const uintptr_t kValidArchiveBound = 0x620f7a2;  // u16
static const uintptr_t kOverlayBuckets = 0x6227170;    // node* [u16 @+8]; node {global, old, next}
static const uintptr_t kOverlayEntries = 0x622717a;    // u16
static const struct {
  const char* extension;
  uintptr_t store;
  uintptr_t vtable;
  uintptr_t find_slot;  // vt+0x48 (store, stem) -> local or -1, never creates a slot
} kOverrideStores[] = {
    {"ptd", 0x621f260, 0x40943d0, 0x114eef0},
    {"pft", 0x61a6d78, 0x40937e0, 0x353b160},
    {"pdr", 0x61a6cd8, 0x40934c0, 0x353a230},
    // DwdStore (drawable dictionaries; streamed ped components are filed by folder, so the slot
    // name is e.g. "player_one/uppr_014_u"): ctor 0x3101b75..0x3101bdc ELF registers "pdd" and
    // stores this vtable; FindSlot equals the DrawableStore's but for the hash rip displacement,
    // and vt+0xd0 (refs, live 0x353ad50) is the same code as the other three stores'.
    {"pdd", 0x61a6e18, 0x4093650, 0x353a9d0},
};
static uint32_t g_override_globals[GTAV_CUSTOM_PACK_OVERRIDE_CAP];
// v1.1 (seen on hardware: patch DLCs already overlay many base cars, e.g. banshee.ptd): a row whose
// name is already overlaid by a `dlc...`/`update...` archive is accepted; right before our
// LoadImage the game's own remover (0x3558c60 live) drops that overlay node on the main thread, so
// RegisterFile takes the plain override path and records {global -> DLC handle} -- the state the
// game produces itself when a dlc/update archive re-overrides.
static const uintptr_t kOverlayRemove = 0x3558c60;  // (global): unlink + free the node, count--
static const uint8_t kOverlayRemoveBytes[32] = {0x0f, 0xb7, 0x0d, 0x11, 0xe5, 0xcc, 0x02, 0x85, 0xc9, 0x74, 0x79, 0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x89, 0xf8, 0x31, 0xd2, 0xf7, 0xf1, 0x48, 0x8b, 0x05, 0xf2, 0xe4, 0xcc, 0x02, 0x48, 0x8b};
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

// Slot refs of `local` in override store `s`, read as the txd/frag/drawable vt+0xd0 do
// (0x3116830/0x313b4e0/0x313a5b0 ELF): slot flag byte >= 0, then
// u32[[store+0x40] + local * i32[store+0x54] + 8]. -1 = no slot data (no object, no refs).
static int32_t pack_override_refs(size_t s, int32_t local) {
  const uintptr_t store = kOverrideStores[s].store;
  const uintptr_t slot_flags = *(const volatile uintptr_t*)(store + 0x48);
  const uintptr_t slots = *(const volatile uintptr_t*)(store + 0x40);
  if (!slot_flags || !slots || *(const volatile int8_t*)(slot_flags + (uint32_t)local) < 0)
    return -1;
  const intptr_t stride = *(const volatile int32_t*)(store + 0x54);
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
  if (local < 0 || (uint32_t)local >= *(const volatile uint32_t*)(store + 0x50))
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
    if (memcmp((const void*)kOverlayRemove, kOverlayRemoveBytes, sizeof(kOverlayRemoveBytes)) != 0)
      return "pack override remover anchors mismatch";
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
  typedef int32_t (*AddImageToListFn)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  typedef const char* (*HashToStringFn)(uint32_t, uint32_t);
  typedef uint64_t (*FileTimeFn)(void*, const char*);
  typedef void (*LoadImageFn)(const char*, uint32_t);
  memset(g_rpf_datafile[i], 0, sizeof(g_rpf_datafile[i]));
  snprintf((char*)g_rpf_datafile[i], sizeof(g_rpf_datafile[i]), "%s", g_rpf_memory_name[i]);
  const uint32_t overlay = g_pack.archive_overlay[i] ? 1u : 0u;  // a4: override permit
  const int32_t index = ((AddImageToListFn)kAddImageToList)(g_rpf_datafile[i], 0, 0, 0, overlay, 0);
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
    typedef void (*RemoveFn)(uint32_t);
    if (g_pack.overrides[r].archive != i || !g_override_remove[r]) continue;
    ((RemoveFn)kOverlayRemove)(g_override_globals[r]);
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

static uint32_t register_pack() {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_REGISTER_PACK;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);

  if (g_rpf_index >= 0) return ok(action, "pack already registered");
  if (g_rpf_register_attempted) return unavailable(action, "pack registration already attempted");
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
  if (*(const volatile uintptr_t*)kArchiveHeaderCache != 0)
    return unavailable(action, "pack startup header cache still active");
  // LoadImage adds one slot per member to its store; a store whose slot pool is full traps
  // (int 0x41 in AddSlot). Fragment/drawable/texture-dictionary stores: size +0x50, first free +0x58
  // (-1 = full), used +0x60 (low 30 bits).
  // Fragment, drawable, texture-dictionary, archetype-definition (ptyp) and map-data stores.
  static const struct {
    uintptr_t store;
    uintptr_t vtable;
  } kStores[] = {{0x61a6d78, 0x40937e0},
                 {0x61a6cd8, 0x40934c0},
                 {0x621f260, 0x40943d0},
                 {0x6210a10, 0x4093b08},
                 {0x61a9aa0, 0x4093970}};
  const int store_count = (int)(sizeof(kStores) / sizeof(kStores[0]));
  uint32_t store_free[sizeof(kStores) / sizeof(kStores[0])] = {};
  for (int i = 0; i < store_count; ++i) {
    const uintptr_t store = kStores[i].store;
    if (*(const volatile uintptr_t*)store != kStores[i].vtable)
      return unavailable(action, "pack store anchors mismatch");
    const uint32_t size = *(const volatile uint32_t*)(store + 0x50);
    const uint32_t used = *(const volatile uint32_t*)(store + 0x60) & 0x3fffffffu;
    store_free[i] = *(const volatile uint32_t*)(store + 0x58) == UINT32_MAX || used > size
                        ? 0u
                        : size - used;
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack store free frag=%u drawable=%u txd=%u typ=%u mapdata=%u",
                     (unsigned)store_free[0], (unsigned)store_free[1], (unsigned)store_free[2],
                     (unsigned)store_free[3], (unsigned)store_free[4]);
  for (int i = 0; i < store_count; ++i)
    if (store_free[i] < 64u) return unavailable(action, "pack store headroom gate refused");
  // .pbn members add slots to the static bounds store too (size +0x50, used +0x60 low 30 bits;
  // a full store traps the same way.
  {
    const uintptr_t bounds_store = 0x61a9740;
    const uint32_t size = *(const volatile uint32_t*)(bounds_store + 0x50);
    const uint32_t used = *(const volatile uint32_t*)(bounds_store + 0x60) & 0x3fffffffu;
    if (*(const volatile uintptr_t*)bounds_store != 0x4093ff0 || used > size || size - used < 64u)
      return unavailable(action, "pack bounds store headroom gate refused");
  }

  if (const char* refusal = pack_load_descriptor()) return unavailable(action, refusal);
  const int archives = pack_archive_count();
  // Ped archives also add .pmt slots to the MetaDataStore and .pdd slots to the DwdStore (LoadImage
  // traps when a store is full). The MetaDataStore
  // runs close to full (20 free in a snapshot), so packs with peds need a little headroom there.
  int has_peds = 0;
  for (int i = 0; i < g_pack.data_count; ++i)
    has_peds |= g_pack.data[i].type == GTAV_CUSTOM_PACK_DATA_PED_METADATA;
  if (has_peds) {
    static const struct {
      uintptr_t store;
      uint32_t min_free;
    } kPedStores[] = {{0x6173230, 8u}, {0x61a6e18, 64u}};  // MetaDataStore, DwdStore
    for (const auto& ped_store : kPedStores) {
      const uint32_t size = *(const volatile uint32_t*)(ped_store.store + 0x50);
      const uint32_t used = *(const volatile uint32_t*)(ped_store.store + 0x60) & 0x3fffffffu;
      gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack ped store %p free=%u",
                         (void*)ped_store.store, used <= size ? (unsigned)(size - used) : 0u);
      if (*(const volatile uint32_t*)(ped_store.store + 0x58) == UINT32_MAX || used > size ||
          size - used < ped_store.min_free)
        return unavailable(action, "pack ped store headroom gate refused");
    }
  }

  // A `<asset>.ppt` member adds a PtFxAssetStore slot (ELF 0x5e26148; 364/400 used in a snapshot,
  // same pool layout). The builder pairs
  // every .ppt member with a `spawn ptfx` row, so a pack with such rows needs a little headroom.
  int has_ptfx = 0;
  for (int i = 0; i < g_pack.spawn_count; ++i)
    has_ptfx |= g_pack.spawns[i].kind == GTAV_CUSTOM_PACK_SPAWN_PTFX;
  if (has_ptfx) {
    const uintptr_t ptfx_store = 0x6226148;
    const uint32_t size = *(const volatile uint32_t*)(ptfx_store + 0x50);
    const uint32_t used = *(const volatile uint32_t*)(ptfx_store + 0x60) & 0x3fffffffu;
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL, "pack ptfx store free=%u",
                       used <= size ? (unsigned)(size - used) : 0u);
    if (*(const volatile uintptr_t*)ptfx_store != 0x4097eb0 ||
        *(const volatile uint32_t*)(ptfx_store + 0x58) == UINT32_MAX || used > size ||
        size - used < 8u)
      return unavailable(action, "pack ptfx store headroom gate refused");
  }

  // One archive record and one collection slot per archive.
  const uintptr_t records = *(const volatile uintptr_t*)kArchiveTable;
  const uint16_t count = *(const volatile uint16_t*)(kArchiveTable + 8);
  const uint16_t capacity = *(const volatile uint16_t*)(kArchiveTable + 10);
  uint32_t free_slot = 0, free_slots = 0, last_free = 0;
  for (uint32_t i = 1; i < kCollectionSlots && free_slots < (uint32_t)archives; ++i)
    if (((const volatile uintptr_t*)kCollectionTable)[i] == 0) {
      if (!free_slot) free_slot = i;
      free_slots++;
      last_free = i;
    }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "pack archive table count=%u capacity=%u free_slot=%u records=%u archives=%d",
                     (unsigned)count, (unsigned)capacity, (unsigned)free_slot, records != 0, archives);
  if (!records || free_slots < (uint32_t)archives || last_free >= capacity ||
      (uint32_t)count + (uint32_t)archives > capacity)
    return unavailable(action, "pack archive capacity gate refused");

  // Every archive is staged and gated before the first registration, so a bad archive refuses the
  // pack without registering any of it.
  for (int i = 0; i < archives; ++i)
    if (const char* refusal = stage_pack_archive(i)) {
      __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
      return unavailable(action, refusal);
    }
  // An overlay archive holds exactly its override members (TOC entries: root + one per folder of
  // its rows + one per row).
  for (int i = 0; i < archives; ++i) {
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
  for (int i = 0; i < archives; ++i)
    if (const char* failure = add_pack_archive(i)) {
      __atomic_store_n(&g_pack_fail_archive, i, __ATOMIC_RELEASE);
      return failed(action, failure);
    }
  g_rpf_index = g_rpf_indices[0];
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

// Every archive of the registered pack has its own streamable resident (members may be requested).
static int pack_archives_resident() {
  if (g_rpf_index < 0) return 0;
  for (int i = 0; i < pack_archive_count(); ++i)
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
  typedef void (*RequestObjectFn)(uint32_t, uint32_t);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_LOAD_PACK_ARCHIVE;
  if (g_rpf_index < 0) return unavailable(action, "pack is not registered");
  if (g_rpf_archive_load_attempted) return unavailable(action, "pack archive load already attempted");
  if (g_rpf_request_attempted) return unavailable(action, "pack card already requested");
  if (!texture_action_is_stable_main_thread())
    return unavailable(action, "pack phase/thread gate refused");
  if (memcmp((const void*)kRequestObject, kRequestObjectBytes, sizeof(kRequestObjectBytes)))
    return unavailable(action, "pack code anchors mismatch");
  const int archives = pack_archive_count();
  uint32_t globals[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {};
  uint32_t before[GTAV_CUSTOM_PACK_ARCHIVE_CAP] = {};
  for (int i = 0; i < archives; ++i)
    if (!(before[i] = pack_archive_flags_at(i, &globals[i])))
      return unavailable(action, "pack archive streamable handle mismatch");
  g_rpf_archive_load_attempted = 1;
  for (int i = 0; i < archives; ++i) {
    gtav_worker_klog("GTAVMenu pack archive request before");
    ((RequestObjectFn)kRequestObject)(globals[i], 0x10u);
    gtav_worker_klog("GTAVMenu pack archive request returned");
    gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                       "pack archive request a=%d global=%u flags 0x%x -> 0x%x", i,
                       (unsigned)globals[i], (unsigned)(before[i] & 0x7fffffffu),
                       (unsigned)(pack_archive_flags_at(i, nullptr) & 0x7fffffffu));
  }
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "pack archive request returned archives=%d", archives);
  return ok(action, message);
}

#if defined(__PROSPERO__) || defined(__ORBIS__)
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

#include "custom_pack_data.inc"
#include "custom_pack_world.inc"
#include "custom_pack_hide.inc"
#include "custom_pack_labels.inc"
#include "custom_pack_select.inc"
#include "custom_pack_autoload.inc"

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
      memory_ok = *(const volatile uintptr_t*)(pack + 0x58) == kFiMemoryDevice &&
                  *(const volatile uintptr_t*)(pack + 0x38) == (uintptr_t)g_rpf_memory[0];
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
  typedef void (*RemoveFn)(uint32_t);
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
  if (plain &&
      memcmp((const void*)kOverlayRemove, kOverlayRemoveBytes, sizeof(kOverlayRemoveBytes)) != 0)
    return unavailable(action, "pack override remover anchors mismatch");

  const uint32_t entries_before = *(const volatile uint16_t*)kOverlayEntries;
  __atomic_store_n(&g_overrides_reverted, 1, __ATOMIC_RELEASE);
  for (int r = 0; r < g_pack.override_count; ++r) {
    const uint32_t global = g_override_globals[r];
    *(volatile uint32_t*)(infos + (uintptr_t)global * 8u) = g_override_prev[r];
    if (g_override_remove[r]) {
      if (const uintptr_t node = pack_overlay_node(global))
        *(volatile uint32_t*)(node + 4) = g_override_dlc_old[r];
    } else {
      ((RemoveFn)kOverlayRemove)(global);
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
#undef GTAV_PACK_STUB_REASON
#endif
