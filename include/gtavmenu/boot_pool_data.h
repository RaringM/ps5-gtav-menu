#pragma once

/* Data-method boot-pool growth, for a target whose boot leaves a window a plain data write can
 * reach (PS4 Legacy 01.57): rewrite each PoolSizes node of the parsed gameconfig after the parse
 * and before the first store is sized, and the parsed model caps before the model-info factories
 * exist. No code is patched and nothing calls the game.
 *
 * Pure logic over two ops, a checked read and an aligned 4-byte compare-and-exchange, so the same
 * code runs on the PRX supervisor thread and in the host tests over a fake memory image. The
 * layout comes from the generated gtavmenu/boot_pool_data_<stem>_generated.h through
 * GTAV_BOOT_POOL_DATA_PROFILE_INIT; the development tree's host watcher reads
 * the same pins.
 *
 * Rules: growth only; every entry is checked against its pinned stock value; a commit is all or
 * nothing (a word that changed rolls back the words already written); a read the ops cannot prove
 * safe is a "not yet" or "unreadable" result, never a dereference. */

#include <stddef.h>
#include <stdint.h>

#include "gtavmenu/boot_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_BOOT_POOL_DATA_FACTORIES 3u
/* A factory block map with more buckets than this is not the pinned layout (1.57's have 107). */
#define GTAV_BOOT_POOL_DATA_FACTORY_SLOTS_MAX 4096u
#define GTAV_BOOT_POOL_DATA_LINE_BYTES 240u

typedef struct GtavBootPoolDataProfile {
  const char* target_id;
  int enabled;  // manifest build.features.bootPools
  uint32_t chain_max;
  uint64_t fsm_state;       // u16 boot state: 0 parse, 1 streaming init, 2 running
  uint64_t config_manager;  // u64 gameconfig manager pointer
  uint64_t config_mgr_buckets;
  uint64_t config_mgr_slots;
  uint64_t config_mgr_cfg;
  uint64_t config_node_value;
  uint64_t config_node_next;
  uint64_t store_init_object;  // u64, set immediately before the stores are sized
  uint64_t first_store_sized;  // u8, set once the first store is sized
  uint64_t store_size_offset;
  uint64_t store_initialized_offset;  // u8 a store's Init sets once it has sized the pool
  uint64_t factories[GTAV_BOOT_POOL_DATA_FACTORIES];  // ped, vehicle, weapon
  uint64_t model_factory_buckets;
  uint64_t model_factory_slots;
  uint64_t model_factory_block_key;
  uint64_t model_factory_block_value;
  uint64_t model_factory_node_next;
  uint64_t model_block_count;
  uint64_t model_block_capacity;
  const GtavBootPoolOverride* overrides;
  const uint64_t* verify;  // per override: the store object or model factory showing the effect
  uint32_t override_count;
} GtavBootPoolDataProfile;

/* Expands where the generated header is included. */
#define GTAV_BOOT_POOL_DATA_PROFILE_INIT                                              \
  {GTAV_BOOT_POOL_DATA_TARGET_ID,                                                     \
   GTAV_BOOT_POOL_DATA_ENABLED,                                                       \
   GTAV_BOOT_POOL_DATA_CHAIN_MAX,                                                     \
   GTAV_BOOT_POOL_DATA_FSM_STATE,                                                     \
   GTAV_BOOT_POOL_DATA_CONFIG_MANAGER,                                                \
   GTAV_BOOT_POOL_DATA_CONFIG_MGR_BUCKETS,                                            \
   GTAV_BOOT_POOL_DATA_CONFIG_MGR_SLOTS,                                              \
   GTAV_BOOT_POOL_DATA_CONFIG_MGR_CFG,                                                \
   GTAV_BOOT_POOL_DATA_CONFIG_NODE_VALUE,                                             \
   GTAV_BOOT_POOL_DATA_CONFIG_NODE_NEXT,                                              \
   GTAV_BOOT_POOL_DATA_STORE_INIT_OBJECT,                                             \
   GTAV_BOOT_POOL_DATA_FIRST_STORE_SIZED,                                             \
   GTAV_BOOT_POOL_DATA_STORE_SIZE_OFFSET,                                             \
   GTAV_BOOT_POOL_DATA_STORE_INITIALIZED_OFFSET,                                      \
   {GTAV_BOOT_POOL_DATA_PED_MODEL_FACTORY, GTAV_BOOT_POOL_DATA_VEHICLE_MODEL_FACTORY, \
    GTAV_BOOT_POOL_DATA_WEAPON_MODEL_FACTORY},                                        \
   GTAV_BOOT_POOL_DATA_MODEL_FACTORY_BUCKETS,                                         \
   GTAV_BOOT_POOL_DATA_MODEL_FACTORY_SLOTS,                                           \
   GTAV_BOOT_POOL_DATA_MODEL_FACTORY_BLOCK_KEY,                                       \
   GTAV_BOOT_POOL_DATA_MODEL_FACTORY_BLOCK_VALUE,                                     \
   GTAV_BOOT_POOL_DATA_MODEL_FACTORY_NODE_NEXT,                                       \
   GTAV_BOOT_POOL_DATA_MODEL_BLOCK_COUNT,                                             \
   GTAV_BOOT_POOL_DATA_MODEL_BLOCK_CAPACITY,                                          \
   gtav_boot_pool_data_overrides,                                                     \
   gtav_boot_pool_data_verify,                                                        \
   (uint32_t)(sizeof(gtav_boot_pool_data_overrides) / sizeof(gtav_boot_pool_data_overrides[0]))}

