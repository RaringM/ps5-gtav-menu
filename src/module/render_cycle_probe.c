#include "gtavmenu/render_cycle_probe.h"

#if GTAV_RENDER_CYCLE_GATE
#include <string.h>

#if GTAV_RENDER_CYCLE_PROBE
GtavRenderCycleState gtav_render_cycle_probe
    __attribute__((used, retain, visibility("default"))) = {
        .magic = GTAV_RENDER_CYCLE_MAGIC, .abi = 1, .size = sizeof(GtavRenderCycleState)};

static GtavRenderCycleSample g_previous;
static int g_seen;  // callback-guard-owned, like g_previous
#endif

static void read_sample(GtavRenderCycleReadWord read_word, void* context,
                        GtavRenderCycleSample* s) {
  s->epoch = read_word(GTAV_CYCLE_EPOCH_ADDR, context);
  s->reset_epoch = read_word(GTAV_CYCLE_RESET_ADDR, context);
  s->producer = read_word(GTAV_CYCLE_PRODUCER_ADDR, context);
  s->consumer = read_word(GTAV_CYCLE_CONSUMER_ADDR, context);
  // Fixed bank addresses: a corrupt selector can never turn into an unchecked dereference.
  s->count0 = read_word(GTAV_CYCLE_COUNT0_ADDR, context);
  s->count1 = read_word(GTAV_CYCLE_COUNT1_ADDR, context);
  s->text_mode = read_word(GTAV_CYCLE_TEXT_ADDR, context);
  s->game_counter = read_word(GTAV_CYCLE_COUNTER_ADDR, context);
}

uint32_t gtav_render_cycle_read(GtavRenderCycleReadWord read_word, void* context,
                                GtavRenderCycleSample* out) {
  GtavRenderCycleSample before;
  read_sample(read_word, context, &before);
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  read_sample(read_word, context, out);
  uint32_t flags = memcmp(&before, out, sizeof(before)) ? GTAV_CYCLE_CHANGED_DURING_READ : 0;
  if (before.producer > 1 || before.consumer > 1 || out->producer > 1 || out->consumer > 1)
    flags |= GTAV_CYCLE_INVALID_SELECTOR;
  if (before.count0 > 500 || before.count1 > 500 || out->count0 > 500 || out->count1 > 500)
    flags |= GTAV_CYCLE_INVALID_COUNT;
  if (out->text_mode) flags |= GTAV_CYCLE_TEXT_BUSY;
  if (out->epoch != out->reset_epoch) flags |= GTAV_CYCLE_RESET_DIFFERS;
  if (out->producer == out->consumer) flags |= GTAV_CYCLE_SELECTORS_EQUAL;
  return flags;
}

#if GTAV_RENDER_CYCLE_PROBE
int gtav_render_cycle_arm(uint32_t operation) {
  if (operation == 2) {
    __atomic_store_n(&gtav_render_cycle_probe.armed, 2, __ATOMIC_RELEASE);
    return 0;
  }
  uint64_t expected = 0;
  if (operation != 1) return -1;
  return __atomic_compare_exchange_n(&gtav_render_cycle_probe.armed, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)
             ? 0
             : -1;
}
#endif

static uint32_t target_word(uintptr_t address, void* context) {
  return *(const volatile uint32_t*)address;
}

uint32_t gtav_render_cycle_sample_target(GtavRenderCycleSample* out) {
  return gtav_render_cycle_read(target_word, NULL, out);
}

