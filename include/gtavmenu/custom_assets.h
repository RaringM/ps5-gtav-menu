#pragma once

// Stable console-side namespace for user-authored content. Converted resource packs are staged
// below PACK_ROOT and remain inactive until the target's mount/lifetime contract is qualified.
#define GTAV_CUSTOM_ASSET_ROOT "/data/GTAVMenu/custom"
#define GTAV_CUSTOM_PACK_ROOT GTAV_CUSTOM_ASSET_ROOT "/packs"
#define GTAV_CUSTOM_CATALOG_PATH GTAV_CUSTOM_ASSET_ROOT "/catalog.json"
#define GTAV_CUSTOM_CATALOG_TEMP_PATH GTAV_CUSTOM_ASSET_ROOT "/catalog.json.tmp"
#define GTAV_CUSTOM_AUTHORED_BC1_PACK_ID "gtavmenu-authored-bc1-v1"
#define GTAV_CUSTOM_AUTHORED_BC1_PACK_ROOT \
  GTAV_CUSTOM_PACK_ROOT "/" GTAV_CUSTOM_AUTHORED_BC1_PACK_ID
#define GTAV_CUSTOM_AUTHORED_BC1_MANIFEST_PATH GTAV_CUSTOM_AUTHORED_BC1_PACK_ROOT "/manifest.json"
#define GTAV_CUSTOM_AUTHORED_BC1_RESOURCE_PATH \
  GTAV_CUSTOM_AUTHORED_BC1_PACK_ROOT "/resources/gtavmenu_authored_bc1.ptd"
#define GTAV_CUSTOM_AUTHORED_BC1_DICTIONARY "gtavmenu_authored_bc1"
#define GTAV_CUSTOM_AUTHORED_BC1_TEXTURE "gtavmenu_authored_bc1"
#define GTAV_CUSTOM_MAP_ROOT GTAV_CUSTOM_ASSET_ROOT "/maps"
#define GTAV_CUSTOM_MAP_PATH GTAV_CUSTOM_MAP_ROOT "/active.map.cfg"
#define GTAV_CUSTOM_MAP_TEMP_PATH GTAV_CUSTOM_MAP_ROOT "/active.map.cfg.tmp"

// GTA runs chrooted to /mnt/sandbox/<TITLE>_<NNN> and cannot see /data. When built with
// GTAV_PAYLOAD_CUSTOM_MOUNT, the loader nullfs-mounts GTAV_CUSTOM_ASSET_ROOT read-only at
// <sandbox>GTAV_CUSTOM_GAME_ROOT, so the game sees the custom root at the console's own path. It
// must stay under /data/: the engine's local device passes /data/ paths through unchanged but
// rewrites unknown absolute roots to /host/.
#define GTAV_CUSTOM_GAME_ROOT GTAV_CUSTOM_ASSET_ROOT
// Mountpoint name used by earlier builds (<sandbox>/gtavmenu); the loader removes it when found.
#define GTAV_CUSTOM_LEGACY_MOUNT_NAME "gtavmenu"

// Small text file that reports whether the custom root is visible from inside GTA.
#define GTAV_CUSTOM_PROBE_NAME "probe.txt"
#define GTAV_CUSTOM_PROBE_PATH GTAV_CUSTOM_ASSET_ROOT "/" GTAV_CUSTOM_PROBE_NAME

// Read-only compatibility path for maps uploaded by older builds.
#define GTAV_LEGACY_MAP_PATH "/data/GTAVMenu/map.cfg"