/* Same values as GTAV_PS4_PLUGIN_CAS_* (static_assert in src/module/ps4/plugin_boot_pools.cpp). */
enum {
  GTAV_BOOT_POOL_DATA_CAS_SWAPPED = 0,
  GTAV_BOOT_POOL_DATA_CAS_DIFFERS = 1,  // the word held *observed; nothing was written
  GTAV_BOOT_POOL_DATA_CAS_REFUSED = -1  // misaligned, unmapped or not writable; nothing written
};

typedef struct GtavBootPoolDataOps {
  void* context;
  /* 0 = `size` bytes copied from `address`; -1 = the range is not proven readable (nothing read).
   */
  int (*read)(void* context, uint64_t address, void* out, uint32_t size);
  /* One aligned 4-byte compare-and-exchange: GTAV_BOOT_POOL_DATA_CAS_*. */
  int (*cas32)(void* context, uint64_t address, uint32_t expected, uint32_t desired,
               uint32_t* observed);
} GtavBootPoolDataOps;

enum {
  GTAV_BOOT_POOL_DATA_MODE_OFF = 0,
  GTAV_BOOT_POOL_DATA_MODE_OBSERVE = 1,  // resolve and report, never write
  GTAV_BOOT_POOL_DATA_MODE_CAPS = 2,     // the cfg rows (model caps) only
  GTAV_BOOT_POOL_DATA_MODE_FULL = 3,     // every row inside the store window; past it, the cfg rows
};

enum {
  GTAV_BOOT_POOL_DATA_WINDOW_NONE = -1,
  GTAV_BOOT_POOL_DATA_WINDOW_WAIT = 0,        // the gameconfig parse is not done
  GTAV_BOOT_POOL_DATA_WINDOW_OPEN = 1,        // parsed; no store sized; no factory
  GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY = 2,   // stores sizing or sized; no factory yet
  GTAV_BOOT_POOL_DATA_WINDOW_CLOSED = 3,      // a model-info factory holds its storage block
  GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE = 4,  // a needed word could not be read: sample again
};

enum {
  GTAV_BOOT_POOL_DATA_PENDING = 0,
  GTAV_BOOT_POOL_DATA_OFF,
  GTAV_BOOT_POOL_DATA_OBSERVED,
  GTAV_BOOT_POOL_DATA_APPLIED,
  GTAV_BOOT_POOL_DATA_RACED,  // written, but a sentinel flipped before the post-commit sample
  GTAV_BOOT_POOL_DATA_MISSED,
  GTAV_BOOT_POOL_DATA_REFUSED,
  GTAV_BOOT_POOL_DATA_TIMEOUT,
  GTAV_BOOT_POOL_DATA_ABORTED,
};

/* Per-entry results. NO_MANAGER..UNREADABLE are "not yet" (sample again); MISALIGNED..UNEXPECTED
   refuse the whole set. */
