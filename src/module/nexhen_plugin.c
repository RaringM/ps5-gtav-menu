#include "gtavmenu/nexhen_plugin.h"

#include "gtavmenu/frame_hook.h"
#include "gtavmenu/patch_broker.h"
#include "gtavmenu/render_phase_intercept.h"
#include "gtavmenu/worker_lifecycle.h"

#include <stdatomic.h>
#include <unistd.h>

#ifndef GTAV_MENU_FRAME_HOOK_TARGET
#define GTAV_MENU_FRAME_HOOK_TARGET 0ull
#endif
#ifndef GTAV_FRAME_HOOK_CONTINUATION
#define GTAV_FRAME_HOOK_CONTINUATION 0ull
#endif
#ifndef GTAV_FRAME_HOOK_PATCH_LEN
#define GTAV_FRAME_HOOK_PATCH_LEN 0u
#endif
#ifndef GTAV_FRAME_HOOK_STOLEN_LEN
#define GTAV_FRAME_HOOK_STOLEN_LEN 0u
#endif
#ifndef GTAV_FRAME_HOOK_STOLEN_BYTES
#define GTAV_FRAME_HOOK_STOLEN_BYTES 0
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_ROOT
#define GTAV_RENDER_PHASE_DISCOVERY_ROOT 0ull
#endif
#ifndef GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE
#define GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE 0ull
#endif
#ifndef GTAV_RENDER_PHASE_LEAF_VTABLE
#define GTAV_RENDER_PHASE_LEAF_VTABLE 0ull
#endif
#ifndef GTAV_RENDER_PHASE_ORIGINAL
#define GTAV_RENDER_PHASE_ORIGINAL 0ull
#endif
#ifndef GTAV_RENDER_PHASE_TASK_ID
#define GTAV_RENDER_PHASE_TASK_ID 0u
#endif
#ifndef GTAV_NEXHEN_TARGET_ID
#define GTAV_NEXHEN_TARGET_ID "unsupported"
#endif
#ifndef GTAV_NEXHEN_TITLE_ID
#define GTAV_NEXHEN_TITLE_ID "unsupported"
#endif
#ifndef GTAV_NEXHEN_CONTENT_VERSION
#define GTAV_NEXHEN_CONTENT_VERSION "unsupported"
#endif

enum { NX_IDLE, NX_STARTING, NX_ACTIVE, NX_STOPPING, NX_FAILED };

extern uintptr_t gtav_frame_hook_thunk_value;
extern uintptr_t gtav_frame_hook_gateway_continuation_value;
void gtav_frame_hook_gateway(void);

static atomic_int g_lifecycle_state;
static atomic_flag g_lifecycle_lock = ATOMIC_FLAG_INIT;

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "nexHEN lifecycle state must be lock-free");
_Static_assert(GTAV_FRAME_HOOK_STOLEN_LEN <= GTAV_NEXHEN_EXPECTED_MAX,
               "frame-hook identity exceeds nexHEN contract capacity");

GTAV_NEXHEN_PUBLIC const char* g_pluginName = "GTAV Menu";
GTAV_NEXHEN_PUBLIC const char* g_pluginDesc = "GTA V menu for the corrected nexHEN module bridge";
GTAV_NEXHEN_PUBLIC const char* g_pluginAuth = "Miles Richey and contributors";
GTAV_NEXHEN_PUBLIC uint32_t g_pluginVersion = 0x00010002u;

GTAV_NEXHEN_PUBLIC GtavNexhenContract gtav_nexhen_contract = {
    .magic = GTAV_NEXHEN_CONTRACT_MAGIC,
    .abi_version = GTAV_NEXHEN_CONTRACT_ABI,
    .struct_size = sizeof(GtavNexhenContract),
    .flags = GTAV_NEXHEN_REQUIRE_EXTERNAL_FRAME_HOOK | GTAV_NEXHEN_REQUIRE_EXTERNAL_RENDER_PHASE |
             GTAV_NEXHEN_REQUIRE_EXACT_TARGET | GTAV_NEXHEN_JOIN_WORKER_ON_UNLOAD,
    .expected_len = GTAV_FRAME_HOOK_STOLEN_LEN,
    .patch_len = GTAV_FRAME_HOOK_PATCH_LEN,
    .stolen_len = GTAV_FRAME_HOOK_STOLEN_LEN,
    .hook_target = GTAV_MENU_FRAME_HOOK_TARGET,
    .hook_continuation = GTAV_FRAME_HOOK_CONTINUATION,
    .thunk_value_slot = (uintptr_t)&gtav_frame_hook_thunk_value,
    .gateway = (uintptr_t)&gtav_frame_hook_gateway,
    .gateway_continuation_slot = (uintptr_t)&gtav_frame_hook_gateway_continuation_value,
#if GTAV_RENDER_PHASE_INTERCEPT
    .render_phase_state = (uintptr_t)&gtav_render_phase_state,
    .render_phase_wrapper = (uintptr_t)&gtav_render_phase_wrapper,
#endif
    .patch_broker_state = (uintptr_t)&gtav_patch_broker_state,
    .render_phase_root = GTAV_RENDER_PHASE_DISCOVERY_ROOT,
    .render_phase_leaf_vtable = GTAV_RENDER_PHASE_LEAF_VTABLE,
    .render_phase_group_vtable = GTAV_RENDER_PHASE_DISCOVERY_GROUP_VTABLE,
    .render_phase_original = GTAV_RENDER_PHASE_ORIGINAL,
    .render_phase_task_id = GTAV_RENDER_PHASE_TASK_ID,
    .expected = {GTAV_FRAME_HOOK_STOLEN_BYTES},
    .target_id = GTAV_NEXHEN_TARGET_ID,
    .title_id = GTAV_NEXHEN_TITLE_ID,
    .content_version = GTAV_NEXHEN_CONTENT_VERSION,
    .frame_hook_inflight = (uintptr_t)&gtav_frame_hook_inflight_count,
#if GTAV_RENDER_PHASE_INTERCEPT
    .render_phase_inflight = (uintptr_t)&gtav_render_phase_inflight_count,
#endif
};

