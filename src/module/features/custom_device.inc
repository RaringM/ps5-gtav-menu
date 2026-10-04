// Source fragment of features.cpp, textually included after spooner.inc.
//
// Experimental (GTAV_MENU_ENABLE_CUSTOM_DEVICE): mount the loader's sandbox mount
// (GTAV_CUSTOM_GAME_ROOT) as the engine device prefix gtavmenu:/ by reusing the game's own
// relative-device class, exactly as the game mounts dlcpacks:/ at startup. See
// research/docs/asset-mount-design.md. Game-thread job; mounts at most once per process.

#if GTAV_MENU_ENABLE_CUSTOM_DEVICE

// Live addresses for PPSA04264 01.010.002 (normalized ELF address + 0x400000). Every one is
// verified against the running image before use, so another build refuses instead of jumping.
static const uintptr_t kFiDeviceMount = 0x2f2d750;      // Mount(prefix, device&, readOnly)
static const uintptr_t kFiDeviceGetDevice = 0x2f24d90;  // GetDevice(path, readOnly)
static const uintptr_t kFiRelativeVtable = 0x4071bb0;
static const uintptr_t kFiRelativeOpen = 0x2f2d0e0;         // vtable slot 2
static const uintptr_t kFiRelativeGetFileSize = 0x2f32260;  // vtable slot 23
static const int kFiSlotOpen = 2;
static const int kFiSlotGetFileSize = 23;
static const uint8_t kFiDeviceMountBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x68, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x3d, 0xf5, 0x22, 0x18, 0x01, 0x49, 0x89, 0xf4, 0x48, 0x8d};
static const uint8_t kFiDeviceGetDeviceBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x18, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xb5, 0xac, 0x18, 0x01, 0x41, 0x89, 0xf6, 0xbe, 0x3a};

#define GTAV_CUSTOM_DEVICE_PREFIX "gtavmenu:/"
// The engine strips this many characters ("gtavmenu:") and appends the rest to path.
#define GTAV_CUSTOM_DEVICE_PREFIX_LEN 9

// Layout of the game's relative device (vtable, parent, path[0x100], prefix length, read-only).
struct GtavFiDeviceRelative {
  uintptr_t vtable;
  void* parent;
  char path[0x100];
  int32_t prefix_len;
  uint8_t read_only;
  uint8_t reserved[3];
};
static_assert(sizeof(GtavFiDeviceRelative) == 0x118, "relative device layout");

// Process lifetime: the engine keeps this pointer in its mount table, and the worker image is
// never unmapped while GTA runs.
static GtavFiDeviceRelative g_custom_device;
static int g_custom_device_mounted;

static int custom_device_code_matches() {
  const uintptr_t* vtable = (const uintptr_t*)kFiRelativeVtable;
  return memcmp((const void*)kFiDeviceMount, kFiDeviceMountBytes, sizeof(kFiDeviceMountBytes)) ==
             0 &&
         memcmp((const void*)kFiDeviceGetDevice, kFiDeviceGetDeviceBytes,
                sizeof(kFiDeviceGetDeviceBytes)) == 0 &&
         vtable[kFiSlotOpen] == kFiRelativeOpen &&
         vtable[kFiSlotGetFileSize] == kFiRelativeGetFileSize;
}

static uint32_t mount_custom_device() {
  typedef void* (*GetDeviceFn)(const char*, bool);
  typedef bool (*MountFn)(const char*, void*, bool);
  typedef uint64_t (*GetFileSizeFn)(void*, const char*);
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_MOUNT_CUSTOM_DEVICE;
  const GetDeviceFn get_device = (GetDeviceFn)kFiDeviceGetDevice;

  if (!custom_device_code_matches()) return unavailable(action, "engine device code mismatch");
  if (!g_custom_device_mounted) {
    void* parent = get_device(GTAV_CUSTOM_GAME_ROOT "/", true);
    if (!parent) return failed(action, "no parent device for " GTAV_CUSTOM_GAME_ROOT);
    memset(&g_custom_device, 0, sizeof(g_custom_device));
    g_custom_device.vtable = kFiRelativeVtable;
    g_custom_device.parent = parent;
    snprintf(g_custom_device.path, sizeof(g_custom_device.path), "%s", GTAV_CUSTOM_GAME_ROOT);
    g_custom_device.prefix_len = GTAV_CUSTOM_DEVICE_PREFIX_LEN;
    g_custom_device.read_only = 1;
    if (!((MountFn)kFiDeviceMount)(GTAV_CUSTOM_DEVICE_PREFIX, &g_custom_device, true))
      return failed(action, "engine refused gtavmenu:/ mount");
    // Never mount twice: the engine's mount table is fixed-size and not idempotent.
    g_custom_device_mounted = 1;
  }

  const char* probe = GTAV_CUSTOM_DEVICE_PREFIX GTAV_CUSTOM_PROBE_NAME;
  void* found = get_device(probe, true);
  uint64_t size = 0;
  if (found == &g_custom_device) {
    size = ((GetFileSizeFn)((const uintptr_t*)kFiRelativeVtable)[kFiSlotGetFileSize])(found, probe);
  }
  gtav_status_eventf(GTAV_MENU_EVENT_NATIVE_SHELL,
                     "custom device %s lookup=%s size=%llu parent=%p", probe,
                     found == &g_custom_device ? "ours" : (found ? "other" : "none"),
                     (unsigned long long)size, g_custom_device.parent);
  char msg[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(msg, sizeof(msg), "gtavmenu:/ lookup=%d size=%llu", found == &g_custom_device ? 1 : 0,
           (unsigned long long)size);
  if (found != &g_custom_device || size == 0) return failed(action, msg);
  return ok(action, msg);
}

#else

static uint32_t mount_custom_device() {
  return unavailable(GTAV_NATIVE_SHELL_ACTION_MOUNT_CUSTOM_DEVICE,
                     "custom device not built (CUSTOM_DEVICE=1)");
}

#endif
