#include "gtavmenu/native_job_queue.h"

// Power-of-two mask so head/tail can advance monotonically and wrap via &.
#define GTAV_JOB_QUEUE_MASK (GTAV_JOB_QUEUE_CAPACITY - 1u)

void gtav_job_queue_init(GtavNativeJobQueue* q) {
  if (!q) return;
  q->head = 0;
  q->tail = 0;
  q->dropped = 0;
  for (uint32_t i = 0; i < GTAV_JOB_QUEUE_CAPACITY; ++i) {
    q->slots[i].action = 0;
    q->slots[i].param = 0;
  }
}

uint32_t gtav_job_queue_count(const GtavNativeJobQueue* q) {
  if (!q) return 0;
  // tail - head is the number of unread slots; both advance monotonically so the
  // unsigned subtraction is correct across wraps.
  return (uint32_t)(q->tail - q->head);
}

int gtav_job_queue_is_empty(const GtavNativeJobQueue* q) {
  return gtav_job_queue_count(q) == 0u;
}

int gtav_job_queue_is_full(const GtavNativeJobQueue* q) {
  return gtav_job_queue_count(q) >= GTAV_JOB_QUEUE_CAPACITY;
}

int gtav_job_queue_push(GtavNativeJobQueue* q, uint32_t action, uint32_t param) {
  if (!q) return 0;
  if (gtav_job_queue_is_full(q)) {
    q->dropped++;
    return 0;
  }
  const uint32_t slot = q->tail & GTAV_JOB_QUEUE_MASK;
  q->slots[slot].action = action;
  q->slots[slot].param = param;
  // Publish the slot contents before advancing tail so the consumer never reads
  // a half-written job.
  __atomic_thread_fence(__ATOMIC_RELEASE);
  q->tail = q->tail + 1u;
  return 1;
}

uint32_t gtav_job_queue_drain(GtavNativeJobQueue* q, uint32_t max_jobs, GtavNativeJobFn fn,
                              void* context) {
  if (!q) return 0;
  // Snapshot the producer's tail at entry and drain only up to it. A job's fn() may
  // re-enqueue itself -- the vehicle spawn job does exactly this every time it is
  // still waiting on model streaming. If we instead looped on the live tail (the
  // is-empty check), that freshly re-enqueued job would be run again immediately,
  // within the same drain, with no game frame elapsing in between. A self-requeuing
  // job would then execute up to max_jobs times per call, burning its retry budget
  // ~max_jobs faster than wall-clock frames and spamming redundant REQUEST_MODEL
  // calls that cannot make progress (the streamer only advances per frame). Bounding
  // by the entry tail makes "one job run == one hook fire", so a requeued job
  // correctly waits for the next fire. Jobs genuinely queued before entry are all
  // still drained (subject to max_jobs).
  const uint32_t tail_at_entry = q->tail;
  // Acquire the slot contents the producer published before it advanced tail.
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  uint32_t drained = 0;
  // tail_at_entry - head is the count present at entry; both advance monotonically
  // so the unsigned subtraction is correct across wraps.
  while ((uint32_t)(tail_at_entry - q->head) != 0u) {
    if (max_jobs != 0u && drained >= max_jobs) break;
    const uint32_t slot = q->head & GTAV_JOB_QUEUE_MASK;
    const uint32_t action = q->slots[slot].action;
    const uint32_t param = q->slots[slot].param;
    if (fn) {
      fn(action, param, context);
    }
    q->head = q->head + 1u;
    ++drained;
  }
  return drained;
}
