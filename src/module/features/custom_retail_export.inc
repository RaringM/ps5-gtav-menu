// Source fragment of features.cpp, textually included after custom_pack_card.inc.
//
// EXPORT_RETAIL_FILE (retail templates from the
// user's game): the PC vehicle converter's encrypted templates, data/ai/vehiclelayouts.meta in
// update.rpf (the copy the game loads) and in common.rpf (the copy it overrides), are read through
// the game's own packfile devices (which decrypt and inflate them) and written to
// <game custom root>/exports/<name>, where tools/fetch_retail_templates.py --exports picks them
// up over FTP. Only the fixed allow-list below is exportable; no path comes from outside.
//
// Why the common.rpf packfile object and not GetDevice("common:/..."): "common:/" is a stacked
// mount (fiDevice::Mount appends; GetDevice searches the stack from the last mount down and returns
// the first device whose slot 32 finds the name). The stack is common.rpf (0x548d850), then the
// relative devices onto "update2:/common" and "update:/common". update.rpf carries its own
// common/data/ai/vehiclelayouts.meta (2,014,170 bytes), so GetDevice returns the update device: the
// bytes the game loads are update.rpf's member, not common.rpf's (1,636,939 bytes). Each row
// therefore opens its member on one pinned packfile object itself (chosen by the row's mount
// prefix), exactly as GetDevice would hand it to fiStream: "common:/data/ai/..." on the common.rpf
// object, "update:/common/data/ai/..." on the update.rpf object (0x53fa2f0, the one the
// "update:/common" relative device forwards to).
//
// Engine contract (normalized ELF + 0x400000, identical in PPSA04263 and PPSA04264; every address
// is byte-checked before use and the action fails closed on any mismatch):
//   0x1d6ba19  startup mount site: lea rbx,[0x548d850]; lea rdi,["common:/"]; Mount(rdi, rbx, 1);
//              then [0x548d8b0] (= object +0x60, the mount prefix length) = 8.
//   0x1d6d313  same for update.rpf: lea rbx,[0x53fa2f0]; lea rdi,["update:/"]; Mount(rdi, rbx, 1);
//              then [0x53fa350] (object +0x60) = 8. Its static constructor (0x193b1eb/0x193b269)
//              stores the same packfile vtable; Init(obj, "update/update.rpf") runs at 0x1d6d306.
//   0x4071950  packfile vtable: slot 2 Open(name, readOnly) 0x2f38f10, slot 7 Read(handle, buf, n)
//              0x2f39990, slot 13 Close(handle) 0x2f39820, slot 23 GetFileSize(name) 0x2f3a1a0.
//              Open/GetFileSize skip the prefix (+0x60) and look the rest up with slot 72
//              (0x2f39610). Open refuses readOnly=0, takes a handle from a 4-slot static pool (free
//              list 0x5eaa748, initialised flag 0x5eaa750; an empty initialised pool traps with int
//              0x41, so the export checks it first) and, for a compressed member, sets up an
//              inflate state through the calling thread's engine allocator (TLS): main thread only.
//              Read streams up to 0x80000 stored bytes through the parent device's ReadBulk
//              (slot 8), decrypts each chunk with key index (name hash + unpacked size) % 101 when
//              the entry is encrypted (0x2f0d410), and inflates into the caller's buffer
//              (0x2f9c060): the bytes returned are the plaintext file.
//   0x5ca9bd8  mount table (40-byte entries: +0 prefix, +0xa prefix length, +0x10 device*[],
//              +0x18 u16 device count), count u16 at 0x5ca9be0; read by GetDevice at 0x2f24f49.
// The read runs as a self-enqueued main-thread job behind the pack lane's phase/thread gate; the
// worker hashes, checks the pinned sha256, writes <name>.tmp and renames it, then notes
// "pack export <path> bytes=N sha=<16 hex>" (pack_world_note: status, klog, `menu-ctl pack-notes`).

#if GTAV_MENU_ENABLE_CUSTOM_PACKS && (defined(__PROSPERO__) || defined(__ORBIS__))

