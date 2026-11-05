// Data-method boot-pool growth (see gtavmenu/boot_pool_data.h). Freestanding: snprintf, memset and
// memcpy are all it needs from the C library.

#include "gtavmenu/boot_pool_data.h"

#include <stdio.h>
#include <string.h>

enum {
  kFound = 1,
  kAbsent = 0,
  kUnreadable = -1,  // a pointer on the way could not be read, or a chain ran past its limit
  kLayout = -2,      // the block map is not laid out as pinned
};

static int read_u8(const GtavBootPoolDataOps* ops, uint64_t address, uint32_t* out) {
  uint8_t value = 0;
  if (ops->read(ops->context, address, &value, sizeof(value)) != 0) return -1;
  *out = value;
  return 0;
}

static int read_u16(const GtavBootPoolDataOps* ops, uint64_t address, uint32_t* out) {
  uint16_t value = 0;
  if (ops->read(ops->context, address, &value, sizeof(value)) != 0) return -1;
  *out = value;
  return 0;
}

static int read_u32(const GtavBootPoolDataOps* ops, uint64_t address, uint32_t* out) {
  uint32_t value = 0;
  if (ops->read(ops->context, address, &value, sizeof(value)) != 0) return -1;
  *out = value;
  return 0;
}

static int read_u64(const GtavBootPoolDataOps* ops, uint64_t address, uint64_t* out) {
  uint64_t value = 0;
  if (ops->read(ops->context, address, &value, sizeof(value)) != 0) return -1;
  *out = value;
  return 0;
}

static int refusing(uint8_t result) {
  return result >= GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED &&
         result <= GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED;
}

static int not_yet(uint8_t result) {
  return result >= GTAV_BOOT_POOL_DATA_ENTRY_NO_MANAGER &&
         result <= GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
}

const char* gtav_boot_pool_data_mode_name(int mode) {
  switch (mode) {
    case GTAV_BOOT_POOL_DATA_MODE_OFF:
      return "off";
    case GTAV_BOOT_POOL_DATA_MODE_OBSERVE:
      return "observe";
    case GTAV_BOOT_POOL_DATA_MODE_CAPS:
      return "caps";
    case GTAV_BOOT_POOL_DATA_MODE_FULL:
      return "full";
    default:
      return "unknown";
  }
}

const char* gtav_boot_pool_data_window_name(int window) {
  switch (window) {
    case GTAV_BOOT_POOL_DATA_WINDOW_NONE:
      return "none";
    case GTAV_BOOT_POOL_DATA_WINDOW_WAIT:
      return "wait";
    case GTAV_BOOT_POOL_DATA_WINDOW_OPEN:
      return "open";
    case GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY:
      return "caps-only";
    case GTAV_BOOT_POOL_DATA_WINDOW_CLOSED:
      return "closed";
    case GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE:
      return "unreadable";
    default:
      return "unknown";
  }
}

const char* gtav_boot_pool_data_status_name(int status) {
  switch (status) {
    case GTAV_BOOT_POOL_DATA_PENDING:
      return "pending";
    case GTAV_BOOT_POOL_DATA_OFF:
      return "off";
    case GTAV_BOOT_POOL_DATA_OBSERVED:
      return "observed";
    case GTAV_BOOT_POOL_DATA_APPLIED:
      return "applied";
    case GTAV_BOOT_POOL_DATA_RACED:
      return "raced";
    case GTAV_BOOT_POOL_DATA_MISSED:
      return "missed";
    case GTAV_BOOT_POOL_DATA_REFUSED:
      return "refused";
    case GTAV_BOOT_POOL_DATA_TIMEOUT:
      return "timeout";
    case GTAV_BOOT_POOL_DATA_ABORTED:
      return "aborted";
    default:
      return "unknown";
  }
}

