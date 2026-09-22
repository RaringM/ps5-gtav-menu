#pragma once

// Census of the rage script handlers that reach our frame hook, and the decision "is THIS hook
// fire attributable to a long-lived script?".
//
// WHY THIS EXISTS
//
// The live hook CHAINS PLAYER_PED_ID, a native every script calls, so it fires ~137 times per
// rendered frame -- once for each script that asks for the player ped that frame. Offline RE of
// the 01.010.002 eboot showed that the model-streaming natives are not frame-scoped but
// SCRIPT-scoped: REQUEST_MODEL (0x1c09550) tail-jumps into a shared helper (0x1c07fb0) which,
// after issuing the real CStreaming request, registers the model as a script resource owned by
// the CALLING script:
//
//     mov rax, fs:[0]
//     mov rax, [rax - 0x140]        ; the active rage scrThread
//     mov rdi, [rax + 0x198]        ; its CGameScriptHandler
//     mov rax, [rdi]
//     call qword ptr [rax + 0x68]   ; vtable slot 13: register a type-0xE resource
//
// and SET_MODEL_AS_NO_LONGER_NEEDED (0x1c09760) releases it through slot 15 ([vtable + 0x78])
// on the same handler. So both halves of a model's lifetime land on whichever script happened
// to call PLAYER_PED_ID on that fire. When that script is transient its handler is destroyed,
// the type-0xE resource is reclaimed, and the pending streaming request is dropped -- which is
// why only already-resident models ever spawned.
//
// The fix this census enables is to WAIT for a fire that a long-lived script already owns,
// rather than to write a thread pointer into the game's TLS. That keeps us read-only with
// respect to the game's thread state; the only thing we change is WHOSE resource list the
// request lands in.
//
// WHAT IT MEASURES
//
// The decisive statistic is NOT "how many fires did handler H get" (a script that fires 100x in
// one frame and never again looks busy but is useless to us). It is "in how many DISTINCT frames
// was handler H present" -- because a request only needs one owned fire per frame, and a script
// present in every frame is by definition not transient. So `frames_present` only advances when
// the observed frame number changes, and `gtav_sh_census_coverage_pct()` reports
// frames_present/frames_observed for the dominant handler. That ratio is the go/no-go.
//
// The pure decisions live here as header-only inlines so they are host-testable with no game and
// no natives (same posture as daemon_lifecycle.h / inject_lock.h); the live wiring -- reading the
// handler off the frame hook and calling observe() once per fire -- lives in
// src/module/features/spawn_skin.inc.

#include <stdint.h>

// Distinct handlers tracked.
//
// There is deliberately NO eviction: a handler that does not fit is counted in `overflow`
// instead. Eviction policy is where this kind of table goes subtly wrong -- a newcomer always has
// frames_present == 1, so any "replace the weakest" rule would evict established slots for
// transients -- and the counter tells an operator what is actually happening instead.
//
// Live 2026-09-18 (pid 586, ~8 min): 8 slots filled immediately and overflow reached 2.1M, so
// there are far more than 8 distinct handlers. The persistent script was nonetheless among the
// first few seen, and the census found it with 100% frame coverage. The consequence of no
// eviction is therefore only that the dominant pick is "best of the first N seen" rather than
// provably the best -- which is fine here, because 100% coverage cannot be beaten. N is raised
// well above the observed steady-state need so a fresh boot is very unlikely to crowd the
// persistent script out; if it ever did, the symptom is self-diagnosing (peek prints the coverage
// and flags it as too low for the gate) rather than a silent wrong answer.
#define GTAV_SH_CENSUS_SLOTS 32u

typedef struct GtavSHSlot {
  uint64_t handler;         // CGameScriptHandler* as observed at [scrThread + 0x198]
  uint32_t first_frame;     // frame it was first seen in
  uint32_t last_frame;      // most recent frame it was seen in
  uint32_t frames_present;  // DISTINCT frames it was seen in -- the statistic that matters
  uint64_t fires;           // raw hook fires attributed to it
} GtavSHSlot;

typedef struct GtavScriptHandlerCensus {
  GtavSHSlot slots[GTAV_SH_CENSUS_SLOTS];
  uint32_t used;              // slots occupied
  uint32_t frames_observed;   // distinct frames the census has seen at all
  uint32_t last_frame;        // most recent frame observed (any handler)
  uint32_t overflow;          // fires whose handler did not fit in the table
  uint64_t fires;             // total fires observed
  uint64_t fires_no_handler;  // fires with no resolvable handler (census blind for these)
  int initialized;
} GtavScriptHandlerCensus;

