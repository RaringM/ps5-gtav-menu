#include "gtavmenu/patch_journal_plan.h"

#include <limits.h>
#include <string.h>

static uint32_t plan_mask(uint32_t count) {
  return count >= 32u ? UINT32_MAX : ((1u << count) - 1u);
}

static int prefix_count(uint32_t mask, uint32_t count, uint32_t* out) {
  uint32_t used = mask & plan_mask(count);
  uint32_t n = 0;
  if (mask != used) return -1;
  while (n < count && (used & (1u << n))) ++n;
  if (used != plan_mask(n)) return -1;
  if (out) *out = n;
  return 0;
}

static int range_end(uint64_t address, uint32_t length, uint64_t* end) {
  if (!address || !length || address > UINT64_MAX - length) return -1;
  *end = address + length;
  return 0;
}

static int ranges_overlap(uint64_t a, uint32_t a_len, uint64_t b, uint32_t b_len) {
  uint64_t a_end, b_end;
  if (range_end(a, a_len, &a_end) != 0 || range_end(b, b_len, &b_end) != 0) return 1;
  return a < b_end && b < a_end;
}

static int rel32_target(uint64_t site, const uint8_t bytes[GTAV_PATCH_JOURNAL_REL32_CALL_SIZE],
                        uint64_t* target) {
  uint32_t raw;
  int64_t displacement;
  uint64_t next;
  if (!bytes || !target || bytes[0] != 0xe8 || site > UINT64_MAX - 5u) return -1;
  raw = (uint32_t)bytes[1] | ((uint32_t)bytes[2] << 8u) | ((uint32_t)bytes[3] << 16u) |
        ((uint32_t)bytes[4] << 24u);
  displacement = (int64_t)(int32_t)raw;
  next = site + 5u;
  if (displacement >= 0) {
    if ((uint64_t)displacement > UINT64_MAX - next) return -1;
    *target = next + (uint64_t)displacement;
  } else {
    uint64_t magnitude = (uint64_t)(-displacement);
    if (magnitude > next) return -1;
    *target = next - magnitude;
  }
  return 0;
}

static uint64_t abs_jump_target(const uint8_t bytes[GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE]) {
  uint64_t target = 0;
  uint32_t i;
  if (!bytes || bytes[0] != 0xff || bytes[1] != 0x25 || bytes[2] || bytes[3] || bytes[4] ||
      bytes[5])
    return 0;
  for (i = 0; i < 8u; ++i) target |= (uint64_t)bytes[6u + i] << (i * 8u);
  return target;
}

void gtav_patch_journal_plan_init(GtavPatchJournalPlan* plan) {
  if (plan) memset(plan, 0, sizeof(*plan));
}

int gtav_patch_journal_encode_rel32_call(uint64_t site, uint64_t destination,
                                         uint8_t out[GTAV_PATCH_JOURNAL_REL32_CALL_SIZE]) {
  uint64_t next;
  int64_t displacement;
  uint32_t raw;
  uint32_t i;
  if (!out || site > UINT64_MAX - GTAV_PATCH_JOURNAL_REL32_CALL_SIZE) return -1;
  next = site + GTAV_PATCH_JOURNAL_REL32_CALL_SIZE;
  if (destination >= next) {
    uint64_t distance = destination - next;
    if (distance > (uint64_t)INT32_MAX) return -1;
    displacement = (int64_t)distance;
  } else {
    uint64_t distance = next - destination;
    if (distance > (uint64_t)INT32_MAX + 1u) return -1;
    displacement = -(int64_t)distance;
  }
  raw = (uint32_t)(int32_t)displacement;
  out[0] = 0xe8;
  for (i = 0; i < 4u; ++i) out[i + 1u] = (uint8_t)(raw >> (i * 8u));
  return 0;
}

void gtav_patch_journal_encode_abs_jump(uint64_t destination,
                                        uint8_t out[GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE]) {
  uint32_t i;
  if (!out) return;
  memset(out, 0, GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE);
  out[0] = 0xff;
  out[1] = 0x25;
  for (i = 0; i < 8u; ++i) out[6u + i] = (uint8_t)(destination >> (i * 8u));
}

static int append_entry(GtavPatchJournalPlan* plan, const GtavPatchJournalEntry* entry) {
  uint32_t i;
  uint32_t earlier;
  if (!plan || !entry || plan->count >= GTAV_PATCH_JOURNAL_MAX_ENTRIES) return -1;
  earlier = plan_mask(plan->count);
  if (entry->requires_installed_mask & ~earlier) return -1;
  for (i = 0; i < plan->count; ++i) {
    if (ranges_overlap(entry->address, entry->verify_len, plan->entries[i].address,
                       plan->entries[i].verify_len))
      return -1;
  }
  plan->entries[plan->count++] = *entry;
  return 0;
}

