#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_NEXHEN_PUBLIC __attribute__((visibility("default")))
#define GTAV_NEXHEN_CONTRACT_MAGIC UINT64_C(0x314E454D56415447)
#define GTAV_NEXHEN_CONTRACT_ABI 1u
#define GTAV_NEXHEN_EXPECTED_MAX 32u

enum {
  GTAV_NEXHEN_UNCONFIGURED = 0,
  GTAV_NEXHEN_ARMED = 1,
  GTAV_NEXHEN_STARTING = 2,
  GTAV_NEXHEN_ACTIVE = 3,
  GTAV_NEXHEN_STOPPING = 4,
  GTAV_NEXHEN_STOPPED = 5,
  GTAV_NEXHEN_FAILED = 6,
};

enum {
  GTAV_NEXHEN_REQUIRE_EXTERNAL_FRAME_HOOK = 1u << 0,
  GTAV_NEXHEN_REQUIRE_EXTERNAL_RENDER_PHASE = 1u << 1,
  GTAV_NEXHEN_REQUIRE_EXACT_TARGET = 1u << 2,
  GTAV_NEXHEN_JOIN_WORKER_ON_UNLOAD = 1u << 3,
};

typedef struct GtavNexhenContract {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t expected_len;
  uint32_t patch_len;
  uint32_t stolen_len;
  uint64_t hook_target;
  uint64_t hook_continuation;
  uint64_t thunk_value_slot;
  uint64_t gateway;
  uint64_t gateway_continuation_slot;
  uint64_t render_phase_state;
  uint64_t render_phase_wrapper;
  uint64_t patch_broker_state;
  uint64_t render_phase_root;
  uint64_t render_phase_leaf_vtable;
  uint64_t render_phase_group_vtable;
  uint64_t render_phase_original;
  uint32_t render_phase_task_id;
  uint32_t reserved0;
  uint8_t expected[GTAV_NEXHEN_EXPECTED_MAX];
  char target_id[64];
  char title_id[16];
  char content_version[16];
  volatile uint64_t operation_id;
  volatile uint32_t expected_pid;
  volatile uint32_t activation_state;
  volatile int32_t start_result;
  volatile int32_t stop_result;
  volatile uint32_t worker_state;
  volatile uint32_t reserved1;
  uint64_t frame_hook_inflight;
  uint64_t render_phase_inflight;
} GtavNexhenContract;

#ifdef __cplusplus
static_assert(offsetof(GtavNexhenContract, operation_id) == 264,
              "nexHEN contract operation offset");
static_assert(sizeof(GtavNexhenContract) == 312, "nexHEN contract ABI size");
#else
_Static_assert(offsetof(GtavNexhenContract, operation_id) == 264,
               "nexHEN contract operation offset");
_Static_assert(sizeof(GtavNexhenContract) == 312, "nexHEN contract ABI size");
#endif

extern GTAV_NEXHEN_PUBLIC GtavNexhenContract gtav_nexhen_contract;
extern GTAV_NEXHEN_PUBLIC const char* g_pluginName;
extern GTAV_NEXHEN_PUBLIC const char* g_pluginDesc;
extern GTAV_NEXHEN_PUBLIC const char* g_pluginAuth;
extern GTAV_NEXHEN_PUBLIC uint32_t g_pluginVersion;

GTAV_NEXHEN_PUBLIC int32_t plugin_load(int32_t argc, const char* argv[]);
GTAV_NEXHEN_PUBLIC int32_t plugin_unload(int32_t argc, const char* argv[]);

int gtav_nexhen_plugin_load(int32_t argc, const char* argv[]);
int gtav_nexhen_plugin_unload(int32_t argc, const char* argv[]);
int gtav_nexhen_module_start(size_t size, const void* args);
int gtav_nexhen_module_stop(size_t size, const void* args);

#ifdef __cplusplus
}
#endif
