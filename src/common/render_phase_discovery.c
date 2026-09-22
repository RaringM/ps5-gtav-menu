#include "gtavmenu/render_phase_discovery.h"

#include <string.h>

#define MAX_GROUPS 128u
#define MAX_NODES 4096u
#define MAX_DEPTH 32u

typedef struct DiscoveryWalk {
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
    if (vtable != GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE &&
        vtable != GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE)
      return fail(walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_VTABLE);
    if (vtable == GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE && group_id == 2u &&
        task_id == GTAV_RENDER_PHASE_DISCOVERY_TASK_ID &&
        payload == GTAV_RENDER_PHASE_DISCOVERY_ORIGINAL) {
      ++walk->result->matches;
      walk->result->object = address;
      walk->result->slot = address + 32u;
    }
    if (vtable == GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE && payload &&
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

int gtav_render_phase_discover(GtavRenderPhaseRead read, void* context,
                               GtavRenderPhaseDiscovery* result) {
  static const uint8_t kLeafInvoke[] = {0xff, 0x67, 0x20};
  static const uint8_t kGroupDispatch[] = {
      0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x5f, 0x20, 0x48, 0x85, 0xdb, 0x74, 0x13,
      0x90, 0x48, 0x8b, 0x03, 0x48, 0x89, 0xdf, 0xff, 0x50, 0x10, 0x48, 0x8b, 0x5b, 0x18, 0x48,
      0x85, 0xdb, 0x75, 0xee, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc,
  };
  static const uint8_t kGroup2Dispatch[] = {0x48, 0x8b, 0x03, 0x48, 0x89, 0xdf, 0xff, 0x50, 0x10,
                                            0x48, 0x8b, 0x5b, 0x18, 0x48, 0x85, 0xdb, 0x75, 0xee};
  static const uint8_t kRegistration[] = {0x48, 0x8d, 0x3d, 0x42, 0x75, 0x3d, 0x01,
                                          0xbe, 0xf7, 0x60, 0x97, 0x24, 0x31, 0xd2,
                                          0xe8, 0x86, 0x80, 0xbb, 0x02};
  static const uint64_t kLeafInvokePointer = 0x3494810ull;
  static const uint64_t kGroupInvokePointer = 0x34948c0ull;
  DiscoveryWalk walk;
  uint64_t root_before = 0, root_after = 0;
  if (!read || !result) return -1;
  memset(&walk, 0, sizeof(walk));
  memset(result, 0, sizeof(*result));
  walk.read = read;
  walk.context = context;
  walk.result = result;
  if (check_bytes(&walk, 0x3494810ull, kLeafInvoke, sizeof(kLeafInvoke)) != 0 ||
      check_bytes(&walk, 0x34948c0ull, kGroupDispatch, sizeof(kGroupDispatch)) != 0 ||
      check_bytes(&walk, 0x8beb31ull, kGroup2Dispatch, sizeof(kGroup2Dispatch)) != 0 ||
      check_bytes(&walk, 0x8d7477ull, kRegistration, sizeof(kRegistration)) != 0 ||
      check_bytes(&walk, GTAV_RENDER_PHASE_DISCOVERY_LEAF_VTABLE + 16u,
                  (const uint8_t*)&kLeafInvokePointer, sizeof(kLeafInvokePointer)) != 0 ||
      check_bytes(&walk, GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE + 16u,
                  (const uint8_t*)&kGroupInvokePointer, sizeof(kGroupInvokePointer)) != 0)
    return -1;
  if (read_exact(&walk, GTAV_RENDER_PHASE_DISCOVERY_ROOT, &root_before, sizeof(root_before)) != 0)
    return -1;
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
  if (read_exact(&walk, GTAV_RENDER_PHASE_DISCOVERY_ROOT, &root_after, sizeof(root_after)) != 0)
    return -1;
  if (root_before != root_after) return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MUTATION);
  if (result->matches != 1u || !result->object || !result->slot)
    return fail(&walk, GTAV_RENDER_PHASE_DISCOVERY_ERROR_MATCH);
  return 0;
}
