// In-PRX boot-time pool growth (see gtavmenu/ps4_plugin_boot_pools.h). Runs only on the supervisor
// thread; every game access goes through the runtime's ops.

#include "gtavmenu/ps4_plugin_boot_pools.h"

#include "gtavmenu/ps4_plugin_log.h"

#ifndef GTAV_PS4_PLUGIN_HEADER
#error \
    "GTAV_PS4_PLUGIN_HEADER must name the target's generated PS4 plugin header (tools/ps4_build.py)"
#endif
#include GTAV_PS4_PLUGIN_HEADER
#ifdef GTAV_PS4_BOOT_POOL_DATA_HEADER
#include GTAV_PS4_BOOT_POOL_DATA_HEADER
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// exchange() hands the runtime's cas32 result to the core unchanged. (Compared as int: the two
// codes are separate enums, which gcc's -Wenum-compare refuses to compare directly.)
static_assert((int)GTAV_BOOT_POOL_DATA_CAS_SWAPPED == (int)GTAV_PS4_PLUGIN_CAS_SWAPPED &&
                  (int)GTAV_BOOT_POOL_DATA_CAS_DIFFERS == (int)GTAV_PS4_PLUGIN_CAS_DIFFERS &&
                  (int)GTAV_BOOT_POOL_DATA_CAS_REFUSED == (int)GTAV_PS4_PLUGIN_CAS_REFUSED,
              "the core's exchange results are the runtime's");