static inline void gtav_sh_census_reset(GtavScriptHandlerCensus* c) {
  if (!c) return;
  for (unsigned i = 0; i < GTAV_SH_CENSUS_SLOTS; ++i) {
    c->slots[i].handler = 0;
    c->slots[i].first_frame = 0;
    c->slots[i].last_frame = 0;
    c->slots[i].frames_present = 0;
    c->slots[i].fires = 0;
  }
  c->used = 0;
  c->frames_observed = 0;
  c->last_frame = 0;
  c->overflow = 0;
  c->fires = 0;
  c->fires_no_handler = 0;
  c->initialized = 1;
}

// Record one hook fire. `handler` is 0 when the script context or handler could not be resolved
// safely -- those fires are counted separately rather than attributed to anyone. `frame` is the
// game's frame counter (GET_FRAME_COUNT); it may repeat many times per frame, which is the whole
// point.
static inline void gtav_sh_census_observe(GtavScriptHandlerCensus* c, uint64_t handler,
                                          uint32_t frame) {
  if (!c) return;
  if (!c->initialized) gtav_sh_census_reset(c);

  c->fires++;
  // A new frame for the census as a whole. Counted before the handler check so a frame in which
  // we resolved nothing still moves the denominator -- otherwise coverage would flatter itself.
  if (c->frames_observed == 0 || frame != c->last_frame) {
    c->frames_observed++;
    c->last_frame = frame;
  }
  if (!handler) {
    c->fires_no_handler++;
    return;
  }

  for (unsigned i = 0; i < c->used; ++i) {
    if (c->slots[i].handler != handler) continue;
    c->slots[i].fires++;
    if (frame != c->slots[i].last_frame) {
      c->slots[i].frames_present++;
      c->slots[i].last_frame = frame;
    }
    return;
  }
  if (c->used >= GTAV_SH_CENSUS_SLOTS) {
    c->overflow++;
    return;
  }
  GtavSHSlot* s = &c->slots[c->used++];
  s->handler = handler;
  s->first_frame = frame;
  s->last_frame = frame;
  s->frames_present = 1;
  s->fires = 1;
}

// Index of the handler present in the most distinct frames, or -1 when the census is empty.
// Frame coverage -- not fire count and not age -- is the ranking key: the request needs one owned
// fire per frame, so the script that shows up in the most frames is the one worth waiting for.
// Ties break toward the earlier-seen slot, which is the more established script.
static inline int gtav_sh_census_dominant_slot(const GtavScriptHandlerCensus* c) {
  if (!c || !c->used) return -1;
  int best = 0;
  for (unsigned i = 1; i < c->used; ++i) {
    if (c->slots[i].frames_present > c->slots[best].frames_present) best = (int)i;
  }
  return best;
}

static inline uint64_t gtav_sh_census_dominant(const GtavScriptHandlerCensus* c) {
  const int i = gtav_sh_census_dominant_slot(c);
  return i < 0 ? 0u : c->slots[i].handler;
}

// Frame coverage of the dominant handler, in percent. This is the go/no-go for the owner gate:
// ~100 means the dominant script is present in essentially every frame, so gating on it costs no
// latency. Well under 100 means a streaming job would sit out whole frames waiting for an owner.
static inline uint32_t gtav_sh_census_coverage_pct(const GtavScriptHandlerCensus* c) {
  const int i = gtav_sh_census_dominant_slot(c);
  if (i < 0 || !c->frames_observed) return 0;
  return (uint32_t)((uint64_t)c->slots[i].frames_present * 100u / c->frames_observed);
}

// Is `handler` the handler a streaming request should be attributed to, right now?
//
// Three conditions, all necessary:
//   * it is the dominant handler (most frame coverage);
//   * it has been around at least `min_age_frames` frames, so a transient that merely got seen
//     first cannot win before a real long-lived script is observed;
//   * it is FRESH -- seen within `max_stale_frames` of the newest frame the census saw. A
//     CGameScriptHandler is a heap pointer: a dead script's handler can be freed and a DIFFERENT
//     script allocated at the same address, so a stale slot must never be trusted just because
//     its recorded age is large.
static inline int gtav_sh_census_is_dominant(const GtavScriptHandlerCensus* c, uint64_t handler,
                                             uint32_t min_age_frames, uint32_t max_stale_frames) {
  if (!c || !handler) return 0;
  const int i = gtav_sh_census_dominant_slot(c);
  if (i < 0) return 0;
  if (c->slots[i].handler != handler) return 0;
  if (c->slots[i].last_frame - c->slots[i].first_frame < min_age_frames) return 0;
  if (c->last_frame - c->slots[i].last_frame > max_stale_frames) return 0;
  return 1;
}
