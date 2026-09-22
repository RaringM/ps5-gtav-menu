#pragma once

// Read-only discovery of the certified 01.010.002 group-2 render callback object. This is used by
// the payload loader after build binding and before it arms the injected worker's in-process CAS
// installer. It never writes target memory and never executes target code.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_RENDER_PHASE_DISCOVERY_ROOT 0x4356178ull
#define GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE 0x408f118ull
#define GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE 0x408f140ull
#define GTAV_RENDER_PHASE_DISCOVERY_TASK_ID 0x249760f7u
#define GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL 0x1cae9c0ull

typedef int (*GtavRenderPhaseRead)(void* context, uintptr_t address, void* output, size_t size);

typedef struct GtavRenderPhaseDiscovery {
  uintptr_t object;
  uintptr_t slot;
  uint32_t groups;
  uint32_t nodes;
  uint32_t matches;
  uint32_t error;
} GtavRenderPhaseDiscovery;

enum {
  GTAV_RENDER_PHASE_DISCOVERY_OK = 0,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_ARGUMENT = 1,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_FINGERPRINT = 2,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_READ = 3,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_RANGE = 4,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_LIMIT = 5,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_CYCLE = 6,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_MUTATION = 7,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_VTABLE = 8,
  GTAV_RENDER_PHASE_DISCOVERY_ERROR_MATCH = 9,
};

int gtav_render_phase_discover(GtavRenderPhaseRead read, void* context,
                               GtavRenderPhaseDiscovery* result);

#ifdef __cplusplus
}
#endif
