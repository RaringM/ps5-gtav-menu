#pragma once

#include <stdint.h>

// The only custom-content root: the console /data that ShadowMountPlus or the HEN shares with GTA.
// Packs, saved and uploaded maps, exports and pack notes all live below it. The console filesystem
// is case-sensitive and this mixed-case directory is the one the console and the host tools use;
// never spell it another way. The loader never mounts or copies packs into the game sandbox. If
// /data is unavailable, custom content is disabled while the ordinary menu remains usable.
#define GTAV_CUSTOM_GAME_ROOT "/data/GTAVMenu/custom"
#define GTAV_CUSTOM_MAP_ROOT GTAV_CUSTOM_GAME_ROOT "/maps"
#define GTAV_CUSTOM_MAP_PATH GTAV_CUSTOM_MAP_ROOT "/active.map.cfg"
#define GTAV_PACK_NOTES_NAME "pack-notes.log"
// At most 75 characters: it is also a feature result message and a toast, which prepends a
// 4-character status glyph into an 80-byte buffer. A platform whose /data arrives another way (the
// PS4 host grants it at injection) overrides the hint.
#ifndef GTAV_CUSTOM_DATA_UNAVAILABLE
#define GTAV_CUSTOM_DATA_UNAVAILABLE \
  "Custom packs unavailable: share /data via ShadowMountPlus/HEN, restart GTA"
#endif
// The two Custom Packs info rows under that refusal: how to get /data, then what to do next.
#ifndef GTAV_CUSTOM_DATA_FIX_ROW
#define GTAV_CUSTOM_DATA_FIX_ROW "Enable shared /data (SMP/HEN)"
#endif
#ifndef GTAV_CUSTOM_DATA_RETRY_ROW
#define GTAV_CUSTOM_DATA_RETRY_ROW "Then restart GTA"
#endif

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
// Engine-less pack lane only (GTAV_MENU_CUSTOM_PACK_ENGINE=0): the platform's whole-file read of
// `path` into `buffer`. The byte count, or -1 when the file is missing, unreadable or larger than
// `capacity`. PS4: gtav_ps4_read_file.
int64_t gtav_custom_read_file(const char* path, void* buffer, uint64_t capacity);

#ifdef __cplusplus
}
#endif
