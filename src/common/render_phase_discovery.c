#include "gtavmenu/render_phase_discovery.h"

#include <string.h>

#define MAX_GROUPS 128u
#define MAX_NODES 4096u
#define MAX_DEPTH 32u

typedef struct DiscoveryWalk {
  const GtavRenderPhaseProfile* profile;
  GtavRenderPhaseRead read;
  void* context;
  GtavRenderPhaseDiscovery* result;
  uintptr_t groups[MAX_GROUPS];
  uintptr_t nodes[MAX_NODES];
} DiscoveryWalk;

static uint32_t rd32(const uint8_t* p) {
  uint32_t value;
  memcpy(&value, p, sizeof(value));
  return value;
}

static uint64_t rd64(const uint8_t* p) {
  uint64_t value;
  memcpy(&value, p, sizeof(value));
  return value;
}

static int fail(DiscoveryWalk* walk, uint32_t error) {
  walk->result->error = error;
  return -1;
}

static int plausible_pointer(uintptr_t address) {
  return address >= 0x100000000ull && address < 0x8000000000000ull && !(address & 7u);
}

static int read_exact(DiscoveryWalk* walk, uintptr_t address, void* output, size_t size) {
  if (walk->read(walk->context, address, output, size) != 0)
    return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_READ);
  return 0;
}

static int seen(const uintptr_t* values, uint32_t count, uintptr_t address) {
  for (uint32_t i = 0; i < count; ++i)
    if (values[i] == address) return 1;
  return 0;
}

static int walk_nodes(DiscoveryWalk* walk, uintptr_t address, uint32_t group_id, uint32_t depth) {
  if (depth > MAX_DEPTH) return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_LIMIT);
  while (address) {
    uint8_t before[40], after[40];
    if (!plausible_pointer(address)) return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_RANGE);
    if (walk->result->nodes >= MAX_NODES)
      return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_LIMIT);
    if (seen(walk->nodes, walk->result->nodes, address))
      return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_CYCLE);
    walk->nodes[walk->result->nodes++] = address;
    if (read_exact(walk, address, before, sizeof(before)) != 0) return -1;
    const uint64_t vtable = rd64(before);
    const uint32_t task_id = rd32(before + 16);
    const uintptr_t sibling = (uintptr_t)rd64(before + 24);
    const uintptr_t payload = (uintptr_t)rd64(before + 32);
    if (vtable != walk->profile->leaf_vtable && vtable != walk->profile->group_vtable)
      return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_VTABLE);
    if (vtable == walk->profile->leaf_vtable && group_id == 2u &&
        task_id == walk->profile->task_id && payload == walk->profile->original) {
      ++walk->result->matches;
      walk->result->object = address;
      walk->result->slot = address + 32u;
    }
    if (vtable == walk->profile->group_vtable && payload &&
        walk_nodes(walk, payload, group_id, depth + 1u) != 0)
      return -1;
    if (read_exact(walk, address, after, sizeof(after)) != 0) return -1;
    if (memcmp(before, after, sizeof(before)) != 0)
      return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MUTATION);
    address = sibling;
  }
  return 0;
}

static int check_bytes(DiscoveryWalk* walk, uintptr_t address, const uint8_t* expected,
                       size_t size) {
  uint8_t actual[48];
  if (size > sizeof(actual) || read_exact(walk, address, actual, size) != 0) return -1;
  return memcmp(actual, expected, size) == 0
             ? 0
             : fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_FINGERPRINT);
}