enum {
  GTAV_BOOT_POOL_DATA_ENTRY_NOT_RUN = 0,
  GTAV_BOOT_POOL_DATA_ENTRY_SKIPPED,  // not in this mode's plan
  GTAV_BOOT_POOL_DATA_ENTRY_STOCK,    // resolved; holds its pinned stock value
  GTAV_BOOT_POOL_DATA_ENTRY_WRITTEN,
  GTAV_BOOT_POOL_DATA_ENTRY_NO_MANAGER,
  GTAV_BOOT_POOL_DATA_ENTRY_NO_BUCKETS,
  GTAV_BOOT_POOL_DATA_ENTRY_NO_CFG,
  GTAV_BOOT_POOL_DATA_ENTRY_UNREADABLE,
  GTAV_BOOT_POOL_DATA_ENTRY_MISALIGNED,
  GTAV_BOOT_POOL_DATA_ENTRY_NOT_FOUND,
  GTAV_BOOT_POOL_DATA_ENTRY_CHAIN_LIMIT,
  GTAV_BOOT_POOL_DATA_ENTRY_UNEXPECTED,
  GTAV_BOOT_POOL_DATA_ENTRY_CHANGED,          // moved off stock between resolve and commit
  GTAV_BOOT_POOL_DATA_ENTRY_CAS_REFUSED,      // the exchange was refused (not writable)
  GTAV_BOOT_POOL_DATA_ENTRY_ROLLED_BACK,      // written, then put back to stock
  GTAV_BOOT_POOL_DATA_ENTRY_ROLLBACK_FAILED,  // written; putting it back failed
  GTAV_BOOT_POOL_DATA_ENTRY_RECONCILED,       // written, its store took stock: put back to stock
};

typedef struct GtavBootPoolDataSample {
  int window;
  uint32_t fsm_state;
  uint32_t first_store_sized;
  uint64_t manager;
  uint64_t store_init_object;
  uint32_t factories;  // bit i: factories[i] holds its storage block
  /* bit i: factories[i]'s block map is not laid out as pinned (a misaligned bucket array or more
     buckets than any 1.57 factory has): the pins do not describe this game, so refuse. */
  uint32_t anomalies;
} GtavBootPoolDataSample;

typedef struct GtavBootPoolDataEntry {
  uint64_t address;  // the value word, once resolved
  int32_t observed;  // the value read at resolve (or seen by a failed exchange)
  uint8_t planned;
  uint8_t result;  // GTAV_BOOT_POOL_DATA_ENTRY_*
  uint8_t depth;   // node: 1-based position in its bucket chain; cfg: 0
} GtavBootPoolDataEntry;

typedef struct GtavBootPoolDataReport {
  int mode;
  int status;        // GTAV_BOOT_POOL_DATA_* status
  int first_window;  // the first sample past WAIT/UNREADABLE
  int window;        // the sample the decision rests on
  /* The furthest boot state any classified sample showed (OPEN < CAPS_ONLY < CLOSED). The boot
     only moves forward, so a later sample that reads an earlier state is taken at this one. */
  int high_window;
  const char* reason;
  uint32_t count;
  uint32_t planned;
  uint32_t written;  // entries holding their new value now
  uint32_t resolves;
  int observed;  // OBSERVE: the values were recorded
  int fallback;  // FULL: past the store window, so the plan kept only the cfg rows
  GtavBootPoolDataEntry entries[GTAV_BOOT_POOL_ENTRIES_MAX];
} GtavBootPoolDataReport;

enum {
  GTAV_BOOT_POOL_DATA_VERIFY_STORES = 1,
  GTAV_BOOT_POOL_DATA_VERIFY_FACTORIES = 2,
};

enum {
  GTAV_BOOT_POOL_DATA_VERIFY_NOT_CHECKED = 0,
  GTAV_BOOT_POOL_DATA_VERIFY_READ = 1,
  GTAV_BOOT_POOL_DATA_VERIFY_UNREADABLE = 2,
  GTAV_BOOT_POOL_DATA_VERIFY_PENDING = 3,  // a store whose Init has not sized it yet
};

typedef struct GtavBootPoolDataVerify {
  int what;
  uint32_t checked;
  uint32_t read;
  uint32_t grown;
  uint32_t pending;                            // stores not initialized yet
  uint8_t state[GTAV_BOOT_POOL_ENTRIES_MAX];   // GTAV_BOOT_POOL_DATA_VERIFY_*
  uint32_t value[GTAV_BOOT_POOL_ENTRIES_MAX];  // store pool size or factory capacity
  uint32_t used[GTAV_BOOT_POOL_ENTRIES_MAX];   // factory: used count
} GtavBootPoolDataVerify;