namespace {

#ifdef GTAV_PS4_BOOT_POOL_DATA_HEADER
const GtavBootPoolDataProfile kProfile = GTAV_BOOT_POOL_DATA_PROFILE_INIT;
constexpr bool kHavePins = true;
constexpr bool kPinsEnabled = GTAV_BOOT_POOL_DATA_ENABLED != 0;
#else
const GtavBootPoolDataProfile kProfile = {};
constexpr bool kHavePins = false;
constexpr bool kPinsEnabled = false;
#endif

// One log ring slot. A result or verify line can be longer (a refusal lists every row with its
// reason): it is formatted whole into kResultBytes and logged in pieces (log_wrapped).
constexpr uint32_t kLineBytes = GTAV_PS4_PLUGIN_LOG_TEXT_BYTES;
constexpr uint32_t kResultBytes = 640;
// A boot word is at most 8 bytes; a longer read is not one the core makes.
constexpr uint32_t kMaxRead = 64;
constexpr uint32_t kMaxQueries = 4;

const GtavPs4PluginOps& ops(const GtavPs4BootPools* pools) {
  return pools->runtime->ops;
}

uint64_t now(const GtavPs4BootPools* pools) {
  return ops(pools).now_us(ops(pools).context);
}

void pause(const GtavPs4BootPools* pools, uint32_t usec) {
  ops(pools).sleep_us(ops(pools).context, usec);
}

void log_text(const GtavPs4BootPools* pools, const char* text) {
  if (ops(pools).log) ops(pools).log(ops(pools).context, text);
}

// `text` in lines the log ring keeps whole, split before a space; each continuation starts with
// "boot-pool +" so a reader can join them back.
void log_wrapped(const GtavPs4BootPools* pools, const char* text) {
  static constexpr char kContinued[] = "boot-pool +";
  size_t length = strlen(text);
  bool first = true;
  while (length) {
    const size_t room = kLineBytes - 1 - (first ? 0 : sizeof(kContinued) - 1);
    size_t take = length;
    if (take > room) {
      take = room;
      while (take > 0 && text[take] != ' ') --take;
      if (take == 0) take = room;
    }
    char line[kLineBytes];
    snprintf(line, sizeof(line), "%s%.*s", first ? "" : kContinued, (int)take, text);
    log_text(pools, line);
    text += take;
    length -= take;
    first = false;
  }
}

void logf(const GtavPs4BootPools* pools, const char* format, ...) {
  char line[kLineBytes];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  log_text(pools, line);
}

bool covers(const GtavPs4PluginMapping& mapping, uint64_t address, uint64_t end) {
  return mapping.start <= address && end <= mapping.end;
}

// The eboot data segments: the identity gate proved them mapped readable, and the main image is
// never unmapped, so the boot words there are read without a query.
bool in_image(uint64_t address, uint64_t end) {
  for (size_t i = 0; i < GTAV_PS4_PLUGIN_DATA_SEGMENT_COUNT; ++i) {
    if (covers(kGtavPs4PluginDataSegments[i], address, end)) return true;
  }
  return false;
}

void note_refused_read(GtavPs4BootPools* pools, uint64_t address, uint32_t size, int queried,
                       const GtavPs4PluginMapInfo& info) {
  if (pools->refused_reads++ != 0) return;
  pools->refused_address = address;
  pools->refused_size = size;
  pools->refused_query = queried;
  pools->refused_map = info;
}

// Every byte of [address, address + size) lies in a mapping the protection query reports
// readable. One proven mapping is remembered for the rest of the current core call.
bool proven_readable(GtavPs4BootPools* pools, uint64_t address, uint64_t end, uint32_t size) {
  if (pools->cache_end && pools->cache_start <= address && end <= pools->cache_end) return true;
  uint64_t cursor = address;
  for (uint32_t i = 0; i < kMaxQueries && cursor < end; ++i) {
    GtavPs4PluginMapInfo info;
    memset(&info, 0, sizeof(info));
    const int queried = ops(pools).query(ops(pools).context, cursor, &info);
    if (queried != 0 || info.start > cursor || info.end <= cursor ||
        (info.prot & GTAV_PS4_PLUGIN_PROT_READ) == 0) {
      note_refused_read(pools, address, size, queried, info);
      return false;
    }
    if (cursor == address && end <= info.end) {
      pools->cache_start = info.start;
      pools->cache_end = info.end;
    }
    cursor = info.end;
  }
  if (cursor < end) {
    GtavPs4PluginMapInfo none;
    memset(&none, 0, sizeof(none));
    note_refused_read(pools, address, size, 1, none);
    return false;
  }
  return true;
}

int checked_read(void* context, uint64_t address, void* out, uint32_t size) {
  GtavPs4BootPools* pools = static_cast<GtavPs4BootPools*>(context);
  const uint64_t end = address + size;
  if (!pools->readable || size == 0 || size > kMaxRead || end < address) return -1;
  if (!in_image(address, end) && !proven_readable(pools, address, end, size)) return -1;
  return ops(pools).read(ops(pools).context, address, out, size) == 0 ? 0 : -1;
}

int exchange(void* context, uint64_t address, uint32_t expected, uint32_t desired,
             uint32_t* observed) {
  GtavPs4BootPools* pools = static_cast<GtavPs4BootPools*>(context);
  if (!pools->readable) return GTAV_BOOT_POOL_DATA_CAS_REFUSED;
  const int result = ops(pools).cas32(ops(pools).context, address, expected, desired, observed);
  if (result != GTAV_BOOT_POOL_DATA_CAS_SWAPPED && pools->failed_exchanges++ == 0) {
    pools->exchange_address = address;
    pools->exchange_result = result;
  }
  return result;
}

// The core's ops for one call: the readable-mapping memory starts empty each time.
GtavBootPoolDataOps data_ops(GtavPs4BootPools* pools) {
  pools->cache_start = 0;
  pools->cache_end = 0;
  return GtavBootPoolDataOps{pools, checked_read, exchange};
}

void mark(GtavPs4BootPools* pools, uint64_t* at, uint32_t flag, bool seen, bool first, uint64_t t) {
  if (!seen || (pools->seen & flag) != 0) return;
  pools->seen |= flag;
  *at = t;
  if (first) pools->bounds |= flag;
}

void track(GtavPs4BootPools* pools, const GtavBootPoolDataSample& sample, uint64_t t) {
  const bool first = pools->samples == 0;
  ++pools->samples;
  if (first) {
    pools->sample1_us = t;
    pools->sample1_window = sample.window;
  } else if (t - pools->last_sample_us > pools->max_gap_us) {
    pools->max_gap_us = t - pools->last_sample_us;
  }
  pools->last_sample_us = t;
  mark(pools, &pools->parse_us, GTAV_PS4_BOOT_POOLS_BOUND_PARSE,
       sample.fsm_state >= 1u && sample.manager != 0, first, t);
  mark(pools, &pools->store_init_us, GTAV_PS4_BOOT_POOLS_BOUND_STORE_INIT,
       sample.store_init_object != 0, first, t);
  mark(pools, &pools->sized_us, GTAV_PS4_BOOT_POOLS_BOUND_SIZED, sample.first_store_sized != 0,
       first, t);
  mark(pools, &pools->factory_us, GTAV_PS4_BOOT_POOLS_BOUND_FACTORY, sample.factories != 0, first,
       t);
}

// " name=+S.mmms", " name<=+S.mmms" (already set at the first sample) or " name=-".
int append_time(char* out, size_t cap, const char* name, const GtavPs4BootPools* pools,
                uint32_t flag, uint64_t t) {
  if (flag && (pools->seen & flag) == 0) return snprintf(out, cap, " %s=-", name);
  return snprintf(out, cap, " %s%s+%llu.%03llus", name, flag && (pools->bounds & flag) ? "<=" : "=",
                  (unsigned long long)(t / 1000000u), (unsigned long long)(t % 1000000u / 1000u));
}

void advance(size_t cap, size_t* n, int written) {
  if (written < 0) return;
  *n += (size_t)written < cap - *n ? (size_t)written : cap - *n - 1;
}

void log_timing(const GtavPs4BootPools* pools) {
  char line[kLineBytes];
  size_t n = 0;
  advance(sizeof(line), &n, snprintf(line, sizeof(line), "boot-pool timing:"));
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "load", pools, 0, pools->load_us));
  if (pools->readable) {
    advance(sizeof(line), &n,
            append_time(line + n, sizeof(line) - n, "identity", pools, 0, pools->identity_us));
  } else {
    advance(sizeof(line), &n, snprintf(line + n, sizeof(line) - n, " identity=-"));
  }
  if (pools->samples) {
    advance(sizeof(line), &n,
            snprintf(line + n, sizeof(line) - n, " sample1=%s@+%llu.%03llus",
                     gtav_boot_pool_data_window_name(pools->sample1_window),
                     (unsigned long long)(pools->sample1_us / 1000000u),
                     (unsigned long long)(pools->sample1_us % 1000000u / 1000u)));
  }
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "parse", pools, GTAV_PS4_BOOT_POOLS_BOUND_PARSE,
                      pools->parse_us));
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "init", pools,
                      GTAV_PS4_BOOT_POOLS_BOUND_STORE_INIT, pools->store_init_us));
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "sized", pools, GTAV_PS4_BOOT_POOLS_BOUND_SIZED,
                      pools->sized_us));
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "factories", pools,
                      GTAV_PS4_BOOT_POOLS_BOUND_FACTORY, pools->factory_us));
  advance(sizeof(line), &n,
          append_time(line + n, sizeof(line) - n, "decided", pools, 0, pools->decided_us));
  advance(sizeof(line), &n,
          snprintf(line + n, sizeof(line) - n, " samples=%u gap=%llu.%llums resolves=%u",
                   (unsigned)pools->samples, (unsigned long long)(pools->max_gap_us / 1000u),
                   (unsigned long long)(pools->max_gap_us % 1000u / 100u),
                   (unsigned)pools->report.resolves));
  log_text(pools, line);
}

