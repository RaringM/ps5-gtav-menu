#pragma once

#include "gtavmenu/build_pin.h"
#include "gtavmenu/render_phase_discovery.h"

#include <string.h>

// One reviewed target and its separately built worker. The loader binds this whole
// contract to an exact process instance; individual addresses are never mixed.
typedef struct GtavLoaderTargetProfile {
  const char* title_id;
  GtavBuildPin pin;
  uintptr_t cave_address;
  uint32_t cave_allocation;
  int custom_packs;
  GtavRenderPhaseProfile render;
  uintptr_t cycle_addresses[8];
  const uint8_t* worker_start;
  const uint8_t* worker_end;
  const char* worker_sha256;
} GtavLoaderTargetProfile;

typedef int (*GtavLoaderProfileProbe)(void* context, const GtavLoaderTargetProfile* profile);

enum {
  GTAV_LOADER_SELECT_OK = 0,
  GTAV_LOADER_SELECT_ERROR = -1,
  GTAV_LOADER_SELECT_UNSUPPORTED = -2,
  GTAV_LOADER_SELECT_AMBIGUOUS = -3,
};

// Probe only rows for the foreground title. Every exact match participates in the
// ballot: identical US/EU signatures cannot cross titles and duplicate matches
// never silently choose the first worker. probe returns 1/0/-1 for match/miss/error.
static inline int gtav_loader_select_profile(const GtavLoaderTargetProfile* profiles, size_t count,
                                             const char* title_id, GtavLoaderProfileProbe probe,
                                             void* context,
                                             const GtavLoaderTargetProfile** selected) {
  const GtavLoaderTargetProfile* match = NULL;
  if (!selected) return GTAV_LOADER_SELECT_ERROR;
  *selected = NULL;
  if (!profiles || !count || !title_id || !*title_id || !probe) return GTAV_LOADER_SELECT_ERROR;
  for (size_t i = 0; i < count; ++i) {
    const GtavLoaderTargetProfile* row = &profiles[i];
    if (!row->title_id || strcmp(row->title_id, title_id) != 0) continue;
    const int result = probe(context, row);
    if (result < 0) return GTAV_LOADER_SELECT_ERROR;
    if (result == 0) continue;
    if (match) return GTAV_LOADER_SELECT_AMBIGUOUS;
    match = row;
  }
  if (!match) return GTAV_LOADER_SELECT_UNSUPPORTED;
  *selected = match;
  return GTAV_LOADER_SELECT_OK;
}