int gtav_patch_journal_add_abs_jump_bridge(GtavPatchJournalPlan* plan, uint64_t address,
                                           const uint8_t original[GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE],
                                           uint64_t destination) {
  GtavPatchJournalEntry entry;
  if (!plan || !original || !address || !destination) return -1;
  memset(&entry, 0, sizeof(entry));
  entry.address = address;
  entry.verify_len = GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE;
  entry.write_len = GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE;
  entry.kind = GTAV_PATCH_JOURNAL_ENTRY_ABS_JUMP_BRIDGE;
  memcpy(entry.original, original, GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE);
  gtav_patch_journal_encode_abs_jump(destination, entry.installed);
  if (memcmp(entry.original, entry.installed, entry.write_len) == 0) return -1;
  return append_entry(plan, &entry);
}

int gtav_patch_journal_add_rel32_call(GtavPatchJournalPlan* plan, uint64_t site,
                                      const uint8_t* original_window, uint32_t window_len,
                                      uint64_t expected_original_target, uint64_t bridge_address,
                                      uint32_t bridge_index) {
  GtavPatchJournalEntry entry;
  uint64_t decoded = 0;
  if (!plan || !original_window || window_len < GTAV_PATCH_JOURNAL_REL32_CALL_SIZE ||
      window_len > GTAV_PATCH_JOURNAL_MAX_BYTES || bridge_index >= plan->count ||
      plan->entries[bridge_index].kind != GTAV_PATCH_JOURNAL_ENTRY_ABS_JUMP_BRIDGE ||
      plan->entries[bridge_index].address != bridge_address ||
      rel32_target(site, original_window, &decoded) != 0 || decoded != expected_original_target) {
    return -1;
  }
  memset(&entry, 0, sizeof(entry));
  entry.address = site;
  entry.verify_len = window_len;
  entry.write_len = GTAV_PATCH_JOURNAL_REL32_CALL_SIZE;
  entry.requires_installed_mask = 1u << bridge_index;
  entry.kind = GTAV_PATCH_JOURNAL_ENTRY_REL32_CALL;
  memcpy(entry.original, original_window, window_len);
  memcpy(entry.installed, original_window, window_len);
  if (gtav_patch_journal_encode_rel32_call(site, bridge_address, entry.installed) != 0 ||
      memcmp(entry.original, entry.installed, entry.write_len) == 0) {
    return -1;
  }
  return append_entry(plan, &entry);
}

int gtav_patch_journal_plan_validate(const GtavPatchJournalPlan* plan) {
  uint32_t i, j;
  if (!plan || !plan->count || plan->count > GTAV_PATCH_JOURNAL_MAX_ENTRIES || plan->reserved)
    return -1;
  for (i = 0; i < plan->count; ++i) {
    const GtavPatchJournalEntry* entry = &plan->entries[i];
    uint64_t end;
    if (range_end(entry->address, entry->verify_len, &end) != 0 || entry->write_len == 0 ||
        entry->write_len > entry->verify_len || entry->verify_len > GTAV_PATCH_JOURNAL_MAX_BYTES ||
        (entry->requires_installed_mask & ~plan_mask(i)) != 0 ||
        memcmp(entry->original, entry->installed, entry->write_len) == 0 ||
        memcmp(entry->original + entry->write_len, entry->installed + entry->write_len,
               entry->verify_len - entry->write_len) != 0) {
      return -1;
    }
    (void)end;
    if (entry->kind == GTAV_PATCH_JOURNAL_ENTRY_ABS_JUMP_BRIDGE) {
      if (entry->write_len != GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE ||
          entry->verify_len != GTAV_PATCH_JOURNAL_ABS_JUMP_SIZE || entry->requires_installed_mask ||
          !abs_jump_target(entry->installed))
        return -1;
    } else if (entry->kind == GTAV_PATCH_JOURNAL_ENTRY_REL32_CALL) {
      uint64_t target = 0;
      uint32_t dependency = 0;
      uint32_t mask = entry->requires_installed_mask;
      while (dependency < i && (mask & (1u << dependency)) == 0) ++dependency;
      if (entry->write_len != GTAV_PATCH_JOURNAL_REL32_CALL_SIZE || entry->installed[0] != 0xe8 ||
          entry->original[0] != 0xe8 || !mask || (mask & (mask - 1u)) != 0 || dependency >= i ||
          plan->entries[dependency].kind != GTAV_PATCH_JOURNAL_ENTRY_ABS_JUMP_BRIDGE ||
          rel32_target(entry->address, entry->installed, &target) != 0 ||
          target != plan->entries[dependency].address)
        return -1;
    } else {
      return -1;
    }
    for (j = 0; j < i; ++j) {
      if (ranges_overlap(entry->address, entry->verify_len, plan->entries[j].address,
                         plan->entries[j].verify_len))
        return -1;
    }
  }
  return 0;
}