void log_diagnostics(GtavPs4BootPools* pools) {
  if (pools->identity < 0) {
    logf(pools, "boot-pool identity: %s (%d)", gtav_ps4_plugin_error_string(pools->identity),
         pools->identity);
  }
  if (pools->refused_reads) {
    const GtavPs4PluginMapInfo& map = pools->refused_map;
    logf(pools, "boot-pool reads: refused=%u first=0x%llx+%u query=%d map=0x%llx-0x%llx prot=%u",
         (unsigned)pools->refused_reads, (unsigned long long)pools->refused_address,
         (unsigned)pools->refused_size, pools->refused_query, (unsigned long long)map.start,
         (unsigned long long)map.end, (unsigned)map.prot);
  }
  if (pools->failed_exchanges) {
    // What the protection query says about the first word that did not swap (seen on hardware: the
    // query can lag behind the real protection).
    GtavPs4PluginMapInfo map;
    memset(&map, 0, sizeof(map));
    const int queried = ops(pools).query(ops(pools).context, pools->exchange_address, &map);
    logf(pools,
         "boot-pool cas32: failed=%u first=0x%llx result=%d query=%d map=0x%llx-0x%llx prot=%u",
         (unsigned)pools->failed_exchanges, (unsigned long long)pools->exchange_address,
         pools->exchange_result, queried, (unsigned long long)map.start,
         (unsigned long long)map.end, (unsigned)map.prot);
  }
}

