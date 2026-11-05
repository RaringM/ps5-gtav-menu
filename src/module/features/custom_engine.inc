// Source fragment of features.cpp, textually included before custom_device.inc and custom_rpf.inc.
//
// Shared engine layer of the custom-asset lanes (PPSA04264 01.010.002, live = normalized ELF address
// + 0x400000; every address is verified against the running image before use): the engine file
// device (GetDevice and the local device's vtable slots), the asset-phase / texture-registry gate
// every main-thread pack step requires, and the virtual-query / direct-memory helpers the pack lane
// uses for its never-freed buffers. Moved verbatim from custom_device.inc (P1 lane).

// Only the runtime pack lane (CUSTOM_PACKS) uses it since the P1 lane retired.
#if GTAV_MENU_ENABLE_CUSTOM_PACKS && GTAV_MENU_CUSTOM_PACK_ENGINE

#if defined(__ORBIS__)
#define PACK_STORE_SLOTS_OFFSET 0x38u
#define PACK_STORE_FLAGS_OFFSET 0x40u
#define PACK_STORE_SIZE_OFFSET 0x48u
#define PACK_STORE_STRIDE_OFFSET 0x4cu
#define PACK_STORE_FIRST_FREE_OFFSET 0x50u
#define PACK_STORE_USED_OFFSET 0x58u
#define PACK_STORE_MAP_NODES_OFFSET 0x70u
#define PACK_STORE_MAP_HEADS_OFFSET 0x78u
#define PACK_STORE_MAP_COUNT_OFFSET 0x80u
#else
#define PACK_STORE_SLOTS_OFFSET 0x40u
#define PACK_STORE_FLAGS_OFFSET 0x48u
#define PACK_STORE_SIZE_OFFSET 0x50u
#define PACK_STORE_STRIDE_OFFSET 0x54u
#define PACK_STORE_FIRST_FREE_OFFSET 0x58u
#define PACK_STORE_USED_OFFSET 0x60u
#define PACK_STORE_MAP_NODES_OFFSET 0x78u
#define PACK_STORE_MAP_HEADS_OFFSET 0x80u
#define PACK_STORE_MAP_COUNT_OFFSET 0x88u
#endif

static const uintptr_t kFiDeviceGetDevice = GTAV_LANE_kFiDeviceGetDevice;  // GetDevice(path, readOnly)
static const uintptr_t kFiLocalVtable = GTAV_LANE_kFiLocalVtable;
static const int kFiSlotOpen = 2;
static const int kFiSlotRead = 7;
static const int kFiSlotClose = 13;
static const int kFiSlotGetFileSize = 23;
static const int kFiSlotGetResourceInfo = 39;
static const int kFiSlotOpenBulk = 3;
static const int kFiSlotReadBulk = 8;
static const int kFiSlotCloseBulk = 14;
static const uint8_t kFiDeviceGetDeviceBytes[] = GTAV_LANE_kFiDeviceGetDeviceBytes;

static const uintptr_t kTextureStore = GTAV_LANE_kTextureStore;
static const uintptr_t kTextureStoreInitialized = GTAV_LANE_kTextureStoreInitialized;
static const uintptr_t kTextureStores = GTAV_LANE_kTextureStores;
static const uintptr_t kTextureEntries = GTAV_LANE_kTextureEntries;
static const uintptr_t kAssetPhaseObject = GTAV_LANE_kAssetPhaseObject;
// The texture-registry anchors and adapter cache, where the target's lane pins them.
#ifdef GTAV_LANE_kTextureAdapterCache
static const uintptr_t kTextureAdapterCache = GTAV_LANE_kTextureAdapterCache;
#endif
#ifdef GTAV_LANE_kTextureCacheAnchor
static const uintptr_t kTextureCacheAnchor = GTAV_LANE_kTextureCacheAnchor;
static const uintptr_t kTextureStoreAnchor = GTAV_LANE_kTextureStoreAnchor;
static const uintptr_t kTextureRequestAnchor = GTAV_LANE_kTextureRequestAnchor;
static const uintptr_t kAssetPhaseAnchor = GTAV_LANE_kAssetPhaseAnchor;
static const uint8_t kTextureCacheAnchorBytes[] = GTAV_LANE_kTextureCacheAnchorBytes;
static const uint8_t kTextureStoreAnchorBytes[] = GTAV_LANE_kTextureStoreAnchorBytes;
static const uint8_t kTextureRequestAnchorBytes[] = GTAV_LANE_kTextureRequestAnchorBytes;
static const uint8_t kAssetPhaseAnchorBytes[] = GTAV_LANE_kAssetPhaseAnchorBytes;

static int texture_registry_anchors_match() {
  return memcmp((const void*)kTextureCacheAnchor, kTextureCacheAnchorBytes,
                sizeof(kTextureCacheAnchorBytes)) == 0 &&
         memcmp((const void*)kTextureStoreAnchor, kTextureStoreAnchorBytes,
                sizeof(kTextureStoreAnchorBytes)) == 0 &&
         memcmp((const void*)kTextureRequestAnchor, kTextureRequestAnchorBytes,
                sizeof(kTextureRequestAnchorBytes)) == 0 &&
         memcmp((const void*)kAssetPhaseAnchor, kAssetPhaseAnchorBytes,
                sizeof(kAssetPhaseAnchorBytes)) == 0;
}
#endif

// Platform hooks of the engine (gtavmenu/pack_platform.h): the game-thread test and the
// memory-range query. PS5 implements both inline below; every other platform links them.
#if !defined(__PROSPERO__)
#include "gtavmenu/pack_platform.h"
#endif