const char* gtav_boot_pool_data_entry_name(int result) {
  switch (result) {
    case GTAV_BOOT_POOL_DATA_ENTRY_NOT_RUN:
      return "not-run";
    case GTAV_BOOT_POOL_DATA_ENTRY_SKIPPED:
      return "skipped";
    case GTAV_BOOT_POOL_DATA_ENTRY_STOCK:
      return "stock";
    case GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN:
      return "written";
    case GTAV_BOOT_POOL_DATA_ENTRY_NO_MANAGER:
      return "no-manager";
    case GTAV_BOOT_POOL_DATA_ENTRY_NO_BUCKETS:
      return "no-buckets";
    case GTAV_BOOT_POOL_DATA_ENTRY_NO_CFG:
      return "no-cfg";
    case GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE:
      return "unreadable";
    case GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED:
      return "misaligned";
    case GTAV_BOOT_POOL_DATA_ENTRY_NOT_FOUND:
      return "not-found";
    case GTAV_BOOT_POOL_DATA_ENTRY_CHAIN_LIMIT:
      return "chain-limit";
    case GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED:
      return "unexpected";
    case GTAV_BOOT_POOL_DATA_ENTRY_CHANGED:
      return "changed";
    case GTAV_BOOT_POOL_DATA_ENTRY_CAS_REFUSED:
      return "cas-refused";
    case GTAV_BOOT_POOL_DATA_ENTRY_ROLLED_BACK:
      return "rolled-back";
    case GTAV_BOOT_POOL_DATA_ENTRY_ROLLBACK_FAILED:
      return "rollback-failed";
    case GTAV_BOOT_POOL_DATA_ENTRY_RECONCILED:
      return "reconciled";
    default:
      return "unknown";
  }
}

// The storage block of one model-info factory: kFound (*block set), kAbsent while the factory has
// no block map or no block yet, kUnreadable when a pointer on the way cannot be read or is not
// aligned or a chain runs past chain_max, kLayout when the map itself is not the pinned layout.
// Every bucket is walked: the block key's bucket is not derived from the key.
static int find_factory_block(const GtavBootPoolDataProfile* profile,
                              const GtavBootPoolDataOps* ops, uint64_t factory, uint64_t* block) {
  uint64_t buckets = 0;
  uint32_t slots = 0;
  uint32_t bucket;
  if (read_u64(ops, factory + profile->model_factory_buckets, &buckets) != 0 ||
      read_u16(ops, factory + profile->model_factory_slots, &slots) != 0) {
    return kUnreadable;
  }
  if (!buckets || !slots) return kAbsent;
  if ((buckets & 7u) != 0 || slots > GTAV_BOOT_POOL_DATA_FACTORY_SLOTS_MAX) return kLayout;
  for (bucket = 0; bucket < slots; ++bucket) {
    uint64_t node = 0;
    uint32_t depth;
    if (read_u64(ops, buckets + 8u * bucket, &node) != 0) return kUnreadable;
    for (depth = 0; node && depth < profile->chain_max; ++depth) {
      uint32_t key = 0;
      if ((node & 7u) != 0 || read_u32(ops, node, &key) != 0) return kUnreadable;
      if (key == (uint32_t)profile->model_factory_block_key) {
        uint64_t value = 0;
        if (read_u64(ops, node + profile->model_factory_block_value, &value) != 0) {
          return kUnreadable;
        }
        if (!value) return kAbsent;
        *block = value;
        return kFound;
      }
      if (read_u64(ops, node + profile->model_factory_node_next, &node) != 0) return kUnreadable;
    }
    if (node) return kUnreadable;  // the chain goes on past chain_max: not proven absent
  }
  return kAbsent;
}

int gtav_boot_pool_data_sample(const GtavBootPoolDataProfile* profile,
                               const GtavBootPoolDataOps* ops, GtavBootPoolDataSample* out) {
  uint32_t i;
  int unreadable = 0;
  memset(out, 0, sizeof(*out));
  out->window = GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE;
  if (!profile || !ops || !ops->read) return out->window;
  if (read_u16(ops, profile->fsm_state, &out->fsm_state) != 0 ||
      read_u64(ops, profile->config_manager, &out->manager) != 0 ||
      read_u64(ops, profile->store_init_object, &out->store_init_object) != 0 ||
      read_u8(ops, profile->first_store_sized, &out->first_store_sized) != 0) {
    return out->window;
  }
  for (i = 0; i < GTAV_BOOT_POOL_DATA_FACTORIES; ++i) {
    uint64_t block = 0;
    const int found = find_factory_block(profile, ops, profile->factories[i], &block);
    if (found == kFound) out->factories |= 1u << i;
    if (found == kUnreadable) unreadable = 1;
    if (found == kLayout) out->anomalies |= 1u << i;
  }
  if (out->factories) {
    out->window = GTAV_BOOT_POOL_DATA_WINDOW_CLOSED;
  } else if (unreadable || out->anomalies) {
    out->window = GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE;
  } else if (out->store_init_object || out->first_store_sized ||
             (out->fsm_state > 1u && out->manager)) {
    // A sentinel says the stores are being sized, or the boot is past streaming init.
    out->window = GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY;
  } else if (out->fsm_state == 1u && out->manager) {
    out->window = GTAV_BOOT_POOL_DATA_WINDOW_OPEN;
  } else {
    out->window = GTAV_BOOT_POOL_DATA_WINDOW_WAIT;
  }
  return out->window;
}