// The allow-list: the converter's encrypted templates (data/retail_templates/<target>.json rows
// with transform decrypt-inflate, in manifest order; tests/test_retail_export.py keeps them equal).
// param of EXPORT_RETAIL_FILE = row index.
struct RetailExportRow {
  const char* path;  // engine path on the row's packfile: its mount prefix + member
  const char* name;  // file under <game custom root>/exports (the member's basename)
  uint32_t bytes;    // plaintext size (GetFileSize must report exactly this)
  uint8_t sha256[32];
};
static const RetailExportRow kRetailExports[] = {
    {"common:/data/ai/vehiclelayouts.meta",
     "vehiclelayouts-common.meta",
     1636939u,
     {0x42, 0x87, 0xf7, 0xb5, 0x17, 0xa2, 0xaa, 0x70, 0x21, 0x86, 0x6a,
      0x52, 0xb5, 0xb5, 0x00, 0xcb, 0x23, 0x14, 0x72, 0xbb, 0xa1, 0xf6,
      0x64, 0x08, 0x69, 0x3c, 0x5c, 0xfb, 0xf1, 0x45, 0xe2, 0x3d}},
    {"update:/common/data/ai/vehiclelayouts.meta",
     "vehiclelayouts.meta",
     2014170u,
     {0xae, 0x78, 0x0f, 0x54, 0x70, 0xb4, 0xff, 0xd2, 0xaa, 0x86, 0x36,
      0xf7, 0x64, 0xf7, 0x0e, 0xec, 0xce, 0xbf, 0x48, 0x27, 0x42, 0xd5,
      0x9e, 0x88, 0xb0, 0x0f, 0x12, 0xec, 0x30, 0xe6, 0xc0, 0x99}},
    // The base audio game data the game loads (update.rpf's copy): convert-vehicle's retail car sounds.
    {"update:/ps5/audio/config/game.dat151.rel",
     "game.dat151.rel",
     2083440u,
     {0xd2, 0x75, 0xcf, 0xb6, 0xf4, 0xc2, 0x9f, 0x46, 0x01, 0xcc, 0x32,
      0xf3, 0x69, 0x4d, 0xe0, 0x32, 0x1f, 0xbb, 0x13, 0x2e, 0xab, 0xd8,
      0xd2, 0x3c, 0x4a, 0xa6, 0x6c, 0x07, 0xd7, 0x38, 0x1e, 0xa9}},
    {"common:/data/ai/weapons.meta",
     "weapons.meta",
     1068665u,
     {0xda, 0xc0, 0xa6, 0x31, 0xe6, 0xad, 0x69, 0xf5, 0x2d, 0xc0, 0xa8, 0xf9, 0xe4, 0x96, 0x6e, 0x51, 0xbe, 0x6b, 0x76, 0xeb, 0xab, 0x5b, 0xc2, 0x26, 0x2b, 0x2b, 0xe1, 0xc3, 0x15, 0x5c, 0xcd, 0x1b}},
    {"common:/data/ai/weaponanimations.meta",
     "weaponanimations.meta",
     791601u,
     {0x91, 0x88, 0x55, 0x30, 0x4a, 0x06, 0xec, 0xb0, 0x39, 0xa3, 0xeb, 0x5e, 0x91, 0x88, 0xc8, 0x96, 0x8b, 0x53, 0xc1, 0xf4, 0x21, 0x6b, 0xe7, 0x57, 0x80, 0x5e, 0x42, 0xcb, 0x31, 0x38, 0xfc, 0x8a}},
    {"common:/data/weaponarchetypes.meta",
     "weaponarchetypes.meta",
     14826u,
     {0x99, 0x89, 0xa2, 0xce, 0x1b, 0x5f, 0x44, 0xe0, 0x18, 0x11, 0x80, 0x06, 0x21, 0xd6, 0x71, 0x31, 0xee, 0x24, 0xd6, 0xa0, 0xb8, 0x28, 0x40, 0x63, 0x67, 0x68, 0x77, 0x77, 0xd2, 0x64, 0xcd, 0x82}},
    {"common:/data/ai/weaponcomponents.meta",
     "weaponcomponents.meta",
     44977u,
     {0x5e, 0xa6, 0xe4, 0x8e, 0x57, 0x0a, 0x6c, 0x2c, 0xdc, 0xc3, 0x5b, 0x58, 0x80, 0x3b, 0xf2, 0x0a, 0x0e, 0x86, 0x94, 0x02, 0x3a, 0xdb, 0x28, 0x66, 0x15, 0x13, 0xe9, 0x8e, 0xc2, 0x8f, 0xde, 0xdc}},
    {"update:/common/data/timecycle/timecycle_mods_4.xml",
     "timecycle_mods_4.xml",
     593109u,
     {0xbd, 0x9f, 0xe3, 0x7e, 0xb9, 0x22, 0x75, 0xea, 0xa9, 0xa1, 0x1a, 0x2b, 0xce, 0xc8, 0x61, 0xfe, 0xe8, 0x81, 0x12, 0xa1, 0xb0, 0x46, 0x58, 0x98, 0x30, 0xa7, 0xb8, 0x0e, 0x49, 0x17, 0xb1, 0xf8}},
};
static const uint32_t kRetailExportCount = sizeof(kRetailExports) / sizeof(kRetailExports[0]);
// One never-freed direct-memory buffer per process holds the largest allowed file.
static const uint64_t kRetailExportBufferBytes = 0x200000u;