void notify(const GtavPs4BootPools* pools, const char* message) {
  if (pools->notify) pools->notify(message);
}

int finish(GtavPs4BootPools* pools, int status, const char* reason) {
  if (status != GTAV_BOOT_POOL_DATA_PENDING) {
    pools->report.status = status;
    pools->report.reason = reason;
  }
  pools->decided_us = now(pools);
  char line[kResultBytes];
  gtav_boot_pool_data_format(&kProfile, &pools->report, line, sizeof(line));
  log_wrapped(pools, line);
  log_timing(pools);
  log_diagnostics(pools);
  // A result that is not what the build meant to do, said once where the player sees it. An
  // identity refusal is not repeated: the start preflight refuses the same game and says so.
  const int final_status = pools->report.status;
  const char* why = pools->report.reason ? pools->report.reason : "";
  if ((final_status == GTAV_BOOT_POOL_DATA_REFUSED || final_status == GTAV_BOOT_POOL_DATA_TIMEOUT ||
       final_status == GTAV_BOOT_POOL_DATA_RACED) &&
      strncmp(why, "identity", 8) != 0) {
    char message[128];
    snprintf(message, sizeof(message), "GTAV Menu boot pools %s (%s); the menu still loads",
             gtav_boot_pool_data_status_name(final_status),
             pools->report.reason ? pools->report.reason : "see the log");
    notify(pools, message);
  }
  return final_status;
}

bool stopping(const GtavPs4BootPoolsConfig* config) {
  return config->stop && config->stop(config->stop_context) != 0;
}

void start_report(GtavPs4BootPools* pools, int mode) {
  memset(&pools->report, 0, sizeof(pools->report));
  pools->report.mode = mode;
  pools->report.first_window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
  pools->report.window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
}

// A store that took its stock size although its node was written (a race the post-commit sample
// did not see): put the node back, so nothing later reads a size the store does not have.
void reconcile(GtavPs4BootPools* pools, const GtavBootPoolDataVerify* verify) {
  if (pools->report.written == 0) return;
  GtavBootPoolDataOps data = data_ops(pools);
  const uint32_t before = pools->report.written;
  const int put_back = gtav_boot_pool_data_reconcile(&kProfile, &data, &pools->report, verify);
  if (put_back == 0) return;
  char line[kResultBytes];
  size_t n = (size_t)snprintf(line, sizeof(line), "boot-pool reconcile: %s",
                              put_back < 0 ? "refused" : "put back");
  for (uint32_t i = 0; i < pools->report.count && n + 1 < sizeof(line); ++i) {
    if (pools->report.entries[i].result != GTAV_BOOT_POOL_DATA_ENTRY_RECONCILED) continue;
    advance(sizeof(line), &n,
            snprintf(line + n, sizeof(line) - n, " %s=%d", kProfile.overrides[i].name,
                     (int)kProfile.overrides[i].stock));
  }
  advance(sizeof(line), &n,
          snprintf(line + n, sizeof(line) - n, " written=%u>%u", (unsigned)before,
                   (unsigned)pools->report.written));
  log_wrapped(pools, line);
  pools->reconciled += put_back > 0 ? (uint32_t)put_back : 0u;
  notify(pools, put_back < 0 ? "GTAV Menu boot pools: a store took stock and its node stays grown"
                             : "GTAV Menu boot pools: a store took its stock size (reconciled)");
}

}  // namespace