int gtav_boot_pool_data_plan(const GtavBootPoolDataProfile* profile, int mode,
                             GtavBootPoolDataReport* report) {
  GtavBootPoolProfile growth;
  GtavBootPoolEntry entries[GTAV_BOOT_POOL_ENTRIES_MAX];
  uint32_t i;
  if (!report) return -1;
  memset(report, 0, sizeof(*report));
  report->mode = mode;
  report->first_window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
  report->window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
  report->high_window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
  if (!profile || mode < GTAV_BOOT_POOL_DATA_MODE_OFF || mode > GTAV_BOOT_POOL_DATA_MODE_FULL ||
      profile->chain_max == 0 || !profile->verify) {
    return -1;
  }
  // The shared growth-only rule: stock > 0 and new > stock for every row.
  memset(&growth, 0, sizeof(growth));
  growth.overrides = profile->overrides;
  growth.override_count = profile->override_count;
  if (gtav_boot_pool_entries(&growth, entries, GTAV_BOOT_POOL_ENTRIES_MAX) == 0) return -1;
  report->count = profile->override_count;
  for (i = 0; i < profile->override_count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    int planned;
    if (o->kind == GTAV_BOOT_POOL_KIND_CFG) {
      if ((o->key & 3u) != 0 || o->key >= GTAV_BOOT_POOL_CFG_OFFSET_MAX) return -1;
    } else if (o->kind != GTAV_BOOT_POOL_KIND_NODE) {
      return -1;
    }
    planned = mode == GTAV_BOOT_POOL_DATA_MODE_OBSERVE || mode == GTAV_BOOT_POOL_DATA_MODE_FULL ||
              (mode == GTAV_BOOT_POOL_DATA_MODE_CAPS && o->kind == GTAV_BOOT_POOL_KIND_CFG);
    report->entries[i].planned = (uint8_t)planned;
    report->entries[i].result =
        planned ? GTAV_BOOT_POOL_DATA_ENTRY_NOT_RUN : GTAV_BOOT_POOL_DATA_ENTRY_SKIPPED;
    if (planned) ++report->planned;
  }
  return 0;
}

// The value word of a PoolSizes node: its bucket is key % count, then a bounded chain walk.
static uint8_t find_node(const GtavBootPoolDataProfile* profile, const GtavBootPoolDataOps* ops,
                         uint64_t buckets, uint32_t slots, uint32_t key,
                         GtavBootPoolDataEntry* entry) {
  uint64_t node = 0;
  uint32_t depth;
  if (read_u64(ops, buckets + 8u * (uint64_t)(key % slots), &node) != 0) {
    return GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
  }
  for (depth = 1; depth <= profile->chain_max; ++depth) {
    uint32_t seen = 0;
    if (!node) return GTAV_BOOT_POOL_DATA_ENTRY_NOT_FOUND;
    if ((node & 7u) != 0) return GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED;
    if (read_u32(ops, node, &seen) != 0) return GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
    if (seen == key) {
      entry->address = node + profile->config_node_value;
      entry->depth = (uint8_t)(depth < 255u ? depth : 255u);
      return GTAV_BOOT_POOL_DATA_ENTRY_STOCK;
    }
    if (read_u64(ops, node + profile->config_node_next, &node) != 0) {
      return GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
    }
  }
  return GTAV_BOOT_POOL_DATA_ENTRY_CHAIN_LIMIT;
}