static const uintptr_t kRetailCommonPackfile = 0x548d850;
static const uintptr_t kRetailPackfileVtable = 0x4071950;
static const uintptr_t kRetailCommonMountSite = 0x1d6ba19;
static const uintptr_t kRetailCommonPrefix = 0x3bd6272;  // "common:/"
static const uintptr_t kRetailUpdatePackfile = 0x53fa2f0;
static const uintptr_t kRetailUpdateMountSite = 0x1d6d313;
static const uintptr_t kRetailUpdatePrefix = 0x3bdf6ed;  // "update:/"
static const uintptr_t kRetailPackOpen = 0x2f38f10;
static const uintptr_t kRetailPackRead = 0x2f39990;
static const uintptr_t kRetailPackClose = 0x2f39820;
static const uintptr_t kRetailPackGetFileSize = 0x2f3a1a0;
static const uintptr_t kRetailPackFindEntry = 0x2f39610;
static const uintptr_t kRetailHandleFreeList = 0x5eaa748;
static const uintptr_t kRetailHandlePoolInit = 0x5eaa750;
static const uintptr_t kRetailMountTableSite = 0x2f24f49;
static const uintptr_t kRetailMountTable = 0x5ca9bd8;
static const uintptr_t kRetailMountCount = 0x5ca9be0;
static const int kRetailSlotFindEntry = 72;
static const uint8_t kRetailCommonMountSiteBytes[45] = {
    0x48, 0x8d, 0x1d, 0x30, 0x1e, 0x72, 0x03, 0x48, 0x8d, 0x3d, 0x4b, 0xa8, 0xe6, 0x01, 0xba,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xde, 0xe8, 0x1c, 0x1d, 0x1c, 0x01, 0x84, 0xc0, 0x0f,
    0x84, 0x30, 0x01, 0x00, 0x00, 0xc7, 0x05, 0x6a, 0x1e, 0x72, 0x03, 0x08, 0x00, 0x00, 0x00};
// lea rbx,[0x53fa2f0]; lea rdi,["update:/"]; Mount; test/je; three argument loads of the next
// call; mov dword [0x53fa350], 8.
static const uint8_t kRetailUpdateMountSiteBytes[71] = {
    0x48, 0x8d, 0x1d, 0xd6, 0xcf, 0x68, 0x03, 0x48, 0x8d, 0x3d, 0xcc, 0x23, 0xe7, 0x01, 0xba,
    0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xde, 0xe8, 0x22, 0x04, 0x1c, 0x01, 0x84, 0xc0, 0x0f,
    0x84, 0xa8, 0x0c, 0x00, 0x00, 0x4c, 0x8d, 0xbd, 0xd0, 0xfe, 0xff, 0xff, 0x48, 0x8d, 0x15,
    0x78, 0x8c, 0xe3, 0x01, 0x48, 0x8d, 0x0d, 0x91, 0xfa, 0x0d, 0x02, 0xbe, 0x00, 0x01, 0x00,
    0x00, 0xc7, 0x05, 0xf6, 0xcf, 0x68, 0x03, 0x08, 0x00, 0x00, 0x00};

// The packfile objects a row can name, chosen by the row path's 8-character mount prefix.
struct RetailExportDevice {
  const char* prefix;   // mount prefix (object +0x60 = 8)
  const char* label;    // archive, for refusal texts
  uintptr_t packfile;   // static packfile object (vtable kRetailPackfileVtable)
  uintptr_t prefix_at;  // the prefix string the startup mount passes
  uintptr_t mount_site;
  const uint8_t* mount_site_bytes;
  uint32_t mount_site_len;
};
static const RetailExportDevice kRetailExportDevices[] = {
    {"common:/", "common.rpf", kRetailCommonPackfile, kRetailCommonPrefix, kRetailCommonMountSite,
     kRetailCommonMountSiteBytes, sizeof(kRetailCommonMountSiteBytes)},
    {"update:/", "update.rpf", kRetailUpdatePackfile, kRetailUpdatePrefix, kRetailUpdateMountSite,
     kRetailUpdateMountSiteBytes, sizeof(kRetailUpdateMountSiteBytes)},
};

