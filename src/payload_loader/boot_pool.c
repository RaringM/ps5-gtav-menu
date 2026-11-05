// Boot-time pool growth for one fresh game instance (gtavmenu/boot_pool.h).
//
// Same ptrace-free primitives as the cave bootstrap: no-stop reads, kernel_proc_copyin writes
// (gtav_proc_write_nostop), vm_map protection changes. Between process start and the
// CGame_LoadGameConfig call (about 400 ms on 01.011.001) the call is pointed at a stub in the .text
// tail cave; the stub runs the parse and applies the guarded override table on the game thread.
// The call bytes and the cave are always restored, except that a cave the game thread has entered
// but not left is never cleared: that would crash it on return.

#define _DEFAULT_SOURCE 1  // usleep under -std=c11 (host tests)

#include "gtavmenu/boot_pool.h"

#include <string.h>
#include <unistd.h>

#include "gtavmenu/cave_bootstrap.h"
#include "gtavmenu/log.h"
#include "gtavmenu/proc_backend.h"

#define BOOT_POOL_PROT_RX 5
#define BOOT_POOL_PROT_RWX 7
#define BOOT_POOL_WRITE_CHUNK 256u

// Bounded by poll counts, not wall time, so every wait has a fixed upper bound.
#ifndef GTAV_BOOT_POOL_POLL_USEC
#define GTAV_BOOT_POOL_POLL_USEC 10000u
#endif
#ifndef GTAV_BOOT_POOL_STUB_POLLS
#define GTAV_BOOT_POOL_STUB_POLLS 6000u  // 60 s for the game thread to reach the stub
#endif
#ifndef GTAV_BOOT_POOL_GRACE_POLLS
#define GTAV_BOOT_POOL_GRACE_POLLS 60000u  // 10 min for an entered stub to return
#endif

enum { STUB_IDLE = 0, STUB_DONE = 1, STUB_BUSY = 2, STUB_GONE = 3, STUB_STOPPED = 4 };

// The hooks are asked once a second of polling.
#define GTAV_BOOT_POOL_HOOK_POLLS (1000000u / GTAV_BOOT_POOL_POLL_USEC)

// STUB_GONE when the target is no longer the game, STUB_STOPPED when a stop was asked, else 0.
static int hook_verdict(const GtavBootPoolHooks* hooks, int pid, uint32_t poll) {
  if (!hooks || poll % GTAV_BOOT_POOL_HOOK_POLLS != 0) return 0;
  if (hooks->live && !hooks->live(hooks->context, pid)) return STUB_GONE;
  if (hooks->stop && hooks->stop(hooks->context)) return STUB_STOPPED;
  return 0;
}

static int write_verified(int pid, uint64_t addr, const uint8_t* buf, uint32_t n,
                          const char* what) {
  uint32_t off = 0;
  while (off < n) {
    const uint32_t chunk = n - off < BOOT_POOL_WRITE_CHUNK ? n - off : BOOT_POOL_WRITE_CHUNK;
    if (gtav_cave_write_verified(pid, (uintptr_t)(addr + off), buf + off, chunk, what) != 0)
      return -1;
    off += chunk;
  }
  return 0;
}

// done (set last) and entered (set first) are adjacent bytes: one read sees both. A stub that was
// entered but has not finished keeps the game thread inside LoadGameConfig; wait it out.
static int wait_stub(int pid, const GtavBootPoolStubLayout* layout, uint32_t polls,
                     uint32_t* waited_ms, const GtavBootPoolHooks* hooks) {
  uint8_t flags[2] = {0, 0};
  uint32_t i;
  for (i = 0;; ++i) {
    if (gtav_proc_read_nostop(pid, (uintptr_t)layout->done_addr, flags, sizeof(flags)) != 0)
      flags[0] = flags[1] = 0;
    if (flags[0]) return STUB_DONE;
    if (i >= polls) break;
    const int verdict = hook_verdict(hooks, pid, i + 1u);
    if (verdict == STUB_GONE) return STUB_GONE;
    if (verdict == STUB_STOPPED) break;  // not entered so far: restore and clear as usual
    usleep(GTAV_BOOT_POOL_POLL_USEC);
    if (waited_ms) *waited_ms += GTAV_BOOT_POOL_POLL_USEC / 1000u;
  }
  if (!flags[1]) return STUB_IDLE;
  gtav_logf("boot-pool: stub entered but not finished; waiting for it to return");
  for (i = 0; i < GTAV_BOOT_POOL_GRACE_POLLS; ++i) {
    usleep(GTAV_BOOT_POOL_POLL_USEC);
    if (gtav_proc_read_nostop(pid, (uintptr_t)layout->done_addr, flags, sizeof(flags)) == 0 &&
        flags[0])
      return STUB_DONE;
    const int verdict = hook_verdict(hooks, pid, i + 1u);
    if (verdict == STUB_GONE) return STUB_GONE;
    if (verdict == STUB_STOPPED) {
      gtav_logf("boot-pool: stop requested while the stub runs; leaving the cave in place");
      return STUB_BUSY;
    }
  }
  return STUB_BUSY;
}

