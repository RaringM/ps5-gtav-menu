#pragma once

// Pure, offline planner for an ordered group of exact-byte patches.
//
// This unit deliberately has no process backend, target-memory callback, or runtime entry point.
// It can describe bridge-first installs, classify caller-supplied snapshots, and plan reverse-order
// restoration, but a future target-memory owner must supply instance binding, durable journal
// publication, verified no-stop writes, and teardown quiescence before any live gate can change.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_PATCH_JOURNAL_TARGET_IO_SUPPORTED 0u
#define GTAV_PATCH_JOURNAL_ATOMIC_CODE_WRITE_SUPPORTED 0u
#define GTAV_PATCH_JOURNAL_RUNTIME_INSTALL_SUPPORTED 0u
#define GTAV_PATCH_JOURNAL_RUNTIME_RETIRE_SUPPORTED 0u

#define GTAV_PATCH_JOURNAL_MAGIC 0x4c4e4a5056415447ull
#define GTAV_PATCH_JOURNAL_ABI_VERSION 1u
#define GTAV_PATCH_JOURNAL_MAX_ENTRIES 8u
#define GTAV_PATCH_JOURNAL_MAX_BYTES 32u
#define GTAV_PATCH_JOURNAL_REL32_CALL_SIZE 5u
#define GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE 14u

typedef enum GtavPatchJournalEntryKind {
  GTAV_PATCH_JOURNAL_ENTRY_INVALID = 0,
  GTAV_PATCH_JOURNAL_ENTRY_ABS_JUMP_BRIDGE = 1,
  GTAV_PATCH_JOURNAL_ENTRY_REL32_CALL = 2,
} GtavPatchJournalEntryKind;

typedef enum GtavPatchJournalImageState {
  GTAV_PATCH_JOURNAL_IMAGE_INVALID = 0,
  GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL = 1,
  GTAV_PATCH_JOURNAL_IMAGE_INSTALLED = 2,
  GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS = 3,
} GtavPatchJournalImageState;

typedef enum GtavPatchJournalPhase {
  GTAV_PATCH_JOURNAL_PHASE_INVALID = 0,
  GTAV_PATCH_JOURNAL_PHASE_PREPARED = 1,
  GTAV_PATCH_JOURNAL_PHASE_INSTALLING = 2,
  GTAV_PATCH_JOURNAL_PHASE_INSTALLED = 3,
  GTAV_PATCH_JOURNAL_PHASE_ROLLING_BACK = 4,
  GTAV_PATCH_JOURNAL_PHASE_RESTORED = 5,
} GtavPatchJournalPhase;

typedef enum GtavPatchJournalActionKind {
  GTAV_PATCH_JOURNAL_ACTION_REFUSE = 0,
  GTAV_PATCH_JOURNAL_ACTION_WRITE_INSTALLED = 1,
  GTAV_PATCH_JOURNAL_ACTION_WRITE_ORIGINAL = 2,
  GTAV_PATCH_JOURNAL_ACTION_COMPLETE = 3,
  GTAV_PATCH_JOURNAL_ACTION_ROLLBACK_REQUIRED = 4,
} GtavPatchJournalActionKind;

typedef struct GtavPatchJournalEntry {
  uint64_t address;
  uint32_t verify_len;
  uint32_t write_len;
  uint32_t requires_installed_mask;
  uint32_t kind;
  uint8_t original[GTAV_PATCH_JOURNAL_MAX_BYTES];
  uint8_t installed[GTAV_PATCH_JOURNAL_MAX_BYTES];
} GtavPatchJournalEntry;

// Entry order is transaction order. Dependencies must name earlier entries. A reverse walk is
// therefore also a safe dependency teardown order: call sites are restored before their bridges.
typedef struct GtavPatchJournalPlan {
  uint32_t count;
  uint32_t reserved;
  GtavPatchJournalEntry entries[GTAV_PATCH_JOURNAL_MAX_ENTRIES];
} GtavPatchJournalPlan;

typedef struct GtavPatchJournalObservation {
  const uint8_t* bytes;
  size_t size;
} GtavPatchJournalObservation;

typedef struct GtavPatchJournalSnapshot {
  uint32_t count;
  // A bit is set when every byte outside that entry's owned write prefix still matches the exact
  // original image. An attempted ambiguous prefix can then be restored once. If an unowned tail
  // byte changed, rollback must refuse instead of repeatedly overwriting the owned prefix.
  uint32_t rollback_safe_mask;
  uint32_t state[GTAV_PATCH_JOURNAL_MAX_ENTRIES];
} GtavPatchJournalSnapshot;

// `attempted_mask` is ownership evidence, not success evidence. A future live owner must publish
// the bit durably before its corresponding write attempt. That lets an exact rollback distinguish
// its own possible partial write from unrelated/foreign bytes without ever treating a write return
// value as proof that memory stayed untouched.
typedef struct GtavPatchJournal {
  uint64_t magic;
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t instance_token;
  uint64_t plan_fingerprint;
  uint32_t attempted_mask;
  uint32_t phase;
} GtavPatchJournal;