static void hash_byte(uint64_t* hash, uint8_t value) {
  *hash ^= value;
  *hash *= 1099511628211ull;
}

static void hash_u32(uint64_t* hash, uint32_t value) {
  uint32_t i;
  for (i = 0; i < 4u; ++i) hash_byte(hash, (uint8_t)(value >> (i * 8u)));
}

static void hash_u64(uint64_t* hash, uint64_t value) {
  uint32_t i;
  for (i = 0; i < 8u; ++i) hash_byte(hash, (uint8_t)(value >> (i * 8u)));
}

uint64_t gtav_patch_journal_plan_fingerprint(const GtavPatchJournalPlan* plan) {
  uint64_t hash = 14695981039346656037ull;
  uint32_t i, j;
  if (gtav_patch_journal_plan_validate(plan) != 0) return 0;
  hash_u32(&hash, plan->count);
  for (i = 0; i < plan->count; ++i) {
    const GtavPatchJournalEntry* entry = &plan->entries[i];
    hash_u64(&hash, entry->address);
    hash_u32(&hash, entry->verify_len);
    hash_u32(&hash, entry->write_len);
    hash_u32(&hash, entry->requires_installed_mask);
    hash_u32(&hash, entry->kind);
    for (j = 0; j < entry->verify_len; ++j) hash_byte(&hash, entry->original[j]);
    for (j = 0; j < entry->verify_len; ++j) hash_byte(&hash, entry->installed[j]);
  }
  return hash ? hash : 1u;
}

GtavPatchJournalImageState gtav_patch_journal_classify(const GtavPatchJournalEntry* entry,
                                                       const uint8_t* observed,
                                                       size_t observed_size) {
  if (!entry || !observed || observed_size != entry->verify_len || !entry->verify_len ||
      entry->verify_len > GTAV_PATCH_JOURNAL_MAX_BYTES)
    return GTAV_PATCH_JOURNAL_IMAGE_INVALID;
  if (memcmp(observed, entry->original, entry->verify_len) == 0)
    return GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL;
  if (memcmp(observed, entry->installed, entry->verify_len) == 0)
    return GTAV_PATCH_JOURNAL_IMAGE_INSTALLED;
  return GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS;
}

int gtav_patch_journal_snapshot(const GtavPatchJournalPlan* plan,
                                const GtavPatchJournalObservation* observations,
                                uint32_t observation_count, GtavPatchJournalSnapshot* snapshot) {
  uint32_t i;
  if (gtav_patch_journal_plan_validate(plan) != 0 || !observations || !snapshot ||
      observation_count != plan->count)
    return -1;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->count = plan->count;
  for (i = 0; i < plan->count; ++i) {
    const GtavPatchJournalEntry* entry = &plan->entries[i];
    snapshot->state[i] =
        gtav_patch_journal_classify(entry, observations[i].bytes, observations[i].size);
    if (observations[i].bytes && observations[i].size == entry->verify_len &&
        (entry->write_len == entry->verify_len ||
         memcmp(observations[i].bytes + entry->write_len, entry->original + entry->write_len,
                entry->verify_len - entry->write_len) == 0)) {
      snapshot->rollback_safe_mask |= 1u << i;
    }
  }
  return 0;
}

int gtav_patch_journal_begin(const GtavPatchJournalPlan* plan, uint64_t instance_token,
                             GtavPatchJournal* journal) {
  uint64_t fingerprint;
  if (!journal || !instance_token || (fingerprint = gtav_patch_journal_plan_fingerprint(plan)) == 0)
    return -1;
  memset(journal, 0, sizeof(*journal));
  journal->magic = GTAV_PATCH_JOURNAL_MAGIC;
  journal->abi_version = GTAV_PATCH_JOURNAL_ABI_VERSION;
  journal->struct_size = (uint32_t)sizeof(*journal);
  journal->instance_token = instance_token;
  journal->plan_fingerprint = fingerprint;
  journal->phase = GTAV_PATCH_JOURNAL_PHASE_PREPARED;
  return 0;
}