int gtav_boot_pool_data_resolve(const GtavBootPoolDataProfile* profile,
                                const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report) {
  uint64_t manager = 0;
  uint64_t buckets = 0;
  uint64_t cfg = 0;
  uint32_t slots = 0;
  uint8_t manager_result = GTAV_BOOT_POOL_DATA_ENTRY_STOCK;
  uint8_t map_result = GTAV_BOOT_POOL_DATA_ENTRY_STOCK;
  uint8_t cfg_result = GTAV_BOOT_POOL_DATA_ENTRY_STOCK;
  int any_node = 0;
  int any_cfg = 0;
  int refused = 0;
  int pending = 0;
  uint32_t i;
  if (!profile || !ops || !ops->read || !report || report->planned == 0) return -1;
  ++report->resolves;
  for (i = 0; i < report->count; ++i) {
    if (!report->entries[i].planned) continue;
    if (profile->overrides[i].kind == GTAV_BOOT_POOL_KIND_NODE) any_node = 1;
    if (profile->overrides[i].kind == GTAV_BOOT_POOL_KIND_CFG) any_cfg = 1;
  }
  if (read_u64(ops, profile->config_manager, &manager) != 0) {
    manager_result = GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
  } else if (!manager) {
    manager_result = GTAV_BOOT_POOL_DATA_ENTRY_NO_MANAGER;
  } else if ((manager & 7u) != 0) {
    manager_result = GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED;
  }
  if (manager_result == GTAV_BOOT_POOL_DATA_ENTRY_STOCK && any_node) {
    if (read_u64(ops, manager + profile->config_mgr_buckets, &buckets) != 0 ||
        read_u16(ops, manager + profile->config_mgr_slots, &slots) != 0) {
      map_result = GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
    } else if (!buckets || !slots) {
      map_result = GTAV_BOOT_POOL_DATA_ENTRY_NO_BUCKETS;
    } else if ((buckets & 7u) != 0) {
      map_result = GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED;
    }
  }
  if (manager_result == GTAV_BOOT_POOL_DATA_ENTRY_STOCK && any_cfg) {
    if (read_u64(ops, manager + profile->config_mgr_cfg, &cfg) != 0) {
      cfg_result = GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
    } else if (!cfg) {
      cfg_result = GTAV_BOOT_POOL_DATA_ENTRY_NO_CFG;
    } else if ((cfg & 7u) != 0) {
      cfg_result = GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED;
    }
  }
  for (i = 0; i < report->count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    GtavBootPoolDataEntry* entry = &report->entries[i];
    uint8_t result = GTAV_BOOT_POOL_DATA_ENTRY_STOCK;
    if (!entry->planned) continue;
    entry->address = 0;
    entry->depth = 0;
    entry->observed = 0;
    if (manager_result != GTAV_BOOT_POOL_DATA_ENTRY_STOCK) {
      result = manager_result;
    } else if (o->kind == GTAV_BOOT_POOL_KIND_NODE) {
      result = map_result != GTAV_BOOT_POOL_DATA_ENTRY_STOCK
                   ? map_result
                   : find_node(profile, ops, buckets, slots, o->key, entry);
    } else if (cfg_result != GTAV_BOOT_POOL_DATA_ENTRY_STOCK) {
      result = cfg_result;
    } else {
      entry->address = cfg + o->key;
    }
    if (result == GTAV_BOOT_POOL_DATA_ENTRY_STOCK) {
      uint32_t value = 0;
      if (read_u32(ops, entry->address, &value) != 0) {
        result = GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE;
      } else {
        entry->observed = (int32_t)value;
        if (entry->observed != o->stock) result = GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED;
      }
    }
    entry->result = result;
    if (refusing(result)) refused = 1;
    if (not_yet(result)) pending = 1;
  }
  if (refused) return -1;
  return pending ? 1 : 0;
}

static void roll_back(const GtavBootPoolDataProfile* profile, const GtavBootPoolDataOps* ops,
                      GtavBootPoolDataReport* report, uint32_t failed) {
  uint32_t i = failed;
  while (i-- > 0) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    GtavBootPoolDataEntry* entry = &report->entries[i];
    uint32_t seen = 0;
    if (entry->result != GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN) continue;
    if (ops->cas32(ops->context, entry->address, (uint32_t)o->new_value, (uint32_t)o->stock,
                   &seen) == GTAV_BOOT_POOL_DATA_CAS_SWAPPED) {
      entry->result = GTAV_BOOT_POOL_DATA_ENTRY_ROLLED_BACK;
      --report->written;
    } else {
      entry->result = GTAV_BOOT_POOL_DATA_ENTRY_ROLLBACK_FAILED;
    }
  }
}

