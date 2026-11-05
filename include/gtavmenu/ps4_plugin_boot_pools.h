#pragma once

/* In-PRX boot-time pool growth (PS4 Legacy): the data-method core (gtavmenu/boot_pool_data.h) on
 * the PS4 plugin's supervisor thread, before Story Mode, through the runtime's ops. Plain data
 * reads and aligned 4-byte exchanges only; no GTA native and no code patch.
 *
 * S1 (gtav_ps4_boot_pools_run): the identity gate (mappings, version bytes, hook bytes, native
 * fingerprints; retried every 10 ms for up to 2 s while unreadable, a mismatch refuses), then a
 * sample every 1 ms (10 ms once the stores are sizing) until the mode decides, an unload asks to
 * leave (ABORTED), Story Mode is ready (MISSED, reason story-mode: far past every window), the
 * samples stay unreadable for 3 s after a classified one (TIMEOUT, reason unreadable) or the
 * deadline passes (TIMEOUT at 120 s of process time; OBSERVE samples until a factory exists + 1 s,
 * or 180 s). Reads outside the eboot image are made only after the protection query proves the
 * whole range readable; anything else is an "unreadable" sample. One result line, one timing line
 * and, when a read or an exchange was refused, one diagnostic line go to the log; a refused,
 * timed-out or raced result is also notified.
 *
 * S2 (gtav_ps4_boot_pools_poll, once per supervisor poll): logs the store sizes once the first
 * store is sized + 500 ms and every store's Init has run (PtFx is sized later, in InitStoresB; a
 * store still pending after 60 s is logged as "pending"), and the model-factory capacities once
 * they exist, each once. A written node whose store took its stock size anyway is put back to stock
 * (reconciled) and notified. A line longer than the log ring keeps continues on "boot-pool +"
 * lines. */

#include <stdint.h>

#include "gtavmenu/boot_pool_data.h"
#include "gtavmenu/ps4_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Present = boot pools OFF for every launch, whatever the build's mode. */
#define GTAV_PS4_BOOT_POOLS_KILL_SWITCH "/data/GTAVMenu/boot-pools.off"
#define GTAV_PS4_BOOT_POOLS_SAMPLE_US 1000u
#define GTAV_PS4_BOOT_POOLS_IDENTITY_RETRY_US 10000u
#define GTAV_PS4_BOOT_POOLS_IDENTITY_BUDGET_US 2000000u
#define GTAV_PS4_BOOT_POOLS_DEADLINE_US UINT64_C(120000000)
#define GTAV_PS4_BOOT_POOLS_OBSERVE_DEADLINE_US UINT64_C(180000000)
#define GTAV_PS4_BOOT_POOLS_OBSERVE_SETTLE_US 1000000u
#define GTAV_PS4_BOOT_POOLS_STORE_VERIFY_DELAY_US 500000u
#define GTAV_PS4_BOOT_POOLS_STORE_VERIFY_DEADLINE_US UINT64_C(60000000)
#define GTAV_PS4_BOOT_POOLS_LATE_SAMPLE_US 10000u
#define GTAV_PS4_BOOT_POOLS_UNREADABLE_US 3000000u
#define GTAV_PS4_BOOT_POOLS_READY_CHECK_US 100000u

typedef int (*GtavPs4BootPoolsStop)(void* context);
typedef void (*GtavPs4BootPoolsNotify)(const char* message);

typedef struct GtavPs4BootPoolsConfig {
  int mode;                // GTAV_BOOT_POOL_DATA_MODE_*, already lowered by the kill switch
  const char* off_reason;  // why the mode is OFF ("kill-switch", "build"), or NULL
  uint64_t load_us;        // plugin_load's process time
  GtavPs4BootPoolsStop stop;
  void* stop_context;
  /* 1 once Story Mode is ready (every boot window long closed); NULL = never asked. */
  GtavPs4BootPoolsStop story_mode;
  /* A system notification for a result the player should know about; NULL = none. */
  GtavPs4BootPoolsNotify notify;
} GtavPs4BootPoolsConfig;

typedef struct GtavPs4BootPools {
  GtavBootPoolDataReport report;
  GtavPs4PluginRuntime* runtime;
  int mode;
  int identity;  // the gate's last result: 0 passed, 1 unreadable, negative mismatch
  int readable;  // the gate passed: boot words may be read (S1 and S2)
  int sample1_window;
  uint64_t load_us;
  uint64_t identity_us;
  uint64_t sample1_us;
  uint64_t parse_us;
  uint64_t store_init_us;
  uint64_t sized_us;
  uint64_t factory_us;
  uint64_t decided_us;
  uint64_t last_sample_us;
  uint64_t max_gap_us;
  uint32_t samples;
  uint32_t seen;    // GTAV_PS4_BOOT_POOLS_BOUND_*: the marker was seen
  uint32_t bounds;  // GTAV_PS4_BOOT_POOLS_BOUND_*: already set at the first sample
  /* The first read outside the image the query did not prove readable. */
  uint32_t refused_reads;
  uint64_t refused_address;
  uint32_t refused_size;
  int refused_query;
  GtavPs4PluginMapInfo refused_map;
  /* The first exchange that did not swap. */
  uint32_t failed_exchanges;
  uint64_t exchange_address;
  int exchange_result;
  /* One proven-readable mapping, valid for one core call. */
  uint64_t cache_start;
  uint64_t cache_end;
  int stores_logged;
  int factories_logged;
  uint32_t reconciled;  // node rows put back to stock after their store took stock
  GtavPs4BootPoolsNotify notify;
} GtavPs4BootPools;

enum {
  GTAV_PS4_BOOT_POOLS_BOUND_PARSE = 1,
  GTAV_PS4_BOOT_POOLS_BOUND_STORE_INIT = 2,
  GTAV_PS4_BOOT_POOLS_BOUND_SIZED = 4,
  GTAV_PS4_BOOT_POOLS_BOUND_FACTORY = 8,
};

/* S1. Returns pools->report.status (GTAV_BOOT_POOL_DATA_*). */
int gtav_ps4_boot_pools_run(GtavPs4BootPools* pools, GtavPs4PluginRuntime* runtime,
                            const GtavPs4BootPoolsConfig* config);
/* S2. 1 once both checks were logged (or nothing may be read), 0 while one is still due. */
int gtav_ps4_boot_pools_poll(GtavPs4BootPools* pools);

#ifdef __cplusplus
}
#endif
