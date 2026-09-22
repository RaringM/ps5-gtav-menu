#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Single-producer / single-consumer job queue used to marshal gameplay native
// calls from the scePad worker thread (producer) onto the game's script/native
// thread (consumer, driven by a vetted hook or native-handler table wrapper).
// Entity-creating natives (CREATE_VEHICLE etc.) crash off-thread, so the worker
// enqueues a job and the script-context consumer drains it in the correct thread
// context.
//
// SPSC with a power-of-two ring and a head/tail index pair: the producer only
// advances tail, the consumer only advances head, so no lock is required as
// long as there is exactly one producer and one consumer. Capacity is fixed and
// a push to a full ring is dropped (reported via the return value) rather than
// blocking the worker.

#define GTAV_JOB_QUEUE_CAPACITY 32u /* must be a power of two */

typedef struct GtavNativeJob {
  uint32_t action;
  uint32_t param;
} GtavNativeJob;

typedef struct GtavNativeJobQueue {
  GtavNativeJob slots[GTAV_JOB_QUEUE_CAPACITY];
  volatile uint32_t head; /* consumer index (next to read)  */
  volatile uint32_t tail; /* producer index (next to write) */
  uint32_t dropped;       /* count of pushes dropped on a full ring */
} GtavNativeJobQueue;

typedef void (*GtavNativeJobFn)(uint32_t action, uint32_t param, void* context);

void gtav_job_queue_init(GtavNativeJobQueue* q);
uint32_t gtav_job_queue_count(const GtavNativeJobQueue* q);
int gtav_job_queue_is_empty(const GtavNativeJobQueue* q);
int gtav_job_queue_is_full(const GtavNativeJobQueue* q);

// Producer side (scePad worker). Returns 1 on enqueue, 0 if the ring was full
// (job dropped; q->dropped incremented).
int gtav_job_queue_push(GtavNativeJobQueue* q, uint32_t action, uint32_t param);

// Consumer side (script-context hook/wrapper). Drains up to max_jobs (0 = all
// queued at entry), invoking fn for each. Returns the number drained. Bounds the
// per-call work so a flood of jobs cannot stall the game thread. Only jobs present
// when the call begins are run: if fn re-enqueues a job (e.g. the spawn job while
// its model is still streaming), the requeue waits for the next call rather than
// re-running in this one -- so one job run corresponds to exactly one hook fire.
uint32_t gtav_job_queue_drain(GtavNativeJobQueue* q, uint32_t max_jobs, GtavNativeJobFn fn,
                              void* context);

#ifdef __cplusplus
}
#endif