// Open through the handle-pool free-list read (prefix skip, readOnly refusal, slot 72 lookup,
// directory refusal, pool lock, free list, initialised flag, the int 0x41 empty-pool trap).
static const uint8_t kRetailPackOpenBytes[153] = {
    0x48, 0x63, 0x47, 0x60, 0x48, 0x01, 0xc6, 0xe9, 0x04, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x18, 0x4c, 0x8b, 0x2d, 0x28, 0x6b, 0x17, 0x01, 0x48, 0xc7, 0xc3, 0xff, 0xff, 0xff, 0xff, 0x49,
    0x8b, 0x45, 0x00, 0x48, 0x89, 0x45, 0xd0, 0x85, 0xd2, 0x0f, 0x84, 0x8a, 0x03, 0x00, 0x00, 0x48,
    0x8b, 0x07, 0x49, 0x89, 0xf6, 0xff, 0x90, 0x40, 0x02, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x0f, 0x84,
    0x75, 0x03, 0x00, 0x00, 0x49, 0x89, 0xc7, 0x48, 0x8b, 0x00, 0x48, 0xc1, 0xe8, 0x28, 0x3d, 0xfe,
    0xff, 0x7f, 0x00, 0x0f, 0x87, 0x60, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x38, 0x15, 0xd7, 0x02,
    0xe8, 0xab, 0x74, 0x85, 0x00, 0x48, 0x8b, 0x1d, 0xbc, 0x17, 0xf7, 0x02, 0x8b, 0x05, 0xbe, 0x17,
    0xf7, 0x02, 0x48, 0x85, 0xdb, 0x75, 0x12, 0x85, 0xc0, 0x74, 0x0e, 0xcd, 0x41, 0x48, 0xc7, 0xc3,
    0xff, 0xff, 0xff, 0xff, 0xe9, 0x24, 0x03, 0x00, 0x00};
static const uint8_t kRetailPackReadBytes[48] = {
    0xe9, 0x0b, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec,
    0x48, 0x48, 0x8b, 0x86, 0x20, 0x00, 0x08, 0x00, 0x49, 0x89, 0xd6, 0x89, 0x4d, 0xd4, 0x49, 0x89};
static const uint8_t kRetailPackCloseBytes[32] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x8d, 0x3d, 0x87, 0x0c, 0xd7,
    0x02, 0x48, 0x89, 0xf3, 0xe8, 0xf7, 0x6b, 0x85, 0x00, 0x48, 0x8b, 0x83, 0x20, 0x00, 0x08, 0x00};
// The whole GetFileSize: entry +8 (unpacked size) for a binary member, 0 when not found.
static const uint8_t kRetailPackGetFileSizeBytes[56] = {
    0x55, 0x48, 0x89, 0xe5, 0x48, 0x63, 0x47, 0x60, 0x48, 0x01, 0xc6, 0x48, 0x8b, 0x07,
    0xff, 0x90, 0x40, 0x02, 0x00, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x1b, 0x48, 0x8b, 0x10,
    0x48, 0x89, 0xc1, 0x48, 0x89, 0xd0, 0x48, 0xc1, 0xe8, 0x10, 0x25, 0xff, 0xff, 0xff,
    0x00, 0x48, 0x85, 0xd2, 0x0f, 0x49, 0x41, 0x08, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3};
// GetDevice's mount-table read: lock, u16 count at 0x5ca9be0, table pointer at 0x5ca9bd8.
static const uint8_t kRetailMountTableSiteBytes[35] = {
    0x48, 0x8d, 0x3d, 0xb0, 0x4c, 0xd8, 0x02, 0xe8, 0xdb, 0xb4, 0x86, 0x00,
    0x0f, 0xb7, 0x05, 0x84, 0x4c, 0xd8, 0x02, 0x48, 0x85, 0xc0, 0x0f, 0x84,
    0x19, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0x2d, 0x6c, 0x4c, 0xd8, 0x02};