static void read_results(int pid, const GtavBootPoolStubLayout* layout,
                         GtavBootPoolReport* report) {
  uint8_t table[GTAV_BOOT_POOL_ENTRIES_MAX * GTAV_BOOT_POOL_ENTRY_SIZE];
  uint32_t i;
  if (gtav_proc_read_nostop(pid, (uintptr_t)layout->table_addr, table,
                            (size_t)layout->count * GTAV_BOOT_POOL_ENTRY_SIZE) != 0) {
    gtav_logf("boot-pool: entry table readback failed: %s", gtav_proc_last_error());
    return;
  }
  for (i = 0; i < layout->count; ++i) {
    gtav_boot_pool_entry_read(table, i, &report->result[i], &report->old_value[i]);
    if (report->result[i] == GTAV_BOOT_POOL_RESULT_WRITTEN) ++report->written;
  }
}

static int finish(GtavBootPoolReport* report, int status) {
  report->status = status;
  return status;
}

int gtav_boot_pool_apply(int pid, const GtavBootPoolProfile* profile, GtavBootPoolReport* report,
                         const GtavBootPoolHooks* hooks) {
  uint8_t stub[GTAV_BOOT_POOL_STUB_MAX];
  uint8_t live[GTAV_BOOT_POOL_STUB_MAX];
  uint8_t call_patch[8];
  GtavBootPoolEntry entries[GTAV_BOOT_POOL_ENTRIES_MAX];
  GtavBootPoolStubLayout layout;
  uint8_t flag = 0;
  int state = STUB_IDLE;
  int armed = 0;
  uint32_t i;

  if (!report) return GTAV_BOOT_POOL_REFUSED;
  memset(report, 0, sizeof(*report));
  if (!profile || pid <= 0) return finish(report, GTAV_BOOT_POOL_REFUSED);
  report->count = profile->override_count;

  if (gtav_proc_read_nostop(pid, (uintptr_t)profile->config_done_flag, &flag, 1) != 0) {
    gtav_logf("boot-pool: config flag unreadable: %s", gtav_proc_last_error());
    return finish(report, GTAV_BOOT_POOL_REFUSED);
  }
  if (flag) {
    gtav_logf("boot-pool: gameconfig already parsed; this instance keeps its stock pools");
    return finish(report, GTAV_BOOT_POOL_MISSED);
  }
  if (gtav_proc_read_nostop(pid, (uintptr_t)profile->call_site, live,
                            sizeof(profile->call_context)) != 0 ||
      memcmp(live, profile->call_context, sizeof(profile->call_context)) != 0) {
    gtav_logf("boot-pool: LoadGameConfig call site does not match the pins; refusing");
    return finish(report, GTAV_BOOT_POOL_REFUSED);
  }

  const uint32_t count = gtav_boot_pool_entries(profile, entries, GTAV_BOOT_POOL_ENTRIES_MAX);
  const uint32_t cap =
      profile->cave_capacity < sizeof(stub) ? profile->cave_capacity : (uint32_t)sizeof(stub);
  const uint32_t stub_len =
      count ? gtav_boot_pool_stub_build(profile->cave_address, profile->load_game_config,
                                        profile->config_manager, entries, count, stub, cap, &layout)
            : 0;
  if (stub_len == 0 || gtav_boot_pool_call_build(profile->call_site, profile->cave_address,
                                                 call_patch, sizeof(call_patch)) != 5) {
    gtav_logf("boot-pool: stub assembly refused (entries=%u cap=%u)", count, cap);
    return finish(report, GTAV_BOOT_POOL_REFUSED);
  }
  if (gtav_proc_read_nostop(pid, (uintptr_t)profile->cave_address, live, stub_len) != 0) {
    gtav_logf("boot-pool: cave unreadable: %s", gtav_proc_last_error());
    return finish(report, GTAV_BOOT_POOL_REFUSED);
  }
  for (i = 0; i < stub_len; ++i) {
    if (live[i] != 0) {
      gtav_logf("boot-pool: cave not zero-filled at +0x%x; refusing", i);
      return finish(report, GTAV_BOOT_POOL_REFUSED);
    }
  }

  const uint64_t mask = profile->cave_page_mask;
  const uintptr_t page = (uintptr_t)(profile->cave_address & ~mask);
  const size_t span = (size_t)(((profile->cave_address + stub_len + mask) & ~mask) -
                               (profile->cave_address & ~mask));
  if (gtav_cave_protect_retry(pid, page, span, BOOT_POOL_PROT_RWX, "boot-pool cave->rwx") != 0) {
    gtav_cave_protect_retry(pid, page, span, BOOT_POOL_PROT_RX, "boot-pool cave->rx");
    return finish(report, GTAV_BOOT_POOL_REFUSED);
  }

  if (write_verified(pid, profile->cave_address, stub, stub_len, "boot-pool stub") == 0) {
    // From here the game thread may enter the stub at any moment. A failed or mismatched write may
    // still have changed bytes, so the call site is restored either way.
    armed = 1;
    if (write_verified(pid, profile->call_site, call_patch, 5, "boot-pool detour") == 0)
      state = wait_stub(pid, &layout, GTAV_BOOT_POOL_STUB_POLLS, &report->waited_ms, hooks);
  }
  if (state == STUB_GONE) {
    gtav_logf("boot-pool: pid=%d exited while the detour was armed; nothing left to restore", pid);
    return finish(report, GTAV_BOOT_POOL_MISSED);
  }

  if (armed) {
    if (write_verified(pid, profile->call_site, profile->call_bytes, sizeof(profile->call_bytes),
                       "boot-pool call restore") != 0) {
      gtav_logf("boot-pool: FAILED to restore the LoadGameConfig call; the detour may be live");
      return finish(report, GTAV_BOOT_POOL_UNSAFE);
    }
    // The game may have entered between the last poll and the restore: look once more.
    if (state == STUB_IDLE) state = wait_stub(pid, &layout, 0, NULL, NULL);
  }
  if (state == STUB_BUSY) {
    gtav_logf("boot-pool: stub still running; leaving the cave in place");
    return finish(report, GTAV_BOOT_POOL_UNSAFE);
  }
  if (state == STUB_DONE) {
    read_results(pid, &layout, report);
    // done is stored before the stub's pops and ret; let the game thread leave the cave.
    usleep(GTAV_BOOT_POOL_POLL_USEC);
  }

  memset(stub, 0, stub_len);
  if (write_verified(pid, profile->cave_address, stub, stub_len, "boot-pool cave clear") != 0) {
    gtav_logf("boot-pool: FAILED to clear the cave");
    return finish(report, GTAV_BOOT_POOL_UNSAFE);
  }
  if (gtav_cave_protect_retry(pid, page, span, BOOT_POOL_PROT_RX, "boot-pool cave->rx") != 0) {
    gtav_logf("boot-pool: FAILED to restore the cave to RX");
    return finish(report, GTAV_BOOT_POOL_UNSAFE);
  }
  if (state != STUB_DONE) {
    gtav_logf("boot-pool: the game never reached the stub; this instance keeps its stock pools");
    return finish(report, armed ? GTAV_BOOT_POOL_MISSED : GTAV_BOOT_POOL_REFUSED);
  }
  return finish(report, GTAV_BOOT_POOL_APPLIED);
}