int gtav_ps4_boot_pools_run(GtavPs4BootPools* pools, GtavPs4PluginRuntime* runtime,
                            const GtavPs4BootPoolsConfig* config) {
  memset(pools, 0, sizeof(*pools));
  pools->runtime = runtime;
  pools->notify = config->notify;
  pools->load_us = config->load_us;
  pools->identity = 1;
  pools->sample1_window = GTAV_BOOT_POOL_DATA_WINDOW_NONE;
  int mode = config->mode;
  const char* off_reason = config->off_reason ? config->off_reason : "build";
  if constexpr (!kHavePins) {
    mode = GTAV_BOOT_POOL_DATA_MODE_OFF;
    off_reason = "no-pins";
  } else if constexpr (!kPinsEnabled) {
    mode = GTAV_BOOT_POOL_DATA_MODE_OFF;
    off_reason = "disabled";
  }
  pools->mode = mode;
  start_report(pools, mode);
  if (mode == GTAV_BOOT_POOL_DATA_MODE_OFF) {
    return finish(pools, GTAV_BOOT_POOL_DATA_OFF, off_reason);
  }
  if (gtav_boot_pool_data_plan(&kProfile, mode, &pools->report) != 0) {
    start_report(pools, mode);
    return finish(pools, GTAV_BOOT_POOL_DATA_REFUSED, "profile");
  }

  // S1a: this exact game, from the eboot image alone. Nothing past it is read before the gate.
  const uint64_t gate_start = now(pools);
  for (;;) {
    if (stopping(config)) return finish(pools, GTAV_BOOT_POOL_DATA_ABORTED, "unload");
    pools->identity = gtav_ps4_plugin_runtime_identity(runtime);
    if (pools->identity == 0) break;
    if (pools->identity < 0) return finish(pools, GTAV_BOOT_POOL_DATA_REFUSED, "identity");
    if (now(pools) - gate_start >= GTAV_PS4_BOOT_POOLS_IDENTITY_BUDGET_US) {
      return finish(pools, GTAV_BOOT_POOL_DATA_REFUSED, "identity-unreadable");
    }
    pause(pools, GTAV_PS4_BOOT_POOLS_IDENTITY_RETRY_US);
  }
  pools->readable = 1;
  pools->identity_us = now(pools);

  // S1b: one sample per millisecond until the mode decides (10 ms once the stores are sizing in
  // the deciding modes: only the factories are left to watch, and they come about 30 s later).
  const uint64_t deadline = mode == GTAV_BOOT_POOL_DATA_MODE_OBSERVE
                                ? GTAV_PS4_BOOT_POOLS_OBSERVE_DEADLINE_US
                                : GTAV_PS4_BOOT_POOLS_DEADLINE_US;
  uint64_t settle_until = 0;
  uint64_t unreadable_since = 0;
  uint64_t ready_checked = 0;
  for (;;) {
    if (stopping(config)) {
      if (settle_until) break;  // already OBSERVED
      return finish(pools, GTAV_BOOT_POOL_DATA_ABORTED, "unload");
    }
    const uint64_t t = now(pools);
    GtavBootPoolDataSample sample;
    GtavBootPoolDataOps data = data_ops(pools);
    gtav_boot_pool_data_sample(&kProfile, &data, &sample);
    track(pools, sample, t);
    if (settle_until) {
      if (t >= settle_until) break;
    } else {
      data = data_ops(pools);
      if (gtav_boot_pool_data_step(&kProfile, &data, &pools->report, &sample)) {
        if (pools->report.status != GTAV_BOOT_POOL_DATA_OBSERVED) break;
        // OBSERVE: keep sampling a little so the other factories and the gap show up too.
        settle_until = t + GTAV_PS4_BOOT_POOLS_OBSERVE_SETTLE_US;
      } else if (t >= deadline) {
        if (mode == GTAV_BOOT_POOL_DATA_MODE_OBSERVE &&
            pools->report.first_window != GTAV_BOOT_POOL_DATA_WINDOW_NONE) {
          return finish(pools, GTAV_BOOT_POOL_DATA_OBSERVED, "deadline");
        }
        return finish(pools, GTAV_BOOT_POOL_DATA_TIMEOUT, "deadline");
      } else {
        // Unreadable samples after a classified one: the factory maps the decision needs cannot
        // be read, and waiting longer only holds the menu back.
        const bool classified = pools->report.first_window != GTAV_BOOT_POOL_DATA_WINDOW_NONE;
        if (classified && sample.window == GTAV_BOOT_POOL_DATA_WINDOW_UNREADABLE) {
          if (!unreadable_since) unreadable_since = t;
          if (t - unreadable_since >= GTAV_PS4_BOOT_POOLS_UNREADABLE_US) {
            return finish(pools, GTAV_BOOT_POOL_DATA_TIMEOUT, "unreadable");
          }
        } else {
          unreadable_since = 0;
        }
        // Story Mode ready: every boot window closed long ago.
        if (config->story_mode && t - ready_checked >= GTAV_PS4_BOOT_POOLS_READY_CHECK_US) {
          ready_checked = t;
          if (config->story_mode(config->stop_context)) {
            return finish(pools,
                          mode == GTAV_BOOT_POOL_DATA_MODE_OBSERVE && classified
                              ? GTAV_BOOT_POOL_DATA_OBSERVED
                              : GTAV_BOOT_POOL_DATA_MISSED,
                          "story-mode");
          }
        }
      }
    }
    // OBSERVE keeps 1 ms: it is there to measure the boot.
    const bool late = mode != GTAV_BOOT_POOL_DATA_MODE_OBSERVE &&
                      pools->report.high_window >= GTAV_BOOT_POOL_DATA_WINDOW_CAPS_ONLY;
    pause(pools, late ? GTAV_PS4_BOOT_POOLS_LATE_SAMPLE_US : GTAV_PS4_BOOT_POOLS_SAMPLE_US);
  }
  return finish(pools, GTAV_BOOT_POOL_DATA_PENDING, NULL);
}