static void lifecycle_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_lifecycle_lock, memory_order_acquire)) {
    __asm__ __volatile__("pause");
  }
}

static void lifecycle_unlock(void) {
  atomic_flag_clear_explicit(&g_lifecycle_lock, memory_order_release);
}

static int activation_is_armed(void) {
  return gtav_nexhen_contract.activation_state == GTAV_NEXHEN_ARMED &&
         gtav_nexhen_contract.operation_id != 0 &&
         gtav_nexhen_contract.expected_pid == (uint32_t)getpid();
}

int gtav_nexhen_plugin_load(int32_t argc, const char* argv[]) {
  int result = -1;
  (void)argc;
  (void)argv;

  lifecycle_lock();
  int state = atomic_load_explicit(&g_lifecycle_state, memory_order_acquire);
  if (state == NX_ACTIVE) {
    lifecycle_unlock();
    return 0;
  }
  if (state != NX_IDLE || !activation_is_armed()) {
    gtav_nexhen_contract.start_result = -1;
    gtav_nexhen_contract.worker_state = gtav_menu_managed_state();
    gtav_nexhen_contract.activation_state = GTAV_NEXHEN_FAILED;
    lifecycle_unlock();
    return -1;
  }

  atomic_store_explicit(&g_lifecycle_state, NX_STARTING, memory_order_release);
  gtav_nexhen_contract.activation_state = GTAV_NEXHEN_STARTING;
  result = gtav_menu_start_managed();
  gtav_nexhen_contract.start_result = result;
  gtav_nexhen_contract.worker_state = gtav_menu_managed_state();
  gtav_nexhen_contract.activation_state = result == 0 ? GTAV_NEXHEN_ACTIVE : GTAV_NEXHEN_FAILED;
  atomic_store_explicit(&g_lifecycle_state, result == 0 ? NX_ACTIVE : NX_IDLE,
                        memory_order_release);
  lifecycle_unlock();
  return result;
}

int gtav_nexhen_plugin_unload(int32_t argc, const char* argv[]) {
  int result;
  (void)argc;
  (void)argv;

  lifecycle_lock();
  int state = atomic_load_explicit(&g_lifecycle_state, memory_order_acquire);
  if (state == NX_IDLE) {
    gtav_nexhen_contract.stop_result = 0;
    gtav_nexhen_contract.worker_state = gtav_menu_managed_state();
    gtav_nexhen_contract.activation_state = GTAV_NEXHEN_STOPPED;
    lifecycle_unlock();
    return 0;
  }
  if (state != NX_ACTIVE) {
    lifecycle_unlock();
    return -1;
  }

  atomic_store_explicit(&g_lifecycle_state, NX_STOPPING, memory_order_release);
  gtav_nexhen_contract.activation_state = GTAV_NEXHEN_STOPPING;
  result = gtav_menu_stop_managed();
  gtav_nexhen_contract.stop_result = result;
  gtav_nexhen_contract.worker_state = gtav_menu_managed_state();
  gtav_nexhen_contract.activation_state = result == 0 ? GTAV_NEXHEN_STOPPED : GTAV_NEXHEN_FAILED;
  atomic_store_explicit(&g_lifecycle_state, result == 0 ? NX_IDLE : NX_FAILED,
                        memory_order_release);
  lifecycle_unlock();
  return result;
}

int gtav_nexhen_module_start(size_t size, const void* args) {
  (void)size;
  (void)args;
  return 0;
}

int gtav_nexhen_module_stop(size_t size, const void* args) {
  (void)size;
  (void)args;
  return gtav_nexhen_plugin_unload(0, NULL);
}