typedef struct GtavPatchJournalAction {
  uint32_t kind;
  uint32_t index;
  uint64_t address;
  uint32_t write_len;
  uint32_t verify_len;
  uint8_t write[GTAV_PATCH_JOURNAL_MAX_BYTES];
  uint8_t verify[GTAV_PATCH_JOURNAL_MAX_BYTES];
} GtavPatchJournalAction;

void gtav_patch_journal_plan_init(GtavPatchJournalPlan* plan);

// Encode a five-byte CALL rel32 or fourteen-byte RIP-indirect absolute jump. These helpers only
// produce bytes; they never read or write a process. The rel32 encoder fails closed outside the
// signed 32-bit reach from `site + 5`.
int gtav_patch_journal_encode_rel32_call(uint64_t site, uint64_t destination,
                                         uint8_t out[GTAV_PATCH_JOURNAL_REL32_CALL_SIZE]);
void gtav_patch_journal_encode_abs_jump(uint64_t destination,
                                        uint8_t out[GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE]);

// Append an exact bridge record. `original` is the complete 14-byte preimage (normally a pinned
// zero-filled cave window), and the installed image jumps to `destination` without changing the
// CALL return address already on the stack.
int gtav_patch_journal_add_abs_jump_bridge(GtavPatchJournalPlan* plan, uint64_t address,
                                           const uint8_t original[GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE],
                                           uint64_t destination);

// Append an exact call-site record. The complete verification window is retained in both images;
// only its first five bytes belong to the write. The original CALL must resolve to
// `expected_original_target`, and `bridge_index` must identify an earlier bridge record.
int gtav_patch_journal_add_rel32_call(GtavPatchJournalPlan* plan, uint64_t site,
                                      const uint8_t* original_window, uint32_t window_len,
                                      uint64_t expected_original_target, uint64_t bridge_address,
                                      uint32_t bridge_index);

int gtav_patch_journal_plan_validate(const GtavPatchJournalPlan* plan);
uint64_t gtav_patch_journal_plan_fingerprint(const GtavPatchJournalPlan* plan);

GtavPatchJournalImageState gtav_patch_journal_classify(const GtavPatchJournalEntry* entry,
                                                       const uint8_t* observed,
                                                       size_t observed_size);
int gtav_patch_journal_snapshot(const GtavPatchJournalPlan* plan,
                                const GtavPatchJournalObservation* observations,
                                uint32_t observation_count, GtavPatchJournalSnapshot* snapshot);

int gtav_patch_journal_begin(const GtavPatchJournalPlan* plan, uint64_t instance_token,
                             GtavPatchJournal* journal);
int gtav_patch_journal_matches(const GtavPatchJournalPlan* plan, const GtavPatchJournal* journal,
                               uint64_t instance_token);

// Installation is strictly ordered. Call this before the corresponding write is attempted, then
// re-snapshot the entire plan before requesting another action.
int gtav_patch_journal_note_install_attempt(const GtavPatchJournalPlan* plan,
                                            GtavPatchJournal* journal, uint64_t instance_token,
                                            const GtavPatchJournalSnapshot* snapshot,
                                            uint32_t index);
int gtav_patch_journal_note_installed(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                      uint64_t instance_token,
                                      const GtavPatchJournalSnapshot* snapshot);
int gtav_patch_journal_begin_rollback(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                      uint64_t instance_token);
int gtav_patch_journal_note_restored(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                     uint64_t instance_token,
                                     const GtavPatchJournalSnapshot* snapshot);

// Produce one hypothetical next action. WRITE actions contain only owned bytes plus the complete
// expected readback image. They do not imply that a live code write is atomic: a five-byte CALL can
// tear even when it lies within one naturally aligned word, and the known list CALL crosses a
// 16-byte boundary. Exact readback proves the final bytes, not that another thread could not have
// fetched an intermediate instruction stream. REFUSE means the snapshot/journal cannot prove
// ownership. An ambiguous or failed attempted install yields ROLLBACK_REQUIRED; it is never retried
// in place. Rollback refuses an ambiguous entry whose bytes outside `write_len` changed, because
// those bytes were never owned by this transaction and rewriting its prefix cannot produce an
// exact restoration.
int gtav_patch_journal_next_install(const GtavPatchJournalPlan* plan,
                                    const GtavPatchJournal* journal, uint64_t instance_token,
                                    const GtavPatchJournalSnapshot* snapshot,
                                    GtavPatchJournalAction* action);
int gtav_patch_journal_next_rollback(const GtavPatchJournalPlan* plan,
                                     const GtavPatchJournal* journal, uint64_t instance_token,
                                     const GtavPatchJournalSnapshot* snapshot,
                                     GtavPatchJournalAction* action);

#ifdef __cplusplus
}
#endif
