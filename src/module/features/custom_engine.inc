// Source fragment of features.cpp, textually included before custom_device.inc and custom_rpf.inc.
//
// Shared engine layer of the custom-asset lanes (PPSA04264 01.010.002, live = normalized ELF address
// + 0x400000; every address is verified against the running image before use): the engine file
// device (GetDevice and the local device's vtable slots), the asset-phase / texture-registry gate
// every main-thread pack step requires, and the virtual-query / direct-memory helpers the pack lane
// uses for its never-freed buffers. Moved verbatim from custom_device.inc (P1 lane).

// Only the runtime pack lane (CUSTOM_PACKS) uses it since the P1 lane retired.
#if GTAV_MENU_ENABLE_CUSTOM_PACKS

static const uintptr_t kFiDeviceGetDevice = 0x2f24d90;  // GetDevice(path, readOnly)
static const uintptr_t kFiLocalVtable = 0x4071f80;
static const int kFiSlotOpen = 2;
static const int kFiSlotRead = 7;
static const int kFiSlotClose = 13;
static const int kFiSlotGetFileSize = 23;
static const int kFiSlotGetResourceInfo = 39;
static const int kFiSlotOpenBulk = 3;
static const int kFiSlotReadBulk = 8;
static const int kFiSlotCloseBulk = 14;
static const uint8_t kFiDeviceGetDeviceBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x18, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xb5, 0xac, 0x18, 0x01, 0x41, 0x89, 0xf6, 0xbe, 0x3a};

static const uintptr_t kTextureStore = 0x621f260;
static const uintptr_t kTextureStoreInitialized = 0x621f2f8;
static const uintptr_t kTextureAdapterCache = 0x5c85b10;
static const uintptr_t kTextureStores = 0x62271c8;
static const uintptr_t kTextureEntries = 0x6227018;
static const uintptr_t kAssetPhaseObject = 0x43433b0;
static const uintptr_t kTextureCacheAnchor = 0x355bc13;
static const uintptr_t kTextureStoreAnchor = 0x8d0e98;
static const uintptr_t kTextureRequestAnchor = 0x1aa86b2;
static const uintptr_t kAssetPhaseAnchor = 0x348e6d6;
static const uint8_t kTextureCacheAnchorBytes[7] = {0x4c, 0x8b, 0x35, 0xf6, 0x9e, 0x72, 0x02};
static const uint8_t kTextureStoreAnchorBytes[14] = {0xc6, 0x05, 0x59, 0xe4, 0x94, 0x05, 0x01,
                                                     0x48, 0x8d, 0x3d, 0xba, 0xe3, 0x94, 0x05};
static const uint8_t kTextureRequestAnchorBytes[13] = {0x48, 0x8b, 0x05, 0x0f, 0xeb, 0x77, 0x04,
                                                       0x8b, 0x0d, 0xad, 0x6b, 0x77, 0x04};
static const uint8_t kAssetPhaseAnchorBytes[13] = {0x0f, 0xbf, 0x73, 0x10, 0x0f, 0xb7, 0x43,
                                                   0x12, 0x66, 0x39, 0xc6, 0x75, 0xcd};

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

static int texture_action_is_stable_main_thread() {
#if defined(__PROSPERO__) || defined(__ORBIS__)
  char thread_name[32] = {};
  if (::scePthreadGetname(pthread_self(), thread_name) != 0) return 0;
  thread_name[sizeof(thread_name) - 1] = '\0';
  if (strcmp(thread_name, "[RAGE] Main Thread") != 0) return 0;
#else
  return 0;
#endif
  if (!texture_registry_anchors_match()) return 0;
  const int16_t current = *(const volatile int16_t*)(kAssetPhaseObject + 16);
  const int16_t requested = *(const volatile int16_t*)(kAssetPhaseObject + 18);
  const int16_t pending = *(const volatile int16_t*)(kAssetPhaseObject + 20);
  const uint16_t flags = *(const volatile uint16_t*)(kAssetPhaseObject + 22);
  const uint32_t store_base = *(const volatile uint32_t*)(kTextureStore + 8);
  const uint32_t store_index = *(const volatile uint32_t*)(kTextureStore + 12);
  const uint32_t resource_class = *(const volatile uint32_t*)(kTextureStore + 48);
  return *(const volatile uint8_t*)kTextureStoreInitialized == 1 && resource_class == 4 &&
         store_base == 108361 && store_index == 3 &&
         *(const volatile uintptr_t*)kTextureAdapterCache != 0 &&
         *(const volatile uintptr_t*)kTextureStores != 0 &&
         *(const volatile uintptr_t*)kTextureEntries != 0 && current == 2 && requested == 2 &&
         pending == -1 && flags == 0;
}


#if defined(__PROSPERO__) || defined(__ORBIS__)
typedef struct {
  void* start;
  void* end;
  int64_t offset;
  int32_t protection;
  int32_t memory_type;
  uint32_t flags;
  char name[32];
} GtavLooseVqInfo;
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
static const uintptr_t kHddStreamer = 0x5c81618;  // live; rbx at the PID 354 trap
static const uintptr_t kHddStreamerPoolArray = 0x34a8;
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

#endif

#endif