int gtav_boot_pool_data_commit(const GtavBootPoolDataProfile* profile,
                               const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report) {
  uint32_t i;
  if (!profile || !ops || !ops->cas32 || !report || report->planned == 0) return -1;
  if (report->mode != GTAV_BOOT_POOL_DATA_MODE_CAPS &&
      report->mode != GTAV_BOOT_POOL_DATA_MODE_FULL) {
    return -1;
  }
  for (i = 0; i < report->count; ++i) {
    const GtavBootPoolDataEntry* entry = &report->entries[i];
    if (entry->planned && (entry->result != GTAV_BOOT_POOL_DATA_ENTRY_STOCK || !entry->address ||
                           (entry->address & 3u) != 0)) {
      return -1;  // not resolved at stock: nothing is written
    }
  }
  for (i = 0; i < report->count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    GtavBootPoolDataEntry* entry = &report->entries[i];
    uint32_t seen = 0;
    int exchanged;
    if (!entry->planned) continue;
    exchanged =
        ops->cas32(ops->context, entry->address, (uint32_t)o->stock, (uint32_t)o->new_value, &seen);
    if (exchanged == GTAV_BOOT_POOL_DATA_CAS_SWAPPED) {
      entry->result = GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN;
      ++report->written;
      continue;
    }
    if (exchanged == GTAV_BOOT_POOL_DATA_CAS_DIFFERS) {
      entry->result = GTAV_BOOT_POOL_DATA_ENTRY_CHANGED;
      entry->observed = (int32_t)seen;
    } else {
      entry->result = GTAV_BOOT_POOL_DATA_ENTRY_CAS_REFUSED;
    }
    roll_back(profile, ops, report, i);
    return -1;
  }
  return 0;
}

static int decide(GtavBootPoolDataReport* report, int status, int window, const char* reason) {
  report->status = status;
  report->window = window;
  report->reason = reason;
  return 1;
}

static const char* missed_reason(int window) {
  return window == GTAV_BOOT_POOL_DATA_WINDOW_CLOSED ? "factories-exist" : "stores-sized";
}

// A classified window, held at the furthest state seen: the boot never moves back, so a sample
// that reads an earlier state (a sentinel that seemed to clear) is taken at the furthest one.
static int forward_window(GtavBootPoolDataReport* report, int window) {
  if (window != GTAV_BOOT_POOL_DATA_WINDOW_OPEN && window != GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY &&
      window != GTAV_BOOT_POOL_DATA_WINDOW_CLOSED) {
    return window;
  }
  if (report->high_window == GTAV_BOOT_POOL_DATA_WINDOW_NONE || window > report->high_window) {
    report->high_window = window;
  }
  return report->high_window;
}

// Why a commit failed: a rollback that could not put every word back, a rollback that did, or the
// first exchange itself ("changed" or "cas-refused"), when nothing was written at all.
static const char* commit_failure(const GtavBootPoolDataReport* report) {
  const char* first = "refused";
  uint32_t i;
  if (report->written) return "rollback-failed";
  for (i = 0; i < report->count; ++i) {
    const uint8_t result = report->entries[i].result;
    if (result == GTAV_BOOT_POOL_DATA_ENTRY_ROLLED_BACK) return "rolled-back";
    if (result == GTAV_BOOT_POOL_DATA_ENTRY_CHANGED ||
        result == GTAV_BOOT_POOL_DATA_ENTRY_CAS_REFUSED) {
      first = gtav_boot_pool_data_entry_name(result);
    }
  }
  return first;
}

