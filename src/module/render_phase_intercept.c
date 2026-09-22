#include "gtavmenu/render_phase_intercept.h"

#if GTAV_RENDER_PHASE_INTERCEPT
#include <stddef.h>
#include <string.h>

#include "gtavmenu/menu.h"

_Static_assert(__atomic_always_lock_free(8, 0), "phase interception requires lock-free qwords");

GtavRenderPhaseState gtav_render_phase_state
    __attribute__((used, retain, visibility("default"))) = {
        .magic = GTAV_RENDER_PHASE_MAGIC,
        .abi = 1,
        .size = sizeof(GtavRenderPhaseState),
        .original = GTAV_RENDER_PHASE_ORIGINAL,
        .wrapper = (uintptr_t)&gtav_render_phase_wrapper,
};

uintptr_t gtav_render_phase_original __attribute__((used, retain, visibility("default"))) =
    GTAV_RENDER_PHASE_ORIGINAL;

static int plausible_slot(uint64_t slot) {
  return slot >= 0x100000020ull && slot < 0x8000000000000ull && !(slot & 7u);
}

static void fail(uint64_t error) {
  __atomic_store_n(&gtav_render_phase_state.error, error, __ATOMIC_RELAXED);
  __atomic_store_n(&gtav_render_phase_state.phase, GTAV_RENDER_PHASE_FAILED, __ATOMIC_RELEASE);
}

int gtav_render_phase_control(uint32_t key, uint64_t value) {
  GtavRenderPhaseState* s = &gtav_render_phase_state;
  uint64_t phase = __atomic_load_n(&s->phase, __ATOMIC_ACQUIRE);
  if (key == 0) {
    if (phase != GTAV_RENDER_PHASE_UNCONFIGURED || !plausible_slot(value)) return -1;
    __atomic_store_n(&s->slot, value, __ATOMIC_RELAXED);
    __atomic_store_n(&s->object, value - 32u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_READY, __ATOMIC_RELEASE);
    return 0;
  }
  if (key != 1) return -1;
  if (value == 1 && phase == GTAV_RENDER_PHASE_READY) {
    __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_INSTALL_PENDING, __ATOMIC_RELEASE);
    return 0;
  }
  if (value == 2 &&
      (phase == GTAV_RENDER_PHASE_READY || phase == GTAV_RENDER_PHASE_INSTALL_PENDING ||
       phase == GTAV_RENDER_PHASE_INSTALLED || phase == GTAV_RENDER_PHASE_RESTORE_PENDING)) {
    __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_RESTORE_PENDING, __ATOMIC_RELEASE);
    return gtav_render_phase_restore();
  }
  return -1;
}

static int object_matches(uintptr_t object) {
  const volatile uint64_t* words = (const volatile uint64_t*)object;
  return words[0] == GTAV_RENDER_PHASE_LEAF_VTABLE &&
         (uint32_t)words[2] == GTAV_RENDER_PHASE_TASK_ID;
}

void gtav_render_phase_service(void) {
  GtavRenderPhaseState* s = &gtav_render_phase_state;
  if (__atomic_load_n(&s->phase, __ATOMIC_ACQUIRE) != GTAV_RENDER_PHASE_INSTALL_PENDING) return;
  __atomic_fetch_add(&s->services, 1, __ATOMIC_RELAXED);
  const uintptr_t slot = __atomic_load_n(&s->slot, __ATOMIC_RELAXED);
  const uintptr_t object = __atomic_load_n(&s->object, __ATOMIC_RELAXED);
  if (!plausible_slot(slot) || object != slot - 32u || !object_matches(object)) {
    fail(GTAV_RENDER_PHASE_ERROR_OBJECT);
    return;
  }
  uint64_t expected = GTAV_RENDER_PHASE_ORIGINAL;
  const uint64_t wrapper = (uintptr_t)&gtav_render_phase_wrapper;
  if (!__atomic_compare_exchange_n((uint64_t*)slot, &expected, wrapper, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    fail(GTAV_RENDER_PHASE_ERROR_INSTALL_CAS);
    return;
  }
  __atomic_fetch_add(&s->installs, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_INSTALLED, __ATOMIC_RELEASE);
}

int gtav_render_phase_restore(void) {
  GtavRenderPhaseState* s = &gtav_render_phase_state;
  const uint64_t phase = __atomic_load_n(&s->phase, __ATOMIC_ACQUIRE);
  // A default-off build may shut down without ever receiving a slot. In that state there is
  // nothing to restore and, importantly, no target address that is safe to dereference.
  if (phase == GTAV_RENDER_PHASE_UNCONFIGURED) return 0;
  const uintptr_t slot = __atomic_load_n(&s->slot, __ATOMIC_RELAXED);
  if (!plausible_slot(slot)) {
    fail(GTAV_RENDER_PHASE_ERROR_CONFIG);
    return -1;
  }
  uint64_t current = __atomic_load_n((uint64_t*)slot, __ATOMIC_ACQUIRE);
  if (current == GTAV_RENDER_PHASE_ORIGINAL) {
    __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_RESTORED, __ATOMIC_RELEASE);
    return 0;
  }
  uint64_t expected = (uintptr_t)&gtav_render_phase_wrapper;
  if (!__atomic_compare_exchange_n((uint64_t*)slot, &expected, GTAV_RENDER_PHASE_ORIGINAL, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    fail(GTAV_RENDER_PHASE_ERROR_RESTORE_CAS);
    return -1;
  }
  __atomic_fetch_add(&s->restores, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&s->phase, GTAV_RENDER_PHASE_RESTORED, __ATOMIC_RELEASE);
  return 0;
}

void gtav_render_phase_wrapper_body(uintptr_t object) {
  GtavRenderPhaseState* s = &gtav_render_phase_state;
  __atomic_fetch_add(&s->wrapper_calls, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&s->last_object, object, __ATOMIC_RELAXED);
  if (__atomic_load_n(&s->phase, __ATOMIC_ACQUIRE) != GTAV_RENDER_PHASE_INSTALLED ||
      object != __atomic_load_n(&s->object, __ATOMIC_RELAXED)) {
    __atomic_fetch_add(&s->gate_rejections, 1, __ATOMIC_RELAXED);
    return;
  }

  GtavRenderCycleSample sample;
  const uint32_t flags = gtav_render_cycle_sample_target(&sample);
  __atomic_store_n(&s->last_flags, flags, __ATOMIC_RELAXED);
  if (flags != 0) {
    __atomic_fetch_add(&s->gate_rejections, 1, __ATOMIC_RELAXED);
    return;
  }
  const uint64_t previous = __atomic_load_n(&s->last_epoch, __ATOMIC_RELAXED);
  if (__atomic_load_n(&s->draw_attempts, __ATOMIC_RELAXED) && previous == sample.epoch) {
    __atomic_fetch_add(&s->duplicate_epochs, 1, __ATOMIC_RELAXED);
    return;
  }
  __atomic_store_n(&s->last_epoch, sample.epoch, __ATOMIC_RELAXED);
  const uint64_t attempt = __atomic_fetch_add(&s->draw_attempts, 1, __ATOMIC_RELAXED);
  if (!attempt) __atomic_store_n(&s->first_epoch, sample.epoch, __ATOMIC_RELAXED);
  if (gtav_menu_render_phase_tick(sample.epoch))
    __atomic_fetch_add(&s->draw_completed, 1, __ATOMIC_RELAXED);
}
#endif