int gtav_patch_journal_matches(const GtavPatchJournalPlan* plan, const GtavPatchJournal* journal,
                               uint64_t instance_token) {
  uint32_t attempts;
  uint64_t fingerprint;
  if (!journal || journal->magic != GTAV_PATCH_JOURNAL_MAGIC ||
      journal->abi_version != GTAV_PATCH_JOURNAL_ABI_VERSION ||
      journal->struct_size != sizeof(*journal) || !instance_token ||
      journal->instance_token != instance_token ||
      journal->phase < GTAV_PATCH_JOURNAL_PHASE_PREPARED ||
      journal->phase > GTAV_PATCH_JOURNAL_PHASE_RESTORED ||
      (fingerprint = gtav_patch_journal_plan_fingerprint(plan)) == 0 ||
      journal->plan_fingerprint != fingerprint ||
      prefix_count(journal->attempted_mask, plan->count, &attempts) != 0) {
    return 0;
  }
  (void)attempts;
  return 1;
}

static int snapshot_valid(const GtavPatchJournalPlan* plan,
                          const GtavPatchJournalSnapshot* snapshot) {
  uint32_t i;
  if (!snapshot || snapshot->count != plan->count ||
      (snapshot->rollback_safe_mask & ~plan_mask(plan->count)) != 0)
    return 0;
  for (i = 0; i < plan->count; ++i) {
    if (snapshot->state[i] < GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL ||
        snapshot->state[i] > GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS)
      return 0;
  }
  return 1;
}

static int all_state(const GtavPatchJournalPlan* plan, const GtavPatchJournalSnapshot* snapshot,
                     uint32_t state) {
  uint32_t i;
  if (!snapshot_valid(plan, snapshot)) return 0;
  for (i = 0; i < plan->count; ++i) {
    if (snapshot->state[i] != state) return 0;
  }
  return 1;
}

int gtav_patch_journal_note_install_attempt(const GtavPatchJournalPlan* plan,
                                            GtavPatchJournal* journal, uint64_t instance_token,
                                            const GtavPatchJournalSnapshot* snapshot,
                                            uint32_t index) {
  uint32_t attempts;
  GtavPatchJournalAction action;
  if (!gtav_patch_journal_matches(plan, journal, instance_token) ||
      (journal->phase != GTAV_PATCH_JOURNAL_PHASE_PREPARED &&
       journal->phase != GTAV_PATCH_JOURNAL_PHASE_INSTALLING) ||
      prefix_count(journal->attempted_mask, plan->count, &attempts) != 0 || index != attempts ||
      index >= plan->count ||
      gtav_patch_journal_next_install(plan, journal, instance_token, snapshot, &action) != 0 ||
      action.kind != GTAV_PATCH_JOURNAL_ACTION_WRITE_INSTALLED || action.index != index) {
    return -1;
  }
  journal->attempted_mask |= 1u << index;
  journal->phase = GTAV_PATCH_JOURNAL_PHASE_INSTALLING;
  return 0;
}

int gtav_patch_journal_note_installed(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                      uint64_t instance_token,
                                      const GtavPatchJournalSnapshot* snapshot) {
  if (!gtav_patch_journal_matches(plan, journal, instance_token) ||
      journal->attempted_mask != plan_mask(plan->count) ||
      !all_state(plan, snapshot, GTAV_PATCH_JOURNAL_IMAGE_INSTALLED))
    return -1;
  journal->phase = GTAV_PATCH_JOURNAL_PHASE_INSTALLED;
  return 0;
}

int gtav_patch_journal_begin_rollback(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                      uint64_t instance_token) {
  if (!gtav_patch_journal_matches(plan, journal, instance_token) ||
      journal->phase == GTAV_PATCH_JOURNAL_PHASE_RESTORED)
    return -1;
  journal->phase = GTAV_PATCH_JOURNAL_PHASE_ROLLING_BACK;
  return 0;
}

int gtav_patch_journal_note_restored(const GtavPatchJournalPlan* plan, GtavPatchJournal* journal,
                                     uint64_t instance_token,
                                     const GtavPatchJournalSnapshot* snapshot) {
  if (!gtav_patch_journal_matches(plan, journal, instance_token) ||
      journal->phase != GTAV_PATCH_JOURNAL_PHASE_ROLLING_BACK ||
      !all_state(plan, snapshot, GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL))
    return -1;
  journal->phase = GTAV_PATCH_JOURNAL_PHASE_RESTORED;
  return 0;
}

static void action_clear(GtavPatchJournalAction* action, uint32_t kind) {
  memset(action, 0, sizeof(*action));
  action->kind = kind;
}