int gtav_render_phase_discover_profile(const GtavRenderPhaseProfile* profile,
                                       GtavRenderPhaseRead read, void* context,
                                       GtavRenderPhaseDiscovery* result) {
  uint64_t leaf_pointer, group_pointer;
  DiscoveryWalk walk;
  uint64_t root_before = 0, root_after = 0;
  if (!result) return -1;
  memset(result, 0, sizeof(*result));
  if (!read || !profile || !profile->root || !profile->leaf_vtable || !profile->group_vtable ||
      !profile->original) {
    result->error = GTAV_RENDER_PHASE_DISCOVERY_ERROR_ARGUMENT;
    return -1;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!profile->fingerprints[i].address || !profile->fingerprints[i].size ||
        profile->fingerprints[i].size > sizeof(profile->fingerprints[i].bytes)) {
      result->error = GTAV_RENDER_PHASE_DISCOVERY_ERROR_ARGUMENT;
      return -1;
    }
  }
  leaf_pointer = profile->fingerprints[0].address;
  group_pointer = profile->fingerprints[1].address;
  memset(&walk, 0, sizeof(walk));
  memset(result, 0, sizeof(*result));
  walk.profile = profile;
  walk.read = read;
  walk.context = context;
  walk.result = result;
  for (size_t i = 0; i < 4; ++i) {
    const GtavRenderPhaseFingerprint* fingerprint = &profile->fingerprints[i];
    if (check_bytes(&walk, fingerprint->address, fingerprint->bytes, fingerprint->size) != 0)
      return -1;
  }
  if (check_bytes(&walk, profile->leaf_vtable + 16u, (const uint8_t*)&leaf_pointer,
                  sizeof(leaf_pointer)) != 0 ||
      check_bytes(&walk, profile->group_vtable + 16u, (const uint8_t*)&group_pointer,
                  sizeof(group_pointer)) != 0)
    return -1;
  if (read_exact(&walk, profile->root, &root_before, sizeof(root_before)) != 0) return -1;
  uintptr_t group = (uintptr_t)root_before;
  while (group) {
    uint8_t before[24], after[24];
    if (!plausible_pointer(group)) return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_RANGE);
    if (result->groups >= MAX_GROUPS) return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_LIMIT);
    if (seen(walk.groups, result->groups, group))
      return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_CYCLE);
    walk.groups[result->groups++] = group;
    if (read_exact(&walk, group, before, sizeof(before)) != 0) return -1;
    const uint32_t group_id = rd32(before);
    const uintptr_t first = (uintptr_t)rd64(before + 8);
    const uintptr_t following = (uintptr_t)rd64(before + 16);
    if (first && walk_nodes(&walk, first, group_id, 0u) != 0) return -1;
    if (read_exact(&walk, group, after, sizeof(after)) != 0) return -1;
    if (memcmp(before, after, sizeof(before)) != 0)
      return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MUTATION);
    group = following;
  }
  if (read_exact(&walk, profile->root, &root_after, sizeof(root_after)) != 0) return -1;
  if (root_before != root_after) return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MUTATION);
  if (result->matches != 1u || !result->object || !result->slot)
    return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MATCH);
  return 0;
}

int gtav_render_phase_discover(GtavRenderPhaseRead read, void* context,
                               GtavRenderPhaseDiscovery* result) {
  static const GtavRenderPhaseProfile profile = {
      GTAV_RENDER_PHASE_DISCOVERY_ROOT,
      GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE,
      GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE,
      GTAV_RENDER_PHASE_DISCOVERY_TASK_ID,
      GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL,
      {{GTAV_RENDER_PHASE_LEAF_INVOKE_ADDR,
        {GTAV_RENDER_PHASE_LEAF_INVOKE_BYTES},
        sizeof((uint8_t[]){GTAV_RENDER_PHASE_LEAF_INVOKE_BYTES})},
       {GTAV_RENDER_PHASE_GROUP_INVOKE_ADDR,
        {GTAV_RENDER_PHASE_GROUP_INVOKE_BYTES},
        sizeof((uint8_t[]){GTAV_RENDER_PHASE_GROUP_INVOKE_BYTES})},
       {GTAV_RENDER_PHASE_GROUP2_DISPATCH_ADDR,
        {GTAV_RENDER_PHASE_GROUP2_DISPATCH_BYTES},
        sizeof((uint8_t[]){GTAV_RENDER_PHASE_GROUP2_DISPATCH_BYTES})},
       {GTAV_RENDER_PHASE_REGISTRATION_ADDR,
        {GTAV_RENDER_PHASE_REGISTRATION_BYTES},
        sizeof((uint8_t[]){GTAV_RENDER_PHASE_REGISTRATION_BYTES})}}};
  return gtav_render_phase_discover_profile(&profile, read, context, result);
}