enum {
  RETAIL_EXPORT_IDLE = 0,
  RETAIL_EXPORT_QUEUED,   // worker queued the main-thread read
  RETAIL_EXPORT_READING,  // main thread owns the buffer
  RETAIL_EXPORT_READ,     // plaintext in the buffer; the worker writes it
};
#define GTAV_RETAIL_EXPORT_TIMEOUT_TICKS 1800u
static volatile uint32_t g_retail_export_state;
static uint32_t g_retail_export_row;
static uint32_t g_retail_export_ticks;
static uint64_t g_retail_export_bytes;
static uint8_t* g_retail_export_buffer;

static const RetailExportDevice* retail_export_device(const RetailExportRow& row) {
  for (const RetailExportDevice& dev : kRetailExportDevices)
    if (memcmp(row.path, dev.prefix, 8) == 0) return &dev;
  return nullptr;
}

// Refusal text with the device named (main thread; one export runs at a time).
static char g_retail_export_why[64];

static const char* retail_export_anchors(const RetailExportDevice& dev) {
  if (memcmp((const void*)dev.mount_site, dev.mount_site_bytes, dev.mount_site_len) ||
      memcmp((const void*)dev.prefix_at, dev.prefix, 9) ||
      memcmp((const void*)kRetailPackOpen, kRetailPackOpenBytes, sizeof(kRetailPackOpenBytes)) ||
      memcmp((const void*)kRetailPackRead, kRetailPackReadBytes, sizeof(kRetailPackReadBytes)) ||
      memcmp((const void*)kRetailPackClose, kRetailPackCloseBytes, sizeof(kRetailPackCloseBytes)) ||
      memcmp((const void*)kRetailPackGetFileSize, kRetailPackGetFileSizeBytes,
             sizeof(kRetailPackGetFileSizeBytes)) ||
      memcmp((const void*)kRetailMountTableSite, kRetailMountTableSiteBytes,
             sizeof(kRetailMountTableSiteBytes)))
    return "export code anchors mismatch";
  const uintptr_t* vtable = (const uintptr_t*)kRetailPackfileVtable;
  if (vtable[kFiSlotOpen] != kRetailPackOpen || vtable[kFiSlotRead] != kRetailPackRead ||
      vtable[kFiSlotClose] != kRetailPackClose ||
      vtable[kFiSlotGetFileSize] != kRetailPackGetFileSize ||
      vtable[kRetailSlotFindEntry] != kRetailPackFindEntry)
    return "export packfile vtable mismatch";
  if (!pack_readable(dev.packfile, 0xc0) ||
      *(const volatile uintptr_t*)dev.packfile != kRetailPackfileVtable ||
      *(const volatile int32_t*)(dev.packfile + 0x60) != 8 ||
      *(const volatile uintptr_t*)(dev.packfile + 0x10) == 0 ||
      *(const volatile uintptr_t*)(dev.packfile + 0x58) == 0) {
    snprintf(g_retail_export_why, sizeof(g_retail_export_why), "export %s packfile not mounted",
             dev.label);
    return g_retail_export_why;
  }
  // The object must be on its mount's stack (the loose-file fallback mounts a relative device
  // there instead when the archive is absent).
  const uintptr_t table = *(const volatile uintptr_t*)kRetailMountTable;
  const uint32_t count = *(const volatile uint16_t*)kRetailMountCount;
  if (!table || count == 0u || count > 256u || !pack_readable(table, (size_t)count * 40u))
    return "export mount table unreadable";
  for (uint32_t i = 0; i < count; ++i) {
    const uintptr_t entry = table + (uintptr_t)i * 40u;
    const uintptr_t prefix = *(const volatile uintptr_t*)entry;
    const uint32_t devices = *(const volatile uint16_t*)(entry + 0x18);
    const uintptr_t list = *(const volatile uintptr_t*)(entry + 0x10);
    if (*(const volatile uint16_t*)(entry + 0xa) != 8u || !prefix || !pack_readable(prefix, 9) ||
        memcmp((const void*)prefix, dev.prefix, 9) != 0)
      continue;
    if (!list || devices == 0u || devices > 16u || !pack_readable(list, devices * 8u)) {
      snprintf(g_retail_export_why, sizeof(g_retail_export_why), "export %.6s mount unreadable",
               dev.prefix);
      return g_retail_export_why;
    }
    for (uint32_t d = 0; d < devices; ++d)
      if (((const volatile uintptr_t*)list)[d] == dev.packfile) return nullptr;
    snprintf(g_retail_export_why, sizeof(g_retail_export_why), "export %s is not on the %s mount",
             dev.label, dev.prefix);
    return g_retail_export_why;
  }
  snprintf(g_retail_export_why, sizeof(g_retail_export_why), "export %s mount not found",
           dev.prefix);
  return g_retail_export_why;
}