#if GTAV_RENDER_CYCLE_PROBE
void gtav_render_cycle_observe(uintptr_t thread, uintptr_t context) {
  if (__atomic_load_n(&gtav_render_cycle_probe.armed, __ATOMIC_ACQUIRE) != 1) return;
  const uint64_t start = gtav_render_diag_stamp();
  GtavRenderCycleSample s;
  const uint32_t flags = gtav_render_cycle_sample_target(&s);
  GtavRenderCycleState* d = &gtav_render_cycle_probe;
  const uint64_t n = __atomic_add_fetch(&d->observations, 1, __ATOMIC_RELAXED);
#define COUNT_IF(field, bit) \
  if (flags & (bit)) __atomic_fetch_add(&d->field, 1, __ATOMIC_RELAXED)
  COUNT_IF(changed_reads, GTAV_CYCLE_CHANGED_DURING_READ);
  COUNT_IF(invalid_selectors, GTAV_CYCLE_INVALID_SELECTOR);
  COUNT_IF(invalid_counts, GTAV_CYCLE_INVALID_COUNT);
  COUNT_IF(text_busy, GTAV_CYCLE_TEXT_BUSY);
  COUNT_IF(reset_differs, GTAV_CYCLE_RESET_DIFFERS);
  COUNT_IF(selectors_equal, GTAV_CYCLE_SELECTORS_EQUAL);
#undef COUNT_IF
  const uint64_t last_thread = __atomic_load_n(&d->last_thread, __ATOMIC_RELAXED);
  if (!g_seen) __atomic_store_n(&d->first_thread, thread, __ATOMIC_RELAXED);
  if (g_seen && last_thread != thread) __atomic_fetch_add(&d->thread_changes, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&d->last_thread, thread, __ATOMIC_RELAXED);

  // Changed-read observations cannot advance epoch bookkeeping. Counts/text may still be useful
  // raw evidence, but there is deliberately no "safe to draw" boolean or inferred cycle token.
  const int valid = !(flags & (GTAV_CYCLE_CHANGED_DURING_READ | GTAV_CYCLE_INVALID_SELECTOR |
                               GTAV_CYCLE_INVALID_COUNT));
  const int changed = !g_seen || s.epoch != g_previous.epoch ||
                      s.reset_epoch != g_previous.reset_epoch ||
                      s.producer != g_previous.producer || s.consumer != g_previous.consumer ||
                      s.text_mode != g_previous.text_mode ||
                      s.game_counter != g_previous.game_counter || last_thread != thread;
  if (valid) {
    if (g_seen && s.epoch != g_previous.epoch) {
      __atomic_fetch_add(&d->epoch_changes, 1, __ATOMIC_RELAXED);
      uint32_t delta = s.epoch - g_previous.epoch;
      // Wrap is handled modulo 32 bits. A large backwards jump is not millions of missing cycles.
      if (delta > 1 && delta < 0x80000000u)
        __atomic_fetch_add(&d->epoch_gaps, delta - 1u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&d->last_epoch, s.epoch, __ATOMIC_RELAXED);
    g_previous = s;
    g_seen = 1;
  }
  if (changed || !valid || (n & 255u) == 1u) {
    GtavRenderDiagEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = GTAV_RD_CYCLE_OBSERVATION;
    e.lane = GTAV_RD_HOOK;
    e.stamp = start;
    e.config = gtav_render_diag_config();
    e.thread_key = thread;
    e.context = context;
    e.tick = s.epoch;
    e.wall_ns = s.reset_epoch;
    e.work_ns = s.game_counter;
    e.wake_late_ns = s.producer;
    e.skip = flags;
    e.attempts = s.count0 | ((uint64_t)s.count1 << 32);
    e.completed = s.consumer;
    e.settings = s.text_mode;
    e.duration = gtav_render_diag_stamp() - start;
    gtav_render_diag_emit(&e);
  }
  const uint64_t duration = gtav_render_diag_stamp() - start;
  __atomic_fetch_add(&d->observation_tsc_total, duration, __ATOMIC_RELAXED);
  // Observer is serialized by the callback guard; readers use individually atomic loads.
  if (duration > __atomic_load_n(&d->observation_tsc_max, __ATOMIC_RELAXED))
    __atomic_store_n(&d->observation_tsc_max, duration, __ATOMIC_RELAXED);
}
#endif
#endif
