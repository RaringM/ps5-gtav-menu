#pragma once

// Stable console-side namespace for user-authored content; maps are written below MAP_ROOT.
#define GTAV_CUSTOM_ASSET_ROOT "/data/GTAVMenu/custom"
#define GTAV_CUSTOM_MAP_ROOT GTAV_CUSTOM_ASSET_ROOT "/maps"
#define GTAV_CUSTOM_MAP_PATH GTAV_CUSTOM_MAP_ROOT "/active.map.cfg"

// Custom content uses the console /data exposed by ShadowMountPlus or the HEN.
// The loader never mounts or copies packs into the game sandbox. If /data is
// unavailable, custom content is disabled while the ordinary menu remains usable.
#define GTAV_CUSTOM_GAME_ROOT "/data/gtavmenu/custom"
#define GTAV_CUSTOM_GAME_PACK_ROOT GTAV_CUSTOM_GAME_ROOT "/packs"
#define GTAV_PACK_NOTES_NAME "pack-notes.log"
#define GTAV_CUSTOM_DATA_UNAVAILABLE \
  "Custom packs unavailable: enable SMP/HEN shared /data, then restart GTA"
#define GTAV_CUSTOM_GAME_MAP_ROOT GTAV_CUSTOM_GAME_ROOT "/maps"
#define GTAV_CUSTOM_GAME_MAP_PATH GTAV_CUSTOM_GAME_MAP_ROOT "/active.map.cfg"

// Read-only compatibility path for maps uploaded by older builds.
#define GTAV_LEGACY_MAP_PATH "/data/GTAVMenu/map.cfg"

#ifdef __cplusplus
extern "C" {
#endif

// Sole custom-content root. Reading it performs no I/O.
const char* gtav_custom_game_root(void);
// Worker-thread initialization only: probe /data accessibility once per process.
// Readable control files count even when the HEN's directory listing is empty.
void gtav_custom_game_root_init(void);
// Cached predicate, safe for game-thread callers. Unknown/unavailable returns 0.
// An empty but accessible /data is available; a missing pack list is a separate state.
int gtav_custom_game_root_available(void);

#ifdef __cplusplus
}
#endif