// Worker: validate the row and queue the main-thread read (EXPORT_RETAIL_FILE, param = row).
static uint32_t export_retail_file(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_EXPORT_RETAIL_FILE;
  if (!gtav_custom_game_root_available())
    return unavailable(action, GTAV_CUSTOM_DATA_UNAVAILABLE);
  if (param >= kRetailExportCount) return unavailable(action, "export row out of range");
  if (!gtav_frame_hook_is_active()) return unavailable(action, "needs the game-thread hook");
  uint32_t idle = RETAIL_EXPORT_IDLE;
  if (!__atomic_compare_exchange_n(&g_retail_export_state, &idle, (uint32_t)RETAIL_EXPORT_QUEUED,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return unavailable(action, "an export is already running");
  g_retail_export_row = param;
  g_retail_export_ticks = 0;
  if (!gtav_frame_hook_enqueue(action, param)) {
    __atomic_store_n(&g_retail_export_state, (uint32_t)RETAIL_EXPORT_IDLE, __ATOMIC_RELEASE);
    return failed(action, "main-thread job queue full");
  }
  return ok(action, "export queued root=data");
}

static uint32_t retail_export_refuse(const RetailExportRow& row, const char* reason) {
  pack_world_note("pack export FAILED %s: %s", row.path, reason);
  __atomic_store_n(&g_retail_export_state, (uint32_t)RETAIL_EXPORT_IDLE, __ATOMIC_RELEASE);
  return unavailable(GTAV_NATIVE_SHELL_ACTION_EXPORT_RETAIL_FILE, reason);
}

// Main thread (run_gated_action): read the row's plaintext through its pinned packfile.
static uint32_t retail_export_read(uint32_t param) {
  const uint32_t action = GTAV_NATIVE_SHELL_ACTION_EXPORT_RETAIL_FILE;
  uint32_t queued = RETAIL_EXPORT_QUEUED;
  if (param >= kRetailExportCount || param != g_retail_export_row ||
      !__atomic_compare_exchange_n(&g_retail_export_state, &queued, (uint32_t)RETAIL_EXPORT_READING,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return unavailable(action, "no export queued");
  const RetailExportRow& row = kRetailExports[param];
  if (!texture_action_is_stable_main_thread())
    return retail_export_refuse(row, "pack phase/thread gate refused");
  const RetailExportDevice* dev = retail_export_device(row);
  if (!dev) return retail_export_refuse(row, "export row has no pinned packfile");
  if (const char* why = retail_export_anchors(*dev)) return retail_export_refuse(row, why);
  if (row.bytes == 0u || row.bytes > kRetailExportBufferBytes)
    return retail_export_refuse(row, "export row size outside the buffer");
  // Open traps (int 0x41) on an empty, initialised handle pool instead of failing.
  if (*(const volatile uintptr_t*)kRetailHandleFreeList == 0 &&
      *(const volatile uint32_t*)kRetailHandlePoolInit != 0)
    return retail_export_refuse(row, "packfile handle pool is empty");
  void* device = (void*)dev->packfile;
  const uintptr_t* vtable = *(const uintptr_t**)device;
  const uint64_t size = ((PackGetFileSizeFn)vtable[kFiSlotGetFileSize])(device, row.path);
  if (size != row.bytes) {
    char why[64];
    snprintf(why, sizeof(why), "size %llu, the allow-list pins %u", (unsigned long long)size,
             (unsigned)row.bytes);
    return retail_export_refuse(row, why);
  }
  if (!g_retail_export_buffer) {
    if (const char* why = pack_map_direct(kRetailExportBufferBytes, &g_retail_export_buffer))
      return retail_export_refuse(row, why);
  }
  const uint64_t handle = ((PackOpenFn)vtable[kFiSlotOpen])(device, row.path, true);
  if (handle == UINT64_MAX) return retail_export_refuse(row, "packfile open failed");
  uint64_t done = 0;
  while (done < size) {
    const uint64_t left = size - done;
    const int32_t got = ((PackReadFn)vtable[kFiSlotRead])(
        device, handle, g_retail_export_buffer + done, left < 0x10000u ? (uint32_t)left : 0x10000u);
    if (got <= 0) break;
    done += (uint64_t)got;
  }
  ((PackCloseFn)vtable[kFiSlotClose])(device, handle);
  if (done != size) {
    char why[64];
    snprintf(why, sizeof(why), "short read %llu of %llu", (unsigned long long)done,
             (unsigned long long)size);
    return retail_export_refuse(row, why);
  }
  g_retail_export_bytes = done;
  __atomic_store_n(&g_retail_export_state, (uint32_t)RETAIL_EXPORT_READ, __ATOMIC_RELEASE);
  char message[GTAV_FEATURE_MESSAGE_LEN];
  snprintf(message, sizeof(message), "export read %s bytes=%llu", row.name,
           (unsigned long long)done);
  return ok(action, message);
}

// Worker tick: hash, check the pin, write <name>.tmp and rename it; or time out a lost job.
static void retail_export_tick() {
  const uint32_t state = __atomic_load_n(&g_retail_export_state, __ATOMIC_ACQUIRE);
  const RetailExportRow& row = kRetailExports[g_retail_export_row % kRetailExportCount];
  if (state == RETAIL_EXPORT_QUEUED && ++g_retail_export_ticks > GTAV_RETAIL_EXPORT_TIMEOUT_TICKS) {
    uint32_t queued = RETAIL_EXPORT_QUEUED;
    if (__atomic_compare_exchange_n(&g_retail_export_state, &queued, (uint32_t)RETAIL_EXPORT_IDLE,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      pack_world_note("pack export FAILED %s: the main-thread read never ran", row.path);
    return;
  }
  if (state != RETAIL_EXPORT_READ) return;
  GtavSha256 sha;
  uint8_t digest[32];
  gtav_sha256_init(&sha);
  gtav_sha256_update(&sha, g_retail_export_buffer, (size_t)g_retail_export_bytes);
  gtav_sha256_final(&sha, digest);
  char hex[17];
  for (int i = 0; i < 8; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  const unsigned long long bytes = (unsigned long long)g_retail_export_bytes;
  if (memcmp(digest, row.sha256, sizeof(digest)) != 0) {
    pack_world_note("pack export FAILED %s: bytes=%llu sha=%s differs from the pin", row.path,
                    bytes, hex);
  } else {
    char root[GTAV_PACK_PATH_MAX];
    char path[GTAV_PACK_PATH_MAX];
    char temp[GTAV_PACK_PATH_MAX + 8];
    snprintf(root, sizeof(root), "%s/exports", gtav_custom_game_root());
    snprintf(path, sizeof(path), "%s/%s", root, row.name);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (!gtav_custom_game_root_available()) {
      pack_world_note("pack export FAILED %s: shared /data unavailable", row.path);
      __atomic_store_n(&g_retail_export_state, (uint32_t)RETAIL_EXPORT_IDLE, __ATOMIC_RELEASE);
      return;
    }
    mkdir(root, 0777);
    FILE* fp = fopen(temp, "wb");
    int bad = !fp;
    if (fp) {
      bad = fwrite(g_retail_export_buffer, 1, (size_t)g_retail_export_bytes, fp) !=
            (size_t)g_retail_export_bytes;
      if (fflush(fp) != 0 || ferror(fp)) bad = 1;
      if (fclose(fp) != 0) bad = 1;
    }
    const int error = bad ? errno : (rename(temp, path) == 0 ? 0 : errno);
    if (bad || error) {
      unlink(temp);
      pack_world_note("pack export FAILED %s: write %s errno=%d", row.path, path, error);
    } else {
      pack_world_note("pack export %s bytes=%llu sha=%s row=%u", row.path, bytes, hex,
                      g_retail_export_row);
    }
  }
  __atomic_store_n(&g_retail_export_state, (uint32_t)RETAIL_EXPORT_IDLE, __ATOMIC_RELEASE);
}

#else

static uint32_t export_retail_file(uint32_t param) {
  (void)param;
  return unavailable(GTAV_NATIVE_SHELL_ACTION_EXPORT_RETAIL_FILE,
                     "retail export needs the PS5 CUSTOM_PACKS build");
}

[[maybe_unused]] static uint32_t retail_export_read(uint32_t param) {
  return export_retail_file(param);
}

static void retail_export_tick() {}

#endif