// Resolve, re-check the window, commit, re-check again. `allowed(window)` is the mode's window.
static int try_commit(const GtavBootPoolDataProfile* profile, const GtavBootPoolDataOps* ops,
                      GtavBootPoolDataReport* report, int (*allowed)(int window)) {
  GtavBootPoolDataSample again;
  uint32_t i;
  const int resolved = gtav_boot_pool_data_resolve(profile, ops, report);
  if (resolved > 0) return 0;
  if (resolved < 0) {
    for (i = 0; i < report->count; ++i) {
      if (report->entries[i].result == GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED) {
        return decide(report, GTAV_BOOT_POOL_DATA_REFUSED, report->window, "unexpected-value");
      }
    }
    return decide(report, GTAV_BOOT_POOL_DATA_REFUSED, report->window, "unexpected-layout");
  }
  // Right before the commit: a sentinel that flipped since the deciding sample means the stores
  // may already be reading the nodes. An unreadable re-check decides nothing.
  gtav_boot_pool_data_sample(profile, ops, &again);
  if (again.window == GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE) return 0;
  again.window = forward_window(report, again.window);
  if (!allowed(again.window)) {
    // FULL seen open, then sized before any write: the next sample can still take the cfg rows.
    if (report->mode == GTAV_BOOT_POOL_DATA_MODE_FULL && !report->fallback &&
        again.window == GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY) {
      return 0;
    }
    return decide(report, GTAV_BOOT_POOL_DATA_MISSED, again.window, missed_reason(again.window));
  }
  if (gtav_boot_pool_data_commit(profile, ops, report) != 0) {
    return decide(report, GTAV_BOOT_POOL_DATA_REFUSED, again.window, commit_failure(report));
  }
  // The writes stay either way: a store sized meanwhile took the stock or the new value, and the
  // verify step reports which.
  gtav_boot_pool_data_sample(profile, ops, &again);
  if (again.window == GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE) {
    return decide(report, GTAV_BOOT_POOL_DATA_RACED, again.window, "unconfirmed");
  }
  again.window = forward_window(report, again.window);
  if (!allowed(again.window)) {
    return decide(report, GTAV_BOOT_POOL_DATA_RACED, again.window, "sentinel-flipped");
  }
  return decide(report, GTAV_BOOT_POOL_DATA_APPLIED, again.window, NULL);
}

static int full_window(int window) {
  return window == GTAV_BOOT_POOL_DATA_WINDOW_OPEN;
}

static int caps_window(int window) {
  return window == GTAV_BOOT_POOL_DATA_WINDOW_OPEN ||
         window == GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY;
}

// FULL past the store window but before the factories: keep the cfg rows (the model caps) and drop
// the PoolSizes nodes the stores may be reading. Returns how many rows are left.
static uint32_t fall_back_to_caps(const GtavBootPoolDataProfile* profile,
                                  GtavBootPoolDataReport* report) {
  uint32_t i;
  report->fallback = 1;
  report->planned = 0;
  for (i = 0; i < report->count; ++i) {
    GtavBootPoolDataEntry* entry = &report->entries[i];
    if (!entry->planned) continue;
    if (profile->overrides[i].kind == GTAV_BOOT_POOL_KIND_CFG) {
      ++report->planned;
      continue;
    }
    memset(entry, 0, sizeof(*entry));
    entry->result = GTAV_BOOT_POOL_DATA_ENTRY_SKIPPED;
  }
  return report->planned;
}

int gtav_boot_pool_data_step(const GtavBootPoolDataProfile* profile, const GtavBootPoolDataOps* ops,
                             GtavBootPoolDataReport* report, const GtavBootPoolDataSample* sample) {
  int window;
  if (!profile || !ops || !report || !sample) return 0;
  if (report->status != GTAV_BOOT_POOL_DATA_PENDING) return 1;
  if (report->mode == GTAV_BOOT_POOL_DATA_MODE_OFF) {
    return decide(report, GTAV_BOOT_POOL_DATA_OFF, GTAV_BOOT_POOL_DATA_WINDOW_NONE, NULL);
  }
  if (sample->anomalies) {
    return decide(report, GTAV_BOOT_POOL_DATA_REFUSED, sample->window, "factory-layout");
  }
  window = sample->window;
  if (window == GTAV_BOOT_POOL_DATA_WINDOW_WAIT ||
      window == GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE) {
    return 0;
  }
  window = forward_window(report, window);
  if (report->first_window == GTAV_BOOT_POOL_DATA_WINDOW_NONE) report->first_window = window;
  report->window = window;
  switch (report->mode) {
    case GTAV_BOOT_POOL_DATA_MODE_OBSERVE:
      if (!report->observed && gtav_boot_pool_data_resolve(profile, ops, report) <= 0) {
        report->observed = 1;
      }
      if (window == GTAV_BOOT_POOL_DATA_WINDOW_CLOSED) {
        return decide(report, GTAV_BOOT_POOL_DATA_OBSERVED, window, NULL);
      }
      return 0;
    case GTAV_BOOT_POOL_DATA_MODE_CAPS:
      if (!caps_window(window)) {
        return decide(report, GTAV_BOOT_POOL_DATA_MISSED, window, missed_reason(window));
      }
      return try_commit(profile, ops, report, caps_window);
    case GTAV_BOOT_POOL_DATA_MODE_FULL:
      if (window == GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY && !report->fallback &&
          fall_back_to_caps(profile, report) == 0) {
        return decide(report, GTAV_BOOT_POOL_DATA_MISSED, window, missed_reason(window));
      }
      if (report->fallback) {
        if (!caps_window(window)) {
          return decide(report, GTAV_BOOT_POOL_DATA_MISSED, window, missed_reason(window));
        }
        return try_commit(profile, ops, report, caps_window);
      }
      if (!full_window(window)) {
        return decide(report, GTAV_BOOT_POOL_DATA_MISSED, window, missed_reason(window));
      }
      return try_commit(profile, ops, report, full_window);
    default:
      return decide(report, GTAV_BOOT_POOL_DATA_REFUSED, window, "mode");
  }
}