const char* gtav_boot_pool_data_mode_name(int mode);
const char* gtav_boot_pool_data_window_name(int window);
const char* gtav_boot_pool_data_status_name(int status);
const char* gtav_boot_pool_data_entry_name(int result);

/* Classify the boot window. Reads the image words and walks the factory block maps; returns
   out->window. OPEN needs the parse done in boot state 1 (streaming init) with no store sentinel
   set; a later boot state with no sentinel reads as CAPS_ONLY. A factory map whose walk cannot be
   read, or ends at the chain limit, makes the sample UNREADABLE. */
int gtav_boot_pool_data_sample(const GtavBootPoolDataProfile* profile,
                               const GtavBootPoolDataOps* ops, GtavBootPoolDataSample* out);

/* Start a report for `mode`: OFF plans nothing, OBSERVE and FULL every row, CAPS the cfg rows.
   0, or -1 when the profile is not growth-only (stock > 0, new > stock, aligned cfg offsets). */
int gtav_boot_pool_data_plan(const GtavBootPoolDataProfile* profile, int mode,
                             GtavBootPoolDataReport* report);

/* Find every planned entry and read its value; no writes. 0 = all found at their stock values,
   1 = not yet (a pointer on the way is null or unreadable), -1 = refused (misaligned pointer,
   missing node, chain limit or a value that is not stock). */
int gtav_boot_pool_data_resolve(const GtavBootPoolDataProfile* profile,
                                const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report);

/* After a 0 from resolve, in CAPS or FULL mode: exchange stock -> new for each planned entry in
   order. A word that is no longer stock, or a refused exchange, rolls the written ones back
   (new -> stock) and fails. 0 = all written, -1 = refused. */
int gtav_boot_pool_data_commit(const GtavBootPoolDataProfile* profile,
                               const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report);

/* One decision over a fresh sample. FULL resolves and commits every row while OPEN; once a sample
   shows CAPS_ONLY before any write, it drops the node rows (the stores may be reading them) and
   goes on like CAPS (report->fallback). CAPS commits the cfg rows while OPEN or CAPS_ONLY. Both
   sample again right before and after the commit (APPLIED, or RACED when a sentinel flipped). A
   first classified sample past the mode's window is MISSED with no write. OBSERVE records the
   values once and decides at CLOSED. 1 once report->status is final, 0 to sample again. */
int gtav_boot_pool_data_step(const GtavBootPoolDataProfile* profile, const GtavBootPoolDataOps* ops,
                             GtavBootPoolDataReport* report, const GtavBootPoolDataSample* sample);

/* Read what the game took: store pool sizes (VERIFY_STORES) or model-factory capacities
   (VERIFY_FACTORIES) for the profile's rows. A store whose initialized byte is still 0 has not
   been sized (PtFx is sized later than the others) and is PENDING, not read. 0 when every checked
   row was read. */
int gtav_boot_pool_data_verify_growth(const GtavBootPoolDataProfile* profile,
                                      const GtavBootPoolDataOps* ops, int what,
                                      GtavBootPoolDataVerify* out);

/* After the store verify: every written node row whose store took its stock size anyway (a race
   the post-commit sample did not see) goes back new -> stock, so nothing later reads a node the
   store did not use. Rows verified grown, or not read, are left as they are. Returns how many rows
   were put back (entries[i].result = RECONCILED); -1 when an exchange was refused or changed. */
int gtav_boot_pool_data_reconcile(const GtavBootPoolDataProfile* profile,
                                  const GtavBootPoolDataOps* ops, GtavBootPoolDataReport* report,
                                  const GtavBootPoolDataVerify* verify);

/* "boot-pool: mode=full status=applied written=8/8 first=open metadata=3200>4200 ..." (a fallback
   adds " fallback=caps" after the window); returns the length (truncated to fit `cap`). */
size_t gtav_boot_pool_data_format(const GtavBootPoolDataProfile* profile,
                                  const GtavBootPoolDataReport* report, char* out, size_t cap);

/* "boot-pool verify: stores metadata=4200 ... grown=6/6" or "... factories ped=435/540 ...";
   an unreadable row prints "=?" and a pending store "=pending". */
size_t gtav_boot_pool_data_format_verify(const GtavBootPoolDataProfile* profile,
                                         const GtavBootPoolDataVerify* verify, char* out,
                                         size_t cap);

#ifdef __cplusplus
}
#endif