static void action_write(const GtavPatchJournalPlan* plan, uint32_t index, uint32_t kind,
                         GtavPatchJournalAction* action) {
  const GtavPatchJournalEntry* entry = &plan->entries[index];
  const uint8_t* image =
      kind == GTAV_PATCH_JOURNAL_ACTION_WRITE_INSTALLED ? entry->installed : entry->original;
  action_clear(action, kind);
  action->index = index;
  action->address = entry->address;
  action->write_len = entry->write_len;
  action->verify_len = entry->verify_len;
  memcpy(action->write, image, entry->write_len);
  memcpy(action->verify, image, entry->verify_len);
}

int gtav_patch_journal_next_install(const GtavPatchJournalPlan* plan,
                                    const GtavPatchJournal* journal, uint64_t instance_token,
                                    const GtavPatchJournalSnapshot* snapshot,
                                    GtavPatchJournalAction* action) {
  uint32_t attempts, installed = 0, i;
  if (!action || !gtav_patch_journal_matches(plan, journal, instance_token) ||
      !snapshot_valid(plan, snapshot) ||
      (journal->phase != GTAV_PATCH_JOURNAL_PHASE_PREPARED &&
       journal->phase != GTAV_PATCH_JOURNAL_PHASE_INSTALLING))
    return -1;
  action_clear(action, GTAV_PATCH_JOURNAL_ACTION_REFUSE);
  if (prefix_count(journal->attempted_mask, plan->count, &attempts) != 0) return -1;

  while (installed < plan->count &&
         snapshot->state[installed] == GTAV_PATCH_JOURNAL_IMAGE_INSTALLED) {
    if ((journal->attempted_mask & (1u << installed)) == 0) return 0;
    ++installed;
  }
  for (i = installed + 1u; i < plan->count; ++i) {
    if (snapshot->state[i] != GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL) return 0;
  }
  if (installed == plan->count) {
    if (attempts != plan->count) return 0;
    action_clear(action, GTAV_PATCH_JOURNAL_ACTION_COMPLETE);
    return 0;
  }

  if (snapshot->state[installed] == GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS) {
    if (attempts == installed + 1u)
      action_clear(action, GTAV_PATCH_JOURNAL_ACTION_ROLLBACK_REQUIRED);
    return 0;
  }
  if (snapshot->state[installed] != GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL) return 0;
  if (attempts > installed) {
    action_clear(action, GTAV_PATCH_JOURNAL_ACTION_ROLLBACK_REQUIRED);
    return 0;
  }
  if (attempts != installed ||
      (plan->entries[installed].requires_installed_mask & ~journal->attempted_mask) != 0)
    return 0;
  action_write(plan, installed, GTAV_PATCH_JOURNAL_ACTION_WRITE_INSTALLED, action);
  return 0;
}

int gtav_patch_journal_next_rollback(const GtavPatchJournalPlan* plan,
                                     const GtavPatchJournal* journal, uint64_t instance_token,
                                     const GtavPatchJournalSnapshot* snapshot,
                                     GtavPatchJournalAction* action) {
  uint32_t attempts, i;
  int highest = -1;
  if (!action || !gtav_patch_journal_matches(plan, journal, instance_token) ||
      journal->phase != GTAV_PATCH_JOURNAL_PHASE_ROLLING_BACK || !snapshot_valid(plan, snapshot))
    return -1;
  action_clear(action, GTAV_PATCH_JOURNAL_ACTION_REFUSE);
  if (prefix_count(journal->attempted_mask, plan->count, &attempts) != 0) return -1;

  for (i = attempts; i < plan->count; ++i) {
    if (snapshot->state[i] != GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL) return 0;
  }
  for (i = 0; i < attempts; ++i) {
    if (snapshot->state[i] != GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL) highest = (int)i;
  }
  if (highest < 0) {
    action_clear(action, GTAV_PATCH_JOURNAL_ACTION_COMPLETE);
    return 0;
  }
  for (i = 0; i < (uint32_t)highest; ++i) {
    if (snapshot->state[i] != GTAV_PATCH_JOURNAL_IMAGE_INSTALLED) return 0;
  }
  for (i = (uint32_t)highest + 1u; i < attempts; ++i) {
    if (snapshot->state[i] != GTAV_PATCH_JOURNAL_IMAGE_ORIGINAL) return 0;
  }
  if (snapshot->state[highest] != GTAV_PATCH_JOURNAL_IMAGE_INSTALLED &&
      snapshot->state[highest] != GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS)
    return 0;
  if (snapshot->state[highest] == GTAV_PATCH_JOURNAL_IMAGE_AMBIGUOUS &&
      (snapshot->rollback_safe_mask & (1u << highest)) == 0)
    return 0;
  action_write(plan, (uint32_t)highest, GTAV_PATCH_JOURNAL_ACTION_WRITE_ORIGINAL, action);
  return 0;
}