static int texture_action_is_stable_main_thread() {
#if defined(__PROSPERO__)
  // The game-thread test: the engine's main thread, by name.
  char thread_name[32] = {};
  if (::scePthreadGetname(pthread_self(), thread_name) != 0) return 0;
  thread_name[sizeof(thread_name) - 1] = '\0';
  if (strcmp(thread_name, "[RAGE] Main Thread") != 0) return 0;
#else
  if (!gtav_platform_on_game_thread()) return 0;
#endif
#ifdef GTAV_LANE_kTextureCacheAnchor
  if (!texture_registry_anchors_match()) return 0;
#endif
  const int16_t current = *(const volatile int16_t*)(kAssetPhaseObject + 16);
  const int16_t requested = *(const volatile int16_t*)(kAssetPhaseObject + 18);
  const int16_t pending = *(const volatile int16_t*)(kAssetPhaseObject + 20);
  const uint16_t flags = *(const volatile uint16_t*)(kAssetPhaseObject + 22);
  const uint32_t store_base = *(const volatile uint32_t*)(kTextureStore + 8);
  const uint32_t store_index = *(const volatile uint32_t*)(kTextureStore + 12);
  const uint32_t resource_class = *(const volatile uint32_t*)(kTextureStore + 48);
  // AddModule starts each streaming range where the previous module's ends, and TxdStore follows
  // DrawableStore. A boot-time pool override that grows Dwd or Drawable moves the base, so check
  // the chain instead of a stock constant.
  const uintptr_t drawable = GTAV_LANE_kDrawableStore;
  const int drawable_ok = *(const volatile uintptr_t*)drawable == GTAV_LANE_kDrawableStoreVtable &&
                          *(const volatile uint32_t*)(drawable + 12) == store_index - 1;
  const uint32_t drawable_end =
      *(const volatile uint32_t*)(drawable + 8) + *(const volatile uint32_t*)(drawable + 16);
  return *(const volatile uint8_t*)kTextureStoreInitialized == 1 && resource_class == 4 &&
         drawable_ok && store_base == drawable_end && store_index == 3 &&
#ifdef GTAV_LANE_kTextureAdapterCache
         *(const volatile uintptr_t*)kTextureAdapterCache != 0 &&
#endif
         *(const volatile uintptr_t*)kTextureStores != 0 &&
         *(const volatile uintptr_t*)kTextureEntries != 0 && current == 2 && requested == 2 &&
         pending == -1 && flags == 0;
}

// One mapping as the pack lane reads it (the PS5 kernel's virtual-query record).
typedef struct {
  void* start;
  void* end;
  int64_t offset;
  int32_t protection;
  int32_t memory_type;
  uint32_t flags;
  char name[32];
} GtavLooseVqInfo;

#if defined(__PROSPERO__)
extern "C" int sceKernelVirtualQuery(const void* address, int flags, GtavLooseVqInfo* info,
                                      size_t info_size);
extern "C" size_t sceKernelGetDirectMemorySize(void);
extern "C" int sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len,
                                             size_t alignment, int memory_type, off_t* phys_out);
extern "C" int sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                        off_t direct_start, size_t alignment);
extern "C" int sceKernelReleaseDirectMemory(off_t start, size_t len);
extern "C" int sceKernelAvailableDirectMemorySize(off_t search_start, off_t search_end,
                                                  size_t alignment, off_t* phys_out,
                                                  size_t* size_out);
static const uintptr_t kHddStreamer = GTAV_LANE_kHddStreamer;  // live; rbx at the PID 354 trap
static const uintptr_t kHddStreamerPoolArray = GTAV_LANE_kHddStreamerPoolArray;
static int loose_query(uintptr_t address, GtavLooseVqInfo* info) {
  memset(info, 0, sizeof(*info));
  return address && sceKernelVirtualQuery((const void*)address, 0, info, sizeof(*info)) == 0;
}

// Memory type and protection of the HDD streamer's first pool buffer (read-only, bounded).
static int stream_pool_query(GtavLooseVqInfo* info) {
  if (!loose_query(kHddStreamer + kHddStreamerPoolArray, info) || !(info->protection & 1)) return 0;
  const uintptr_t array = *(const volatile uintptr_t*)(kHddStreamer + kHddStreamerPoolArray);
  GtavLooseVqInfo array_info = {};
  if (!loose_query(array, &array_info) || !(array_info.protection & 1)) return 0;
  return loose_query(*(const volatile uintptr_t*)array, info);
}

#else
// The platform's readable run around `address`; pack_readable then bounds each individual read
// within it.
static int loose_query(uintptr_t address, GtavLooseVqInfo* info) {
  memset(info, 0, sizeof(*info));
  uintptr_t start = 0, end = 0;
  if (gtav_platform_mapped_range(address, &start, &end) != 0) return 0;
  info->start = (void*)start;
  info->end = (void*)end;
  info->protection = 1;
  return 1;
}
#endif

#elif GTAV_MENU_ENABLE_CUSTOM_PACKS

// Engine-less lane (GTAV_MENU_CUSTOM_PACK_ENGINE=0): no engine object to check. Every main-thread
// pack step reaches its handler only through the frame-hook drain (gtav_features_run_job), so the
// gate is that the drain is live. On PS4 the whole menu runs in that game-thread hook.
static int texture_action_is_stable_main_thread() {
  return gtav_frame_hook_is_active();
}

#endif