int gtav_ps4_boot_pools_poll(GtavPs4BootPools* pools) {
  if (!pools->runtime || !pools->readable) return 1;
  if (pools->stores_logged && pools->factories_logged) return 1;
  const uint64_t t = now(pools);
  char line[kResultBytes];
  if (!pools->stores_logged) {
    if ((pools->seen & GTAV_PS4_BOOT_POOLS_BOUND_SIZED) == 0) {
      uint8_t sized = 0;
      GtavBootPoolDataOps data = data_ops(pools);
      if (data.read(data.context, kProfile.first_store_sized, &sized, sizeof(sized)) == 0 &&
          sized) {
        pools->seen |= GTAV_PS4_BOOT_POOLS_BOUND_SIZED;
        pools->sized_us = t;
      }
    }
    if ((pools->seen & GTAV_PS4_BOOT_POOLS_BOUND_SIZED) != 0 &&
        t - pools->sized_us >= GTAV_PS4_BOOT_POOLS_STORE_VERIFY_DELAY_US) {
      // Each store reads its node once, when its Init runs: wait for every Init (PtFx comes
      // later, in InitStoresB) so a pending store is not reported at its old size.
      GtavBootPoolDataVerify verify;
      GtavBootPoolDataOps data = data_ops(pools);
      const int read_all = gtav_boot_pool_data_verify_growth(
                               &kProfile, &data, GTAV_BOOT_POOL_DATA_VERIFY_STORES, &verify) == 0;
      if (read_all || t - pools->sized_us >= GTAV_PS4_BOOT_POOLS_STORE_VERIFY_DEADLINE_US) {
        gtav_boot_pool_data_format_verify(&kProfile, &verify, line, sizeof(line));
        log_wrapped(pools, line);
        pools->stores_logged = 1;
        reconcile(pools, &verify);
      }
    }
  }
  if (!pools->factories_logged) {
    GtavBootPoolDataVerify verify;
    GtavBootPoolDataOps data = data_ops(pools);
    if (gtav_boot_pool_data_verify_growth(&kProfile, &data, GTAV_BOOT_POOL_DATA_VERIFY_FACTORIES,
                                          &verify) == 0) {
      gtav_boot_pool_data_format_verify(&kProfile, &verify, line, sizeof(line));
      log_wrapped(pools, line);
      pools->factories_logged = 1;
    }
  }
  return pools->stores_logged && pools->factories_logged;
}
