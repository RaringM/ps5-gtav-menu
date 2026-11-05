#pragma once

// Boot-time pool growth: per-target profiles and the pure helpers around the boot-pool stub
// (boot_pool_stub.h). The loader applies a profile once per game instance, between process start
// and CGame_LoadGameConfig, through src/payload_loader/boot_pool.c. Profiles are generated from
// data/boot_pools (pins per target, one shared growth policy): into
// boot_pool_profile_<target>_generated.h for a single-target loader, and into the universal
// loader's registry table.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gtavmenu/boot_pool_stub.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavBootPoolOverride {
  const char* name;
  uint8_t kind;  // GTAV_BOOT_POOL_KIND_*
  uint32_t key;  // joaat config key (node) or byte offset into the parsed config (cfg)
  int32_t stock;
  int32_t new_value;
} GtavBootPoolOverride;

typedef struct GtavBootPoolProfile {
  const char* target_id;  // GtavBuildPin.target_id of the build these pins describe
  int enabled;            // manifest build.features.bootPools
  uint64_t cave_address;
  uint32_t cave_capacity;
  uint32_t cave_page_mask;
  uint64_t call_site;  // `call CGame_LoadGameConfig` in CGame_Init
  uint64_t load_game_config;
  uint64_t config_manager;    // the g_ConfigManager global pointer
  uint64_t config_done_flag;  // u8, set once the gameconfig parse has finished
  uint8_t call_bytes[5];
  uint8_t call_context[16];
  const GtavBootPoolOverride* overrides;
  uint32_t override_count;
} GtavBootPoolProfile;

enum {
  GTAV_BOOT_POOL_APPLIED = 0,  // the stub ran; per-entry results in the report; code restored
  GTAV_BOOT_POOL_MISSED = 1,   // the parse had already run, or never reached the stub; no change
  GTAV_BOOT_POOL_REFUSED = 2,  // an identity, cave or table check failed before the detour
  GTAV_BOOT_POOL_UNSAFE = -1,  // the call site or cave could not be restored: do not inject
};

typedef struct GtavBootPoolReport {
  int status;
  uint32_t count;
  uint32_t written;
  uint32_t waited_ms;
  uint8_t result[GTAV_BOOT_POOL_ENTRIES_MAX];
  int32_t old_value[GTAV_BOOT_POOL_ENTRIES_MAX];
} GtavBootPoolReport;

static inline const char* gtav_boot_pool_status_name(int status) {
  switch (status) {
    case GTAV_BOOT_POOL_APPLIED:
      return "applied";
    case GTAV_BOOT_POOL_MISSED:
      return "missed";
    case GTAV_BOOT_POOL_REFUSED:
      return "refused";
    case GTAV_BOOT_POOL_UNSAFE:
      return "unsafe";
    default:
      return "unknown";
  }
}

static inline const GtavBootPoolProfile* gtav_boot_pool_find(const GtavBootPoolProfile* profiles,
                                                             size_t count, const char* target_id) {
  size_t i;
  if (!profiles || !target_id) return NULL;
  for (i = 0; i < count; ++i)
    if (profiles[i].target_id && strcmp(profiles[i].target_id, target_id) == 0) return &profiles[i];
  return NULL;
}

// The stub's entry table for `profile`. Only growth is accepted. Returns the entry count, or 0 when
// the profile is unusable.
static inline uint32_t gtav_boot_pool_entries(const GtavBootPoolProfile* profile,
                                              GtavBootPoolEntry* out, uint32_t cap) {
  uint32_t i;
  if (!profile || !out || !profile->overrides || profile->override_count == 0 ||
      profile->override_count > cap || profile->override_count > GTAV_BOOT_POOL_ENTRIES_MAX)
    return 0;
  for (i = 0; i < profile->override_count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    if (o->stock <= 0 || o->new_value <= o->stock) return 0;
    out[i].kind = o->kind;
    out[i].key = o->key;
    out[i].expected_old = o->stock;
    out[i].new_value = o->new_value;
  }
  return profile->override_count;
}

// One log line: "status=applied written=8/8 waited=483ms metadata=3200>4200
// ptfx=401!unexpected-old". Returns the number of characters written (truncated to fit `cap`).
static inline size_t gtav_boot_pool_format(const GtavBootPoolProfile* profile,
                                           const GtavBootPoolReport* report, char* out,
                                           size_t cap) {
  size_t n = 0;
  uint32_t i;
  int w;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!profile || !report) return 0;
  w = snprintf(out, cap, "status=%s written=%u/%u waited=%ums",
               gtav_boot_pool_status_name(report->status), report->written, report->count,
               report->waited_ms);
  if (w < 0) return 0;
  n = (size_t)w < cap ? (size_t)w : cap - 1;
  for (i = 0; i < report->count && i < profile->override_count && n + 1 < cap; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    const uint8_t result = report->result[i];
    if (result == GTAV_BOOT_POOL_RESULT_WRITTEN) {
      w = snprintf(out + n, cap - n, " %s=%d>%d", o->name, report->old_value[i], o->new_value);
    } else if (result == GTAV_BOOT_POOL_RESULT_UNEXPECTED_OLD) {
      w = snprintf(out + n, cap - n, " %s=%d!%s", o->name, report->old_value[i],
                   gtav_boot_pool_result_name(result));
    } else {
      w = snprintf(out + n, cap - n, " %s!%s", o->name, gtav_boot_pool_result_name(result));
    }
    if (w < 0) break;
    n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
  }
  return n;
}

// What the stub waits ask once a second (both optional): `stop` = the daemon was asked to stop,
// `live` = `pid` is still the game. A stop before the game thread entered the stub restores and
// clears as usual; a stop while it is still inside leaves the cave in place (unsafe) instead of
// waiting up to 10 minutes; a target that is gone ends the wait at once (nothing left to restore).
typedef struct GtavBootPoolHooks {
  int (*stop)(void* context);
  int (*live)(void* context, int pid);
  void* context;
} GtavBootPoolHooks;

// The loader's side: install the stub for one fresh game instance, wait for the game thread to run
// it, restore the call site and cave, and report. Never throws away a cave the game may still be
// executing. `hooks` may be NULL. Returns report->status.
int gtav_boot_pool_apply(int pid, const GtavBootPoolProfile* profile, GtavBootPoolReport* report,
                         const GtavBootPoolHooks* hooks);

#ifdef __cplusplus
}
#endif