int gtav_boot_pool_data_verify_growth(const GtavBootPoolDataProfile* profile,
                                      const GtavBootPoolDataOps* ops, int what,
                                      GtavBootPoolDataVerify* out) {
  uint32_t i;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  out->what = what;
  if (!profile || !ops || !ops->read || !profile->verify ||
      profile->override_count > GTAV_BOOT_POOL_ENTRIES_MAX) {
    return -1;
  }
  for (i = 0; i < profile->override_count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    const uint64_t object = profile->verify[i];
    if (!object) continue;
    if (what == GTAV_BOOT_POOL_DATA_VERIFY_STORES && o->kind == GTAV_BOOT_POOL_KIND_NODE) {
      uint32_t initialized = 0;
      ++out->checked;
      if (read_u8(ops, object + profile->store_initialized_offset, &initialized) != 0 ||
          (initialized &&
           read_u32(ops, object + profile->store_size_offset, &out->value[i]) != 0)) {
        out->state[i] = GTAV_BOOT_POOL_DATA_VERIFY_UNREADABLE;
        continue;
      }
      if (!initialized) {
        out->state[i] = GTAV_BOOT_POOL_DATA_VERIFY_PENDING;
        ++out->pending;
        continue;
      }
    } else if (what == GTAV_BOOT_POOL_DATA_VERIFY_FACTORIES && o->kind == GTAV_BOOT_POOL_KIND_CFG) {
      uint64_t block = 0;
      ++out->checked;
      if (find_factory_block(profile, ops, object, &block) != kFound ||
          read_u16(ops, block + profile->model_block_count, &out->used[i]) != 0 ||
          read_u16(ops, block + profile->model_block_capacity, &out->value[i]) != 0) {
        out->state[i] = GTAV_BOOT_POOL_DATA_VERIFY_UNREADABLE;
        continue;
      }
    } else {
      continue;
    }
    out->state[i] = GTAV_BOOT_POOL_DATA_VERIFY_READ;
    ++out->read;
    if (out->value[i] == (uint32_t)o->new_value) ++out->grown;
  }
  return out->checked && out->read == out->checked ? 0 : -1;
}

int gtav_boot_pool_data_reconcile(const GtavBootPoolDataProfile* profile,
                                  const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report,
                                  const GtavBootPoolDataVerify* verify) {
  uint32_t i;
  int put_back = 0;
  if (!profile || !ops || !ops->cas32 || !report || !verify ||
      verify->what != GTAV_BOOT_POOL_DATA_VERIFY_STORES) {
    return -1;
  }
  for (i = 0; i < report->count && i < profile->override_count; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    GtavBootPoolDataEntry* entry = &report->entries[i];
    uint32_t seen = 0;
    if (o->kind != GTAV_BOOT_POOL_KIND_NODE || entry->result != GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN ||
        verify->state[i] != GTAV_BOOT_POOL_DATA_VERIFY_READ ||
        verify->value[i] != (uint32_t)o->stock) {
      continue;
    }
    if (ops->cas32(ops->context, entry->address, (uint32_t)o->new_value, (uint32_t)o->stock,
                   &seen) != GTAV_BOOT_POOL_DATA_CAS_SWAPPED) {
      return -1;
    }
    entry->result = GTAV_BOOT_POOL_DATA_ENTRY_RECONCILED;
    --report->written;
    ++put_back;
  }
  return put_back;
}

// snprintf into the tail of `out`; keeps *n within cap - 1.
static void append(size_t cap, size_t* n, int written) {
  if (written < 0) return;
  *n += (size_t)written < cap - *n ? (size_t)written : cap - *n - 1;
}

