#pragma once

// Read-only discovery of a target-pinned group-2 render callback object. This is used by
// the payload loader after build binding and before it arms the injected worker's in-process CAS
// installer. It never writes target memory and never executes target code.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GTAV_RENDER_PHASE_DISCOVERY_ROOT
#define GTAV_RENDER_PHASE_DISCOVERY_ROOT 0ull
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE
#define GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE 0ull
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE
#define GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE 0ull
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_TASK_ID
#define GTAV_RENDER_PHASE_DISCOVERY_TASK_ID 0u
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL
#define GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL 0ull
#endif
#ifndef GTAV_RENDER_PHASE_LEAF_INVOKE_ADDR
#define GTAV_RENDER_PHASE_LEAF_INVOKE_ADDR 0ull
#define GTAV_RENDER_PHASE_LEAF_INVOKE_BYTES 0x00
#endif
#ifndef GTAV_RENDER_PHASE_GROUP_INVOKE_ADDR
#define GTAV_RENDER_PHASE_GROUP_INVOKE_ADDR 0ull
#define GTAV_RENDER_PHASE_GROUP_INVOKE_BYTES 0x00
#endif
#ifndef GTAV_RENDER_PHASE_GROUP2_DISPATCH_ADDR
#define GTAV_RENDER_PHASE_GROUP2_DISPATCH_ADDR 0ull
#define GTAV_RENDER_PHASE_GROUP2_DISPATCH_BYTES 0x00
#endif
#ifndef GTAV_RENDER_PHASE_REGISTRATION_ADDR
#define GTAV_RENDER_PHASE_REGISTRATION_ADDR 0ull
#define GTAV_RENDER_PHASE_REGISTRATION_BYTES 0x00
#endif

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