size_t gtav_boot_pool_data_format(const GtavBootPoolDataProfile* profile,
                                  const GtavBootPoolDataReport* report, char* out, size_t cap) {
  size_t n = 0;
  uint32_t i;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!report) return 0;
  append(
      cap, &n,
      snprintf(out, cap, "boot-pool: mode=%s status=%s written=%u/%u first=%s",
               gtav_boot_pool_data_mode_name(report->mode),
               gtav_boot_pool_data_status_name(report->status), (unsigned)report->written,
               (unsigned)report->planned, gtav_boot_pool_data_window_name(report->first_window)));
  if (report->window != report->first_window && n + 1 < cap) {
    append(
        cap, &n,
        snprintf(out + n, cap - n, " window=%s", gtav_boot_pool_data_window_name(report->window)));
  }
  if (report->fallback && n + 1 < cap) {
    append(cap, &n, snprintf(out + n, cap - n, " fallback=caps"));
  }
  if (report->reason && n + 1 < cap) {
    append(cap, &n, snprintf(out + n, cap - n, " reason=%s", report->reason));
  }
  if (!profile) return n;
  for (i = 0; i < report->count && i < profile->override_count && n + 1 < cap; ++i) {
    const GtavBootPoolOverride* o = &profile->overrides[i];
    const GtavBootPoolDataEntry* e = &report->entries[i];
    int w;
    switch (e->result) {
      case GTAV_BOOT_POOL_DATA_ENTRY_NOT_RUN:
      case GTAV_BOOT_POOL_DATA_ENTRY_SKIPPED:
        continue;
      case GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN:
        w = snprintf(out + n, cap - n, " %s=%d>%d", o->name, (int)o->stock, (int)o->new_value);
        break;
      case GTAV_BOOT_POOL_DATA_ENTRY_STOCK:
        w = e->depth && report->mode == GTAV_BOOT_POOL_DATA_MODE_OBSERVE
                ? snprintf(out + n, cap - n, " %s=%d@%u", o->name, (int)e->observed,
                           (unsigned)e->depth)
                : snprintf(out + n, cap - n, " %s=%d", o->name, (int)e->observed);
        break;
      case GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED:
      case GTAV_BOOT_POOL_DATA_ENTRY_CHANGED:
        w = snprintf(out + n, cap - n, " %s=%d!%s", o->name, (int)e->observed,
                     gtav_boot_pool_data_entry_name(e->result));
        break;
      default:
        w = snprintf(out + n, cap - n, " %s!%s", o->name,
                     gtav_boot_pool_data_entry_name(e->result));
        break;
    }
    append(cap, &n, w);
  }
  return n;
}

size_t gtav_boot_pool_data_format_verify(const GtavBootPoolDataProfile* profile,
                                         const GtavBootPoolDataVerify* verify, char* out,
                                         size_t cap) {
  size_t n = 0;
  uint32_t i;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!profile || !verify) return 0;
  append(cap, &n,
         snprintf(out, cap, "boot-pool verify: %s",
                  verify->what == GTAV_BOOT_POOL_DATA_VERIFY_STORES ? "stores" : "factories"));
  for (i = 0; i < profile->override_count && i < GTAV_BOOT_POOL_ENTRIES_MAX && n + 1 < cap; ++i) {
    const char* name = profile->overrides[i].name;
    int w;
    if (verify->state[i] == GTAV_BOOT_POOL_DATA_VERIFY_NOT_CHECKED) continue;
    if (verify->state[i] == GTAV_BOOT_POOL_DATA_VERIFY_UNREADABLE) {
      w = snprintf(out + n, cap - n, " %s=?", name);
    } else if (verify->state[i] == GTAV_BOOT_POOL_DATA_VERIFY_PENDING) {
      w = snprintf(out + n, cap - n, " %s=pending", name);
    } else if (verify->what == GTAV_BOOT_POOL_DATA_VERIFY_FACTORIES) {
      w = snprintf(out + n, cap - n, " %s=%u/%u", name, (unsigned)verify->used[i],
                   (unsigned)verify->value[i]);
    } else {
      w = snprintf(out + n, cap - n, " %s=%u", name, (unsigned)verify->value[i]);
    }
    append(cap, &n, w);
  }
  if (n + 1 < cap) {
    append(cap, &n,
           snprintf(out + n, cap - n, " grown=%u/%u", (unsigned)verify->grown,
                    (unsigned)verify->checked));
  }
  return n;
}
